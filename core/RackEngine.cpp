#include "RackEngine.h"

#include <cmath>

namespace lynx
{
namespace
{
constexpr int scratchChannels = 8;          // per hosted plugin; its buses are reduced to a stereo main output
constexpr int minimumBlock = 8192;          // scratch size, so oversized host blocks still fit
constexpr float quietThreshold = 1.0e-5f;   // about -100 dBFS

// A hosted plugin that raises a hardware exception (Windows) or throws must not take the DAW down with it.
bool guardedProcess (juce::AudioProcessor& p, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) noexcept
{
#if JUCE_WINDOWS && JUCE_MSVC
    __try
    {
        p.processBlock (b, m);
        return true;
    }
    __except (1)
    {
        return false;
    }
#else
    try
    {
        p.processBlock (b, m);
        return true;
    }
    catch (...)
    {
        return false;
    }
#endif
}

inline juce::MidiMessage noteMessage (int key, float velocity, bool on)
{
    if (! on)
        return juce::MidiMessage::noteOff (1, key, 0.0f);

    return juce::MidiMessage::noteOn (1, key, (juce::uint8) std::max (1, (int) std::lround (velocity * 127.0f)));
}
} // namespace

RackEngine::RackEngine()
{
    for (int i = 0; i < maxPatterns; ++i)
        rebuildPattern (i);

    for (auto& c : crashed)
        c.store (false);

    publish();
}

RackEngine::~RackEngine()
{
    rt.current = nullptr;
    pending.store (nullptr);
    alive.clear();
    for (auto& p : plugins)
        p.reset();
}

// --- message thread ------------------------------------------------------------------------------

void RackEngine::prepare (double sr, int blockSize)
{
    sampleRate = sr;
    maxBlock = std::max (blockSize, minimumBlock);

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        rt.chanBuf[(size_t) ch].setSize (scratchChannels, maxBlock, false, true, false);
        rt.chanMidi[(size_t) ch].ensureSize (4096);
        rt.gainL[(size_t) ch] = rt.gainR[(size_t) ch] = 0.0f;
        rt.quietSamples[(size_t) ch] = 0;
    }

    rt.outMidi.ensureSize (4096);
    rt.sounding = {};
    rt.heldTargets = {};
    rt.wasPlaying = false;
    rt.lastPattern = -1;

    for (auto& p : plugins)
        if (p != nullptr)
            preparePlugin (*p);

    publish(); // the audio thread starts from a fresh snapshot
}

void RackEngine::release()
{
    rt.current = nullptr;
    acked.store (generation, std::memory_order_release);

    for (auto& p : plugins)
        if (p != nullptr)
            p->releaseResources();

    reap();
}

void RackEngine::preparePlugin (juce::AudioPluginInstance& p)
{
    p.releaseResources();

    const auto original = p.getBusesLayout();
    auto wanted = original;

    for (int i = 0; i < wanted.inputBuses.size(); ++i)
        wanted.getChannelSet (true, i) = juce::AudioChannelSet::disabled();

    for (int i = 0; i < wanted.outputBuses.size(); ++i)
        wanted.getChannelSet (false, i) = i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();

    if (! p.setBusesLayout (wanted))
    {
        auto stereoOnly = original;
        if (stereoOnly.outputBuses.size() > 0)
            stereoOnly.getChannelSet (false, 0) = juce::AudioChannelSet::stereo();
        p.setBusesLayout (stereoOnly);
    }

    p.setRateAndBufferSizeDetails (sampleRate, maxBlock);
    p.prepareToPlay (sampleRate, maxBlock);
}

void RackEngine::rebuildPattern (int p)
{
    const auto& src = songData.patterns[(size_t) p];
    auto out = std::make_shared<RtPattern>();
    out->steps = src.steps;
    const auto limit = src.lengthTicks();

    out->notes.reserve (src.notes.size());
    for (const auto& n : src.notes)
        if (n.start < limit && ! n.muted)
            out->notes.push_back (n);

    rtPatterns[(size_t) p] = std::move (out);
}

