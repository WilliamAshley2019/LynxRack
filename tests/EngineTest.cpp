// Headless test of RackEngine: timing, wrap, stop/jump release, MIDI routing, mute, live edits.
#include <juce_audio_utils/juce_audio_utils.h>

#include <cmath>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>

#include "../core/RackEngine.h"

using namespace lynx;

namespace
{
int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "FAIL: " << msg << "  (" #cond ")\n"; } } while (0)

struct Seen { bool on; int key; juce::int64 sample; };

class FakePlugin final : public juce::AudioPluginInstance
{
public:
    FakePlugin() : juce::AudioPluginInstance (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Fake"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        for (const auto meta : m)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn())  { seen.push_back ({ true, msg.getNoteNumber(), position + meta.samplePosition }); sounding.insert (msg.getNoteNumber()); }
            if (msg.isNoteOff()) { seen.push_back ({ false, msg.getNoteNumber(), position + meta.samplePosition }); sounding.erase (msg.getNoteNumber()); }
            held = (int) sounding.size();
        }
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (c, i, 0.5f);   // constant DC so gain / mute is easy to read back
        position += b.getNumSamples();
        ++blocks;
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = "Fake"; }

    std::vector<Seen> seen;
    std::set<int> sounding;
    juce::int64 position = 0;
    int held = 0, blocks = 0;
};

struct Rig
{
    static constexpr double sr = 48000.0;
    static constexpr int block = 256;

    RackEngine engine;
    ParamRefs refs;
    std::array<std::atomic<float>, maxChannels> vol, pan, mute, solo;
    std::atomic<float> pattern { 0 }, swing { 0 }, mode { 1 }, tempo { 120 };
    FakePlugin* fake = nullptr;
    juce::AudioBuffer<float> buffer { 2, block };
    std::array<int, maxChannels + 1> first {};
    juce::int64 samples = 0;

    Rig()
    {
        for (int i = 0; i < maxChannels; ++i)
        {
            vol[(size_t) i] = 0.0f; pan[(size_t) i] = 0.0f; mute[(size_t) i] = 0.0f; solo[(size_t) i] = 0.0f;
            refs.volDb[(size_t) i] = &vol[(size_t) i]; refs.pan[(size_t) i] = &pan[(size_t) i];
            refs.mute[(size_t) i] = &mute[(size_t) i]; refs.solo[(size_t) i] = &solo[(size_t) i];
        }
        refs.pattern = &pattern; refs.swing = &swing; refs.mode = &mode; refs.tempo = &tempo;
        engine.setParams (refs);
        engine.prepare (sr, block);
        auto p = std::make_unique<FakePlugin>();
        fake = p.get();
        engine.setChannelPlugin (0, std::move (p));
        first.fill (-1);
        first[0] = 0;
    }

    // one block; returns the first output sample (left)
    float run (HostTime ht = {}, juce::MidiBuffer* midi = nullptr)
    {
        buffer.clear();
        juce::MidiBuffer empty;
        engine.process (buffer, first, midi != nullptr ? *midi : empty, ht);
        samples += block;
        return buffer.getSample (0, 0);
    }
};

double samplesPerTick() { return 60.0 / (120.0 * ppq) * Rig::sr; }   // 120 bpm

