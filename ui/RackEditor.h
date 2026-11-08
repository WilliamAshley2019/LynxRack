#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../core/RackProcessor.h"
#include "PianoRoll.h"
#include "RackStrip.h"
#include "Theme.h"

namespace lynx
{
class RackEditor final : public juce::AudioProcessorEditor,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    explicit RackEditor (RackProcessor&);
    ~RackEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void refreshAll();
    void selectChannel (int channel);
    void showSlotMenu (int channel);
    void renameChannel (int channel);
    void showScanMenu();
    void runScan (bool fromFl);

    RackProcessor& rack;          // not "processor": that name belongs to AudioProcessorEditor
    theme::LookAndFeel lnf;

    juce::Label patternLabel { {}, "Pattern" }, swingLabel { {}, "Swing" }, snapLabel { {}, "Snap" }, modeLabel { {}, "Transport" },
        tempoLabel { {}, "Tempo" }, status { {}, {} };
    juce::ComboBox patternBox, lengthBox, snapBox, modeBox;
    juce::Slider swingSlider, tempoSlider;
    juce::TextButton undoButton { "Undo" }, fitButton { "Fit" }, rewindButton { "|<" }, playButton { "Play" }, stopButton { "Stop" },
        panicButton { "Panic" }, scanButton { "Scan plugins" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> patternAttach, modeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> swingAttach, tempoAttach;

    RackStrip strip;
    juce::Viewport viewport;
    PianoRoll roll;

    int shownPattern = -1;
    bool shownPlaying = false;
    juce::String scanStatus;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RackEditor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackEditor)
};
} // namespace lynx