void RackEngine::publish()
{
    auto snap = std::make_unique<RackSnapshot>();
    snap->generation = ++generation;
    snap->patterns = rtPatterns;

    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = snap->channels[(size_t) i];
        c.plugin = plugins[(size_t) i];
        c.midiOut = songData.channels[(size_t) i].midiOut;
        c.transpose = songData.channels[(size_t) i].transpose;
    }

    auto* raw = snap.get();
    alive.push_back (std::move (snap));

    // A snapshot the audio thread never took can be dropped at once: only this thread ever saw it.
    if (auto* old = pending.exchange (raw, std::memory_order_acq_rel))
        alive.erase (std::remove_if (alive.begin(), alive.end(), [old] (auto& s) { return s.get() == old; }), alive.end());

    reap();
}

void RackEngine::reap()
{
    const auto a = acked.load (std::memory_order_acquire);
    auto* pend = pending.load (std::memory_order_acquire);

    alive.erase (std::remove_if (alive.begin(), alive.end(),
                                 [a, pend] (auto& s) { return s->generation < a && s.get() != pend; }),
                 alive.end());
}

void RackEngine::setSong (RackSong&& s)
{
    songData = std::move (s);
    undoStack.clear();

    for (auto& p : plugins)
        p.reset();
    for (auto& c : crashed)
        c.store (false);
    for (int i = 0; i < maxPatterns; ++i)
        rebuildPattern (i);

    publish();
    sendChangeMessage();
}

void RackEngine::beginEdit (int pattern)
{
    if (! juce::isPositiveAndBelow (pattern, maxPatterns))
        return;

    undoStack.emplace_back (pattern, songData.patterns[(size_t) pattern]);
    if (undoStack.size() > 100)
        undoStack.pop_front();
}

void RackEngine::editPattern (int pattern, const std::function<void (Pattern&)>& fn)
{
    if (! juce::isPositiveAndBelow (pattern, maxPatterns))
        return;

    auto& pat = songData.patterns[(size_t) pattern];
    fn (pat);
    pat.steps = juce::jlimit (1, maxSteps, pat.steps);
    pat.sortNotes();
    rebuildPattern (pattern);
    publish();
    sendChangeMessage();
}

bool RackEngine::undo()
{
    if (undoStack.empty())
        return false;

    auto item = std::move (undoStack.back());
    undoStack.pop_back();
    songData.patterns[(size_t) item.first] = std::move (item.second);
    rebuildPattern (item.first);
    publish();
    sendChangeMessage();
    return true;
}

void RackEngine::editChannel (int channel, const std::function<void (ChannelInfo&)>& fn)
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return;

    fn (songData.channels[(size_t) channel]);
    publish();
    sendChangeMessage();
}

juce::AudioPluginInstance* RackEngine::channelPlugin (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxChannels) ? plugins[(size_t) channel].get() : nullptr;
}

void RackEngine::prepareChild (juce::AudioPluginInstance& p)
{
    p.setPlayHead (&playHead);
    p.setNonRealtime (false);
    preparePlugin (p);
}

void RackEngine::setChannelPlugin (int channel, std::unique_ptr<juce::AudioPluginInstance> plugin, bool alreadyPrepared)
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return;

    if (plugin != nullptr && ! alreadyPrepared)
        prepareChild (*plugin);

    // Whoever drops the last reference (always the message thread, via reap()) shuts the plugin down cleanly first:
    // no processing, no dangling play-head pointer into this engine, resources released, then destroy.
    plugins[(size_t) channel] = plugin == nullptr ? nullptr
        : std::shared_ptr<juce::AudioPluginInstance> (plugin.release(), [] (juce::AudioPluginInstance* p) {
              p->suspendProcessing (true);
              p->setPlayHead (nullptr);
              p->releaseResources();
              delete p;
          });
    crashed[(size_t) channel].store (false);
    publish();
    sendChangeMessage();
}

void RackEngine::captureStates()
{
    for (int i = 0; i < maxChannels; ++i)
        if (auto* p = plugins[(size_t) i].get(); p != nullptr && ! crashed[(size_t) i].load())
        {
            juce::MemoryBlock mb;
            p->getStateInformation (mb);
            songData.channels[(size_t) i].pluginState = std::move (mb);
        }
}

// --- any thread ----------------------------------------------------------------------------------

