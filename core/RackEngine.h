#pragma once

// RackEngine: owns the song, the hosted plugin instances, and renders them.
//
// Threading
//   message thread : every non-const method except those marked "any thread" / "audio thread"
//   audio thread   : process() only. It reads an immutable RackSnapshot (published by the message thread
//                    through one atomic pointer) and DAW parameters through std::atomic<float>*. It never
//                    locks, allocates or frees.
//   Fast path      : volume, pan, mute, solo are DAW parameters read directly by process(); turning a
//                    knob never rebuilds a snapshot. Only structural edits (notes, plugins, pattern
//                    length, transpose, MIDI out) publish.
// No worker threads: the plugin lives inside someone else's audio callback.

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>

#include "RackTypes.h"

namespace lynx
{
struct ParamRefs
{
    std::array<std::atomic<float>*, maxChannels> volDb {}, pan {}, mute {}, solo {};
    std::atomic<float>* pattern = nullptr;      // choice index 0..15
    std::atomic<float>* swing = nullptr;        // 0..1
    std::atomic<float>* mode = nullptr;         // 0 = follow host, 1 = internal
    std::atomic<float>* tempo = nullptr;        // internal tempo
};

struct HostTime
{
    bool hasPosition = false, playing = false;
    double ppqPosition = 0.0, bpm = 0.0;
};

// Play head handed to hosted plugins: pattern-local position, so tempo-synced plugins follow the loop.
class RackPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        p.setPpqPosition (ppqPos);
        p.setIsPlaying (playing);
        p.setTimeSignature (TimeSignature { 4, 4 });
        p.setTimeInSamples (samples);
        return p;
    }

    double bpm = 120.0, ppqPos = 0.0;
    bool playing = false;
    juce::int64 samples = 0;
};

class RackEngine final : public juce::ChangeBroadcaster
{
public:
    RackEngine();
    ~RackEngine() override;

    // --- message thread ------------------------------------------------------------------------
    void setParams (const ParamRefs& p) noexcept { params = p; }
    void prepare (double sampleRate, int maxBlock);
    void release();

    const RackSong& song() const noexcept { return songData; }
    void setSong (RackSong&& s);

    void beginEdit (int pattern);                                   // one undo step (call at gesture start)
    void editPattern (int pattern, const std::function<void (Pattern&)>& fn);
    bool undo();
    void editChannel (int channel, const std::function<void (ChannelInfo&)>& fn);

    juce::AudioPluginInstance* channelPlugin (int channel) const noexcept;
    // Publishes `plugin` on the channel. Pass alreadyPrepared = true when the caller used prepareChild() and applied
    // the plugin's state itself (state must be restored AFTER prepare and BEFORE the audio thread can see it).
    void setChannelPlugin (int channel, std::unique_ptr<juce::AudioPluginInstance> plugin, bool alreadyPrepared = false);
    void prepareChild (juce::AudioPluginInstance& plugin);          // playhead + bus layout + prepareToPlay at the engine's rate
    double sampleRateNow() const noexcept { return sampleRate; }
    int blockSizeNow() const noexcept { return maxBlock; }
    void captureStates();                                           // hosted plugin state -> song

    // --- any thread ----------------------------------------------------------------------------
    void liveNote (int channel, int key, float velocity, bool on) noexcept;   // UI preview
    void setSelectedChannels (std::uint32_t mask) noexcept { selectedMask.store (mask); }
    void setInternalPlaying (bool shouldPlay) noexcept { internalPlaying.store (shouldPlay); }
    bool isInternalPlaying() const noexcept { return internalPlaying.load(); }
    void rewindInternal() noexcept { rewindRequested.store (true); }
    void panic() noexcept { panicRequested.store (true); }

    double playheadTick() const noexcept { return playhead.load (std::memory_order_relaxed); }
    bool isPlayingNow() const noexcept { return playingNow.load (std::memory_order_relaxed); }
    double currentBpm() const noexcept { return bpmNow.load (std::memory_order_relaxed); }
    float takePeak (int channel) noexcept { return peaks[(size_t) channel].exchange (0.0f, std::memory_order_relaxed); }
    bool hasCrashed (int channel) const noexcept { return crashed[(size_t) channel].load (std::memory_order_relaxed); }