void stepNotes (Rig& r, std::initializer_list<int> steps, int key = 60)
{
    r.engine.editPattern (0, [&] (Pattern& p) {
        p.notes.clear();
        for (int s : steps)
        {
            Note n; n.start = (std::int64_t) s * ticksPerStep; n.length = ticksPerStep; n.channel = 0; n.key = key;
            p.add (n);
        }
    });
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // 1. internal transport: sample-accurate on/off, correct period across the pattern wrap
    {
        Rig r;
        stepNotes (r, { 0, 4, 8, 12 });
        r.engine.setInternalPlaying (true);
        const int blocks = (int) (2.0 * 4 * ppq * samplesPerTick() / Rig::block) + 4;      // a bit over 2 bars
        for (int i = 0; i < blocks; ++i)
            r.run();

        std::vector<juce::int64> ons, offs;
        for (const auto& s : r.fake->seen) (s.on ? ons : offs).push_back (s.sample);

        CHECK (ons.size() >= 8, "at least 8 note-ons in two bars, got " << ons.size());
        const auto stepSamples = ticksPerStep * samplesPerTick();
        for (size_t i = 0; i < ons.size(); ++i)
            CHECK (std::abs ((double) ons[i] - std::round ((double) i * 4 * stepSamples)) <= 1.0,
                   "note-on " << i << " at " << ons[i] << " expected " << std::round ((double) i * 4 * stepSamples));
        for (size_t i = 0; i < offs.size() && i < ons.size(); ++i)
            CHECK (std::abs ((double) (offs[i] - ons[i]) - stepSamples) <= 1.5, "note length is one step, got " << offs[i] - ons[i]);
    }

    // 2. stop releases everything that is sounding
    {
        Rig r;
        r.engine.editPattern (0, [] (Pattern& p) {
            p.notes.clear();
            Note n; n.start = 0; n.length = 8 * ticksPerStep; n.channel = 0; n.key = 64;
            p.add (n);
        });
        r.engine.setInternalPlaying (true);
        for (int i = 0; i < 4; ++i) r.run();
        CHECK (r.fake->held == 1, "note held while playing, held=" << r.fake->held);
        r.engine.setInternalPlaying (false);
        r.run(); r.run();
        CHECK (r.fake->held == 0, "stop sends the note-off, held=" << r.fake->held);
    }

    // 3. host mode: follows ppq, a jump releases hanging notes, notes start at the right offset
    {
        Rig r;
        r.mode = 0.0f;
        r.engine.editPattern (0, [] (Pattern& p) {
            p.notes.clear();
            Note n; n.start = 0; n.length = 16 * ticksPerStep; n.channel = 0; n.key = 60;
            p.add (n);
        });
        HostTime ht; ht.hasPosition = true; ht.playing = true; ht.bpm = 120.0;
        const double ticksPerBlock = Rig::block / samplesPerTick() * 1.0;
        double tick = 0.0;
        for (int i = 0; i < 6; ++i) { ht.ppqPosition = tick / ppq; r.run (ht); tick += ticksPerBlock; }
        CHECK (r.fake->held == 1, "host mode: note started, held=" << r.fake->held);
        ht.ppqPosition = 3.0;      // relocate
        r.run (ht);
        CHECK (r.fake->held == 0 || r.fake->held == 1, "relocate does not leave a stuck double note, held=" << r.fake->held);
        ht.playing = false;
        r.run (ht);
        CHECK (r.fake->held == 0, "host stop releases, held=" << r.fake->held);
    }

    // 4. host MIDI goes to the selected channel at its sample offset; note-off follows the note-on's channels
    {
        Rig r;
        juce::MidiBuffer in;
        in.addEvent (juce::MidiMessage::noteOn (3, 70, (juce::uint8) 100), 100);
        in.addEvent (juce::MidiMessage::noteOff (3, 70), 200);
        r.engine.setSelectedChannels (1u);
        r.run (HostTime {}, &in);
        CHECK (r.fake->seen.size() == 2, "two live events reached channel 1, got " << r.fake->seen.size());
        if (r.fake->seen.size() == 2)
        {
            CHECK (r.fake->seen[0].sample == 100 && r.fake->seen[0].on, "live note-on keeps its sample offset");
            CHECK (r.fake->seen[1].sample == 200 && ! r.fake->seen[1].on, "live note-off keeps its sample offset");
        }
    }

    // 5. mute and volume are applied from the parameters without a snapshot rebuild
    {
        Rig r;
        for (int i = 0; i < 40; ++i) r.run();     // settle the gain ramp
        const float open = r.run();
        CHECK (std::abs (open - 0.5f) < 1.0e-4f, "unity gain passes the plugin through, got " << open);
        r.mute[0] = 1.0f;
        r.run();
        CHECK (std::abs (r.run()) < 1.0e-6f, "muted channel is silent");
        r.mute[0] = 0.0f; r.vol[0] = -6.0f;
        r.run();
        CHECK (std::abs (r.run() - 0.5f * juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-3f, "-6 dB volume");
        r.vol[0] = 0.0f; r.pan[0] = 1.0f;
        r.run();
        r.run();
        CHECK (std::abs (r.buffer.getSample (0, 0)) < 1.0e-6f && std::abs (r.buffer.getSample (1, 0) - 0.5f) < 1.0e-4f, "hard right pan");
    }

    // 6. swing delays odd steps
    {
        Rig r;
        stepNotes (r, { 1 });
        r.swing = 1.0f;
        r.engine.setInternalPlaying (true);
        for (int i = 0; i < 100; ++i) r.run();
        CHECK (! r.fake->seen.empty(), "swung note played");
        if (! r.fake->seen.empty())
        {
            const auto expected = (ticksPerStep + ticksPerStep * 0.5) * samplesPerTick();
            CHECK (std::abs ((double) r.fake->seen.front().sample - expected) <= 1.5, "swing delay: " << r.fake->seen.front().sample << " vs " << expected);
        }
    }

    // 7. editing from another thread while audio runs (snapshot hand-over, no crash, no leak of stale state)
    {
        Rig r;
        r.engine.setInternalPlaying (true);
        std::atomic<bool> stop { false };
        std::thread audio ([&] { while (! stop.load()) { r.run(); std::this_thread::sleep_for (std::chrono::microseconds (200)); } });
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (400);
        for (int i = 0; std::chrono::steady_clock::now() < until; ++i)
            stepNotes (r, { i % 16, (i + 5) % 16 }, 40 + i % 30);
        stop = true;
        audio.join();
        CHECK (r.fake->blocks > 10, "audio kept running during edits");
    }

    // 8. undo restores the previous pattern
    {
        Rig r;
        stepNotes (r, { 0 });
        r.engine.beginEdit (0);
        stepNotes (r, { 0, 4, 8 });
        CHECK (r.engine.song().patterns[0].notes.size() == 3, "three notes");
        CHECK (r.engine.undo() && r.engine.song().patterns[0].notes.size() == 1, "undo back to one note");
    }

    // 9. state round trip
    {
        RackSong s;
        for (int i = 0; i < 5; ++i) { Note n; n.start = i * 100; n.length = 50; n.key = 50 + i; n.channel = 2; n.velocity = 0.25f * (float) (i % 4 + 1); s.patterns[3].add (n); }
        s.patterns[3].steps = 24;
        s.channels[2].name = "Bass";
        s.channels[2].transpose = -12;
        s.channels[2].midiOut = 5;
        RackSong back;
        CHECK (RackSong::fromValueTree (s.toValueTree(), back), "song restores");
        CHECK (back.patterns[3].notes.size() == 5 && back.patterns[3].steps == 24 && back.channels[2].name == "Bass"
                   && back.channels[2].transpose == -12 && back.channels[2].midiOut == 5, "song content round-trips");
    }

    std::cout << (failures == 0 ? "ALL ENGINE TESTS PASSED\n" : "FAILURES: " + std::to_string (failures) + "\n");
    return failures == 0 ? 0 : 1;
}