void RackEngine::liveNote (int channel, int key, float velocity, bool on) noexcept
{
    const auto w = liveWrite.load (std::memory_order_relaxed);
    const auto next = (w + 1) % liveCapacity;

    if (next == liveRead.load (std::memory_order_acquire))
        return;

    liveRing[(size_t) w] = { (std::uint8_t) juce::jlimit (0, maxChannels - 1, channel), (std::uint8_t) juce::jlimit (0, 127, key),
                             (std::uint8_t) (on ? 1 : 0), velocity };
    liveWrite.store (next, std::memory_order_release);
}

// --- audio thread --------------------------------------------------------------------------------

void RackEngine::sendRaw (int ch, const juce::MidiMessage& m, int offset) noexcept
{
    rt.chanMidi[(size_t) ch].addEvent (m, offset);

    if (rt.current != nullptr)
        if (const auto out = rt.current->channels[(size_t) ch].midiOut; out > 0)
        {
            auto copy = m;
            copy.setChannel (out);
            rt.outMidi.addEvent (copy, offset);
        }
}

void RackEngine::sendNote (int ch, int key, float vel, bool on, int offset) noexcept
{
    auto& bits = rt.sounding[(size_t) ch][(size_t) (key >> 6)];
    const auto bit = std::uint64_t (1) << (key & 63);
    bits = on ? (bits | bit) : (bits & ~bit);
    sendRaw (ch, noteMessage (key, vel, on), offset);
}

void RackEngine::releaseAll() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        auto& s = rt.sounding[(size_t) ch];

        if (s[0] == 0 && s[1] == 0)
            continue;

        for (int key = 0; key < 128; ++key)
            if ((s[(size_t) (key >> 6)] >> (key & 63)) & 1u)
                sendNote (ch, key, 0.0f, false, 0);

        sendRaw (ch, juce::MidiMessage::allNotesOff (1), 0);
    }

    rt.heldTargets = {};
}

void RackEngine::schedule (const RtPattern& pat, const RackSnapshot& snap, double startTick, int n, double tps, float swing) noexcept
{
    const double L = (double) pat.steps * ticksPerStep;
    constexpr double eps = 1.0e-6;
    const double a = startTick - eps, b = startTick + n * tps - eps;
    const double swingTicks = (double) swing * ticksPerStep * 0.5;

    const auto emit = [&] (double t, bool on, int ch, int key, float vel) {
        for (double tick = t + std::ceil ((a - t) / L) * L; tick < b; tick += L)
        {
            if (tick < a || rt.eventCount >= Rt::evCapacity)
                continue;

            const auto offset = juce::jlimit (0, n - 1, (int) std::llround ((tick - startTick) / tps));
            rt.events[(size_t) rt.eventCount++] = { offset, (std::int8_t) (on ? 1 : 0), (std::uint8_t) ch, (std::uint8_t) key, vel };
        }
    };

    for (const auto& note : pat.notes)
    {
        double on = (double) note.start;

        if (swingTicks > 0.0 && ((note.start / ticksPerStep) & 1) != 0)
            on += swingTicks;

        const auto length = std::min ((double) std::max<std::int64_t> (note.length, 1), L);
        const auto key = juce::jlimit (0, 127, note.key + snap.channels[(size_t) note.channel].transpose);

        emit (on, true, note.channel, key, note.velocity);
        emit (on + length, false, note.channel, key, 0.0f);
    }
}