    // --- audio thread --------------------------------------------------------------------------
    // `buffer`: the whole process buffer. firstChannel[0] = main bus, [1 + i] = channel i's own bus; -1 = bus
    // disabled (the channel then goes to the main bus). `midi`: host MIDI in; replaced by MIDI out.
    void process (juce::AudioBuffer<float>& buffer, const std::array<int, maxChannels + 1>& firstChannel,
                  juce::MidiBuffer& midi, const HostTime& host);

private:
    struct RtPattern
    {
        int steps = 16;
        std::vector<Note> notes;
    };

    struct RackSnapshot
    {
        struct Chan
        {
            std::shared_ptr<juce::AudioPluginInstance> plugin;
            int midiOut = 0, transpose = 0;
        };
        std::array<Chan, maxChannels> channels;
        std::array<std::shared_ptr<const RtPattern>, maxPatterns> patterns;
        std::uint64_t generation = 0;
    };

    struct LiveEvent
    {
        std::uint8_t channel = 0, key = 0, on = 0;
        float velocity = 0.0f;
    };

    void rebuildPattern (int p);
    void publish();
    void reap();
    void preparePlugin (juce::AudioPluginInstance&);

    void sendNote (int ch, int key, float vel, bool on, int offset) noexcept;
    void sendRaw (int ch, const juce::MidiMessage&, int offset) noexcept;
    void releaseAll() noexcept;
    void schedule (const RtPattern&, const RackSnapshot&, double startTick, int n, double tps, float swing) noexcept;

    // message-thread state
    RackSong songData;
    std::array<std::shared_ptr<juce::AudioPluginInstance>, maxChannels> plugins;
    std::array<std::shared_ptr<const RtPattern>, maxPatterns> rtPatterns;
    std::deque<std::pair<int, Pattern>> undoStack;
    std::vector<std::unique_ptr<RackSnapshot>> alive;
    std::uint64_t generation = 0;
    double sampleRate = 44100.0;
    int maxBlock = 512;
    ParamRefs params;
    RackPlayHead playHead;

    // shared
    std::atomic<RackSnapshot*> pending { nullptr };
    std::atomic<std::uint64_t> acked { 0 };
    std::atomic<std::uint32_t> selectedMask { 1 };
    std::atomic<bool> internalPlaying { false }, rewindRequested { false }, panicRequested { false };
    std::atomic<double> playhead { 0.0 }, bpmNow { 120.0 };
    std::atomic<bool> playingNow { false };
    std::array<std::atomic<float>, maxChannels> peaks {};
    std::array<std::atomic<bool>, maxChannels> crashed {};

    // single-producer single-consumer ring: UI -> audio
    static constexpr int liveCapacity = 256;
    std::array<LiveEvent, liveCapacity> liveRing {};
    std::atomic<int> liveWrite { 0 }, liveRead { 0 };

    // audio-thread state
    struct Rt
    {
        RackSnapshot* current = nullptr;
        double expectedTick = 0.0, internalTick = 0.0;
        bool wasPlaying = false;
        int lastPattern = -1;
        std::array<juce::AudioBuffer<float>, maxChannels> chanBuf;
        std::array<juce::MidiBuffer, maxChannels> chanMidi;
        juce::MidiBuffer outMidi;
        std::array<std::array<std::uint64_t, 2>, maxChannels> sounding {};   // keys currently on, per channel
        std::array<std::uint32_t, 128> heldTargets {};                      // host MIDI note -> channels it started
        std::array<float, maxChannels> gainL {}, gainR {};
        std::array<int, maxChannels> quietSamples {};
        struct Ev { int offset; std::int8_t on; std::uint8_t ch, key; float vel; };
        static constexpr int evCapacity = 4096;
        std::array<Ev, evCapacity> events {};
        int eventCount = 0;
        juce::int64 samplePos = 0;
    } rt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackEngine)
};
} // namespace lynx
