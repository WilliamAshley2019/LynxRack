#include "RackProcessor.h"

#include "../ui/RackEditor.h"

namespace lynx
{
juce::AudioProcessor::BusesProperties RackProcessor::makeBuses()
{
    auto props = BusesProperties().withOutput ("Main", juce::AudioChannelSet::stereo(), true);

    // One stereo output per channel, off by default: in FL, Reaper and others the host can route each
    // sequenced instrument to its own mixer track. A channel whose bus is off plays into Main.
    for (int i = 0; i < maxChannels; ++i)
        props = props.withOutput ("Ch " + juce::String (i + 1), juce::AudioChannelSet::stereo(), false);

    return props;
}

juce::AudioProcessorValueTreeState::ParameterLayout RackProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    StringArray patterns;
    for (int i = 1; i <= maxPatterns; ++i)
        patterns.add (String (i));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "pattern", 1 }, "Pattern", patterns, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "swing", 1 }, "Swing", NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Transport", StringArray { "Follow host", "Internal" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tempo", 1 }, "Internal tempo", NormalisableRange<float> (20.0f, 300.0f, 0.01f), 120.0f));

    for (int i = 0; i < maxChannels; ++i)
    {
        const auto n = "Ch" + String (i + 1) + " ";
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { volId (i), 1 }, n + "Volume",
                                                           NormalisableRange<float> (-60.0f, 6.0f, 0.1f, 2.5f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("dB")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { panId (i), 1 }, n + "Pan", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { muteId (i), 1 }, n + "Mute", false));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { soloId (i), 1 }, n + "Solo", false));
    }

    return layout;
}

RackProcessor::RackProcessor()
    : juce::AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "LynxRack", makeLayout())
{
    ParamRefs refs;
    refs.pattern = apvts.getRawParameterValue ("pattern");
    refs.swing = apvts.getRawParameterValue ("swing");
    refs.mode = apvts.getRawParameterValue ("mode");
    refs.tempo = apvts.getRawParameterValue ("tempo");

    for (int i = 0; i < maxChannels; ++i)
    {
        refs.volDb[(size_t) i] = apvts.getRawParameterValue (volId (i));
        refs.pan[(size_t) i] = apvts.getRawParameterValue (panId (i));
        refs.mute[(size_t) i] = apvts.getRawParameterValue (muteId (i));
        refs.solo[(size_t) i] = apvts.getRawParameterValue (soloId (i));
    }

    patternParam = refs.pattern;
    tempoParam = refs.tempo;
    engine.setParams (refs);
    engine.setSelectedChannels (1u);
}

RackProcessor::~RackProcessor()
{
    stopTimer();
    host.closeAllEditors();
}

bool RackProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (! layouts.inputBuses.isEmpty() && layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (const auto& bus : layouts.outputBuses)
        if (bus != juce::AudioChannelSet::stereo() && bus != juce::AudioChannelSet::disabled())
            return false;

    return true;
}

void RackProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
}

void RackProcessor::releaseResources()
{
    engine.release();
}

void RackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    HostTime ht;

    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            ht.playing = pos->getIsPlaying();
            if (const auto beats = pos->getPpqPosition())
            {
                ht.hasPosition = true;
                ht.ppqPosition = *beats;
            }
            if (const auto bpm = pos->getBpm())
                ht.bpm = *bpm;
        }

    std::array<int, maxChannels + 1> first {};
    for (int bus = 0; bus <= maxChannels; ++bus)
        first[(size_t) bus] = getBus (false, bus) != nullptr && getBus (false, bus)->isEnabled()
                                  ? getChannelIndexInProcessBlockBuffer (false, bus, 0)
                                  : -1;

    engine.process (buffer, first, midi, ht);
}

juce::AudioProcessorEditor* RackProcessor::createEditor()
{
    return new RackEditor (*this);
}

void RackProcessor::selectChannel (int channel)
{
    selected = juce::jlimit (0, maxChannels - 1, channel);
    engine.setSelectedChannels (1u << selected);
}

void RackProcessor::setModuleLink (std::shared_ptr<IModuleLink> newLink)
{
    link = std::move (newLink);

    if (link != nullptr)
        startTimerHz (20);
    else
        stopTimer();
}

void RackProcessor::timerCallback()
{
    if (link == nullptr)
        return;

    ModuleStatus s;
    s.playing = engine.isPlayingNow();
    s.bpm = engine.currentBpm();
    s.positionTicks = engine.playheadTick();
    s.activePattern = activePattern();

    for (int i = 0; i < maxChannels; ++i)
        s.peaks[(size_t) i] = engine.takePeak (i);

    link->publishStatus (s);

    ModuleCommand c;
    while (link->takeCommand (c))
    {
        switch (c.kind)
        {
            case ModuleCommand::Kind::play: engine.setInternalPlaying (true); break;
            case ModuleCommand::Kind::stop: engine.setInternalPlaying (false); break;
            case ModuleCommand::Kind::rewind: engine.rewindInternal(); break;
            case ModuleCommand::Kind::selectChannel: selectChannel ((int) c.value); break;
            case ModuleCommand::Kind::setPattern:
                if (auto* p = apvts.getParameter ("pattern"))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (0, maxPatterns - 1, (int) c.value)));
                break;
            case ModuleCommand::Kind::setTempo:
                if (auto* p = apvts.getParameter ("tempo"))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) c.value));
                break;
            case ModuleCommand::Kind::none: break;
        }
    }
}

// --- state: DAW parameters + the song (patterns, channels, hosted plugin states) -----------------------

void RackProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    engine.captureStates();

    juce::ValueTree root ("LynxRack");
    root.setProperty ("module", moduleId, nullptr);
    root.setProperty ("schema", schemaVersion, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (engine.song().toValueTree(), nullptr);

    juce::MemoryOutputStream out (destData, false);
    root.writeToStream (out);
}

void RackProcessor::setStateInformation (const void* data, int size)
{
    const auto root = juce::ValueTree::readFromData (data, (size_t) size);

    if (! root.hasType ("LynxRack"))
        return;

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        applyState (root);
    else
        juce::MessageManager::callAsync ([this, root, alive = juce::WeakReference<RackProcessor> (this)] {
            if (alive != nullptr)
                applyState (root);
        });
}

void RackProcessor::applyState (const juce::ValueTree& root)
{
    host.closeAllEditors();

    if (const auto params = root.getChildWithName (apvts.state.getType()); params.isValid())
        apvts.replaceState (params);

    RackSong song;
    if (RackSong::fromValueTree (root.getChildWithName ("Song"), song))
    {
        engine.setSong (std::move (song));
        host.restoreFromSong();
    }
}
} // namespace lynx

// --- plugin entry point --------------------------------------------------------------------------------
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new lynx::RackProcessor();
}
