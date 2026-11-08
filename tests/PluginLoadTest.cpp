// Loads the built VST3 the way a DAW does: scan, instantiate, play MIDI, save/restore state, open the editor.
#include <juce_audio_utils/juce_audio_utils.h>

#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    int failures = 0;
    const auto check = [&] (bool ok, const char* what) { std::cout << (ok ? "ok   " : "FAIL ") << what << "\n"; failures += ok ? 0 : 1; };

    if (argc < 2) { std::cout << "usage: PluginLoadTest <path to .vst3>\n"; return 2; }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, argv[1]);
    check (found.size() == 1, "VST3 is found and described");
    if (found.isEmpty()) return 1;

    const auto& d = *found[0];
    std::cout << "     name=" << d.name << " synth=" << (int) d.isInstrument << " inputs=" << d.numInputChannels << " outputs=" << d.numOutputChannels << "\n";
    check (d.isInstrument, "reported as an instrument");

    juce::String error;
    auto plugin = format.createInstanceFromDescription (d, 48000.0, 512, error);
    check (plugin != nullptr, "instance created");
    if (plugin == nullptr) { std::cout << error << "\n"; return 1; }

    check (plugin->getBusCount (false) == 17, "17 output buses (main + 16 channels)");
    check (plugin->acceptsMidi() && plugin->producesMidi(), "accepts and produces MIDI");

    std::cout << "     total out channels=" << plugin->getTotalNumOutputChannels() << " main=" << plugin->getMainBusNumOutputChannels() << "\n";
    check (plugin->getTotalNumOutputChannels() == 2, "only the stereo main output is active by default");
    plugin->prepareToPlay (48000.0, 512);

    // drive it for a few hundred blocks with live MIDI, internal transport follows host: no play head here
    juce::AudioBuffer<float> buffer (plugin->getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    bool finite = true;
    for (int i = 0; i < 300; ++i)
    {
        buffer.clear();
        midi.clear();
        if (i == 5)  midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 17);
        if (i == 50) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 3);
        plugin->processBlock (buffer, midi);
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            finite = finite && std::isfinite (buffer.getMagnitude (c, 0, 512));
    }
    check (finite, "300 blocks process with finite output");

    // state round trip
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    check (state.getSize() > 100, "state saved");
    auto second = format.createInstanceFromDescription (d, 48000.0, 512, error);
    check (second != nullptr, "second instance created");
    if (second != nullptr)
    {
        second->setStateInformation (state.getData(), (int) state.getSize());
        juce::MemoryBlock again;
        second->getStateInformation (again);
        check (again.getSize() > 100, "state restored into a new instance and saved again");
        second->prepareToPlay (48000.0, 512);
        second->releaseResources();
    }

    // parameters visible to the DAW
    check (plugin->getParameters().size() >= 4 + 16 * 4, "DAW parameters exposed");

    // editor
    std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorIfNeeded());
    check (editor != nullptr, "editor created");
    if (editor != nullptr)
    {
        editor->setSize (1000, 600);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        editor.reset();
    }

    plugin->releaseResources();
    plugin.reset();
    std::cout << (failures == 0 ? "PLUGIN LOAD TEST PASSED\n" : "PLUGIN LOAD TEST FAILED\n");
    return failures == 0 ? 0 : 1;
}
