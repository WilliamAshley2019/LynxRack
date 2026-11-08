#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ChildHost.h"
#include "ModuleLink.h"
#include "RackEngine.h"

namespace lynx
{
class RackProcessor final : public juce::AudioProcessor,
                            private juce::Timer
{
public:
    RackProcessor();
    ~RackProcessor() override;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Lynx Rack"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- module API (UI, hub) ---
    RackEngine& getEngine() noexcept { return engine; }
    ChildHost& getHost() noexcept { return host; }
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return apvts; }
    int activePattern() const noexcept { return juce::jlimit (0, maxPatterns - 1, (int) patternParam->load()); }
    int selectedChannel() const noexcept { return selected; }
    void selectChannel (int channel);
    void setModuleLink (std::shared_ptr<IModuleLink> link);

    static juce::String volId (int ch) { return "ch" + juce::String (ch + 1) + "_vol"; }
    static juce::String panId (int ch) { return "ch" + juce::String (ch + 1) + "_pan"; }
    static juce::String muteId (int ch) { return "ch" + juce::String (ch + 1) + "_mute"; }
    static juce::String soloId (int ch) { return "ch" + juce::String (ch + 1) + "_solo"; }

private:
    static BusesProperties makeBuses();
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void timerCallback() override;
    void applyState (const juce::ValueTree&);

    RackEngine engine;                              // declared before host: the host (and its windows) go first
    ChildHost host { engine };
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* patternParam = nullptr;
    std::atomic<float>* tempoParam = nullptr;
    int selected = 0;
    std::shared_ptr<IModuleLink> link;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RackProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackProcessor)
};
} // namespace lynx
