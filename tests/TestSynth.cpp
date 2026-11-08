// LynxTestSynth: a minimal VST3 instrument used only by the host tests.
//  * square wave at the played key, amplitude 0.25, starts on the exact sample the note-on arrives
//  * state = 4 payload bytes + [before-prepare flag] + [state-was-applied flag]
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

class TestSynth final : public juce::AudioProcessor
{
public:
    TestSynth() : juce::AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "LynxTestSynth"; }
    void prepareToPlay (double sr, int) override { rate = sr; prepared = true; }
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo(); }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        b.clear();
        auto it = m.begin();
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            while (it != m.end() && (*it).samplePosition <= i)
            {
                const auto msg = (*it).getMessage();
                if (msg.isNoteOn())  { key = msg.getNoteNumber(); phase = 0.0; }
                if (msg.isNoteOff() && msg.getNoteNumber() == key) key = -1;
                ++it;
            }
            if (key >= 0)
            {
                const float v = phase < 0.5 ? 0.25f : -0.25f;
                for (int c = 0; c < b.getNumChannels(); ++c)
                    b.setSample (c, i, v);
                phase += juce::MidiMessage::getMidiNoteInHertz (key) / rate;
                phase -= std::floor (phase);
            }
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& d) override
    {
        d.setSize (6);
        d.copyFrom (payload, 0, 4);
        d[4] = (char) (stateBeforePrepare ? 1 : 0);
        d[5] = (char) (stateApplied ? 1 : 0);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (size >= 4) std::memcpy (payload, data, 4);
        stateApplied = true;
        if (! prepared) stateBeforePrepare = true;
    }

private:
    double rate = 44100.0, phase = 0.0;
    int key = -1;
    bool prepared = false, stateBeforePrepare = false, stateApplied = false;
    char payload[4] { 'S', 'Y', 'N', 'T' };
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestSynth(); }