void RackEngine::process (juce::AudioBuffer<float>& buffer, const std::array<int, maxChannels + 1>& firstChannel,
                          juce::MidiBuffer& midi, const HostTime& host)
{
    if (auto* incoming = pending.exchange (nullptr, std::memory_order_acq_rel))
    {
        rt.current = incoming;
        acked.store (incoming->generation, std::memory_order_release);
    }

    const int n = buffer.getNumSamples();
    auto* snap = rt.current;

    if (snap == nullptr || n <= 0 || n > maxBlock)
    {
        midi.clear();
        return;
    }

    for (auto& m : rt.chanMidi)
        m.clear();
    rt.outMidi.clear();
    rt.eventCount = 0;

    // --- time base -------------------------------------------------------------------------------
    const int patIdx = params.pattern != nullptr ? juce::jlimit (0, maxPatterns - 1, (int) params.pattern->load()) : 0;
    const auto* pat = snap->patterns[(size_t) patIdx].get();
    const float swing = params.swing != nullptr ? params.swing->load() : 0.0f;
    const double tempoParam = params.tempo != nullptr ? (double) params.tempo->load() : 120.0;
    const bool follow = params.mode == nullptr || params.mode->load() < 0.5f;
    const bool useHost = follow && host.hasPosition;
    const double bpm = useHost && host.bpm > 0.0 ? host.bpm : tempoParam;
    const double tps = bpm * ppq / (60.0 * sampleRate);

    if (rewindRequested.exchange (false))
    {
        rt.internalTick = 0.0;
        releaseAll();
    }

    if (panicRequested.exchange (false))
        releaseAll();

    bool playing = useHost ? host.playing : internalPlaying.load (std::memory_order_relaxed);
    const double startTick = useHost ? host.ppqPosition * ppq : rt.internalTick;
    playing = playing && startTick >= 0.0 && pat != nullptr;

    const double L = pat != nullptr ? (double) pat->steps * ticksPerStep : (double) (16 * ticksPerStep);

    // A stop, a pattern change or a position jump (loop, relocate) ends the notes that are sounding.
    const bool jumped = playing && rt.wasPlaying && std::abs (startTick - rt.expectedTick) > std::max (2.0, 2.0 * tps);
    if ((rt.wasPlaying && ! playing) || (rt.lastPattern >= 0 && rt.lastPattern != patIdx) || jumped)
        releaseAll();
    rt.lastPattern = patIdx;

    playHead.bpm = bpm;
    playHead.ppqPos = std::fmod (std::max (0.0, startTick), L) / ppq;
    playHead.playing = playing;
    playHead.samples = rt.samplePos;
    rt.samplePos += n;

    if (playing)
    {
        schedule (*pat, *snap, startTick, n, tps, swing);

        // off before on on the same sample, so a repeated note is not cut; insertion sort, nearly ordered already
        const auto before = [] (const Rt::Ev& x, const Rt::Ev& y) {
            return x.offset != y.offset ? x.offset < y.offset : (x.on == 0 && y.on != 0);
        };
        for (int i = 1; i < rt.eventCount; ++i)
        {
            const auto e = rt.events[(size_t) i];
            int j = i;
            while (j > 0 && before (e, rt.events[(size_t) j - 1]))
            {
                rt.events[(size_t) j] = rt.events[(size_t) j - 1];
                --j;
            }
            rt.events[(size_t) j] = e;
        }

        for (int i = 0; i < rt.eventCount; ++i)
        {
            const auto& e = rt.events[(size_t) i];
            sendNote (e.ch, e.key, e.vel, e.on != 0, e.offset);
        }

        rt.expectedTick = startTick + n * tps;
        if (! useHost)
            rt.internalTick += n * tps;
    }

    rt.wasPlaying = playing;
    playhead.store (std::fmod (std::max (0.0, startTick), L), std::memory_order_relaxed);
    playingNow.store (playing, std::memory_order_relaxed);
    bpmNow.store (bpm, std::memory_order_relaxed);

    // --- live input: host MIDI goes to the selected channels, sample-accurate ----------------------
    const auto mask = selectedMask.load (std::memory_order_relaxed);

    for (const auto meta : midi)
    {
        if (meta.numBytes > 3)
            continue;

        const auto msg = meta.getMessage();
        const int offset = juce::jlimit (0, n - 1, meta.samplePosition);

        if (msg.isNoteOn())
        {
            const int key = msg.getNoteNumber();

            for (int ch = 0; ch < maxChannels; ++ch)
                if ((rt.heldTargets[(size_t) key] >> ch) & 1u)
                    sendNote (ch, key, 0.0f, false, offset);

            rt.heldTargets[(size_t) key] = mask;

            for (int ch = 0; ch < maxChannels; ++ch)
                if ((mask >> ch) & 1u)
                    sendNote (ch, key, msg.getFloatVelocity(), true, offset);
        }
        else if (msg.isNoteOff())
        {
            const int key = msg.getNoteNumber();
            const auto targets = rt.heldTargets[(size_t) key] != 0 ? rt.heldTargets[(size_t) key] : mask;
            rt.heldTargets[(size_t) key] = 0;

            for (int ch = 0; ch < maxChannels; ++ch)
                if ((targets >> ch) & 1u)
                    sendNote (ch, key, 0.0f, false, offset);
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            releaseAll();
        }
        else if (msg.isController() || msg.isPitchWheel() || msg.isAftertouch() || msg.isChannelPressure() || msg.isProgramChange())
        {
            auto m = msg;
            m.setChannel (1);

            for (int ch = 0; ch < maxChannels; ++ch)
                if ((mask >> ch) & 1u)
                    sendRaw (ch, m, offset);
        }
    }

    // --- live input: UI previews ------------------------------------------------------------------
    for (auto r = liveRead.load (std::memory_order_relaxed); r != liveWrite.load (std::memory_order_acquire);)
    {
        const auto& e = liveRing[(size_t) r];
        sendNote (e.channel, e.key, e.velocity, e.on != 0, 0);
        r = (r + 1) % liveCapacity;
        liveRead.store (r, std::memory_order_release);
    }

    // --- render the hosted plugins ----------------------------------------------------------------
    bool anySolo = false;
    for (auto* s : params.solo)
        anySolo = anySolo || (s != nullptr && s->load() >= 0.5f);

    const int quietLimit = (int) (3.0 * sampleRate);

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        auto* plugin = snap->channels[(size_t) ch].plugin.get();
        auto& midiIn = rt.chanMidi[(size_t) ch];

        if (plugin == nullptr || crashed[(size_t) ch].load (std::memory_order_relaxed))
            continue;

        const int needed = std::max ({ 2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels() });
        if (needed > scratchChannels)
            continue;

        // an instrument that has been silent for a while and has nothing to play is not run
        if (midiIn.isEmpty() && rt.quietSamples[(size_t) ch] > quietLimit)
        {
            rt.gainL[(size_t) ch] = rt.gainR[(size_t) ch] = 0.0f;
            continue;
        }

        juce::AudioBuffer<float> view (rt.chanBuf[(size_t) ch].getArrayOfWritePointers(), needed, n);
        view.clear();

        if (! guardedProcess (*plugin, view, midiIn))
        {
            crashed[(size_t) ch].store (true, std::memory_order_relaxed);
            continue;
        }

        const float magnitude = view.getMagnitude (0, n);
        rt.quietSamples[(size_t) ch] = magnitude < quietThreshold ? std::min (rt.quietSamples[(size_t) ch] + n, 1 << 30) : 0;
        auto& peak = peaks[(size_t) ch];
        peak.store (std::max (peak.load (std::memory_order_relaxed), magnitude), std::memory_order_relaxed);

        const float db = params.volDb[(size_t) ch] != nullptr ? params.volDb[(size_t) ch]->load() : 0.0f;
        const float pan = params.pan[(size_t) ch] != nullptr ? params.pan[(size_t) ch]->load() : 0.0f;
        const bool muted = params.mute[(size_t) ch] != nullptr && params.mute[(size_t) ch]->load() >= 0.5f;
        const bool solo = params.solo[(size_t) ch] != nullptr && params.solo[(size_t) ch]->load() >= 0.5f;
        const float gain = (muted || (anySolo && ! solo) || db <= -59.9f) ? 0.0f : juce::Decibels::decibelsToGain (db);
        const float targetL = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
        const float targetR = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);

        const int destFirst = firstChannel[(size_t) ch + 1] >= 0 ? firstChannel[(size_t) ch + 1] : firstChannel[0];
        const int srcR = needed > 1 ? 1 : 0;

        if (destFirst >= 0 && destFirst + 1 < buffer.getNumChannels())
        {
            if (rt.gainL[(size_t) ch] > 0.0f || targetL > 0.0f)
                buffer.addFromWithRamp (destFirst, 0, view.getReadPointer (0), n, rt.gainL[(size_t) ch], targetL);
            if (rt.gainR[(size_t) ch] > 0.0f || targetR > 0.0f)
                buffer.addFromWithRamp (destFirst + 1, 0, view.getReadPointer (srcR), n, rt.gainR[(size_t) ch], targetR);
        }

        rt.gainL[(size_t) ch] = targetL;
        rt.gainR[(size_t) ch] = targetR;
    }

    // MIDI out: only what the channels asked to forward
    midi.clear();
    midi.addEvents (rt.outMidi, 0, n, 0);
}
} // namespace lynx
