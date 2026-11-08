#pragma once

// The channel rack: one row per channel (select, plugin slot, GUI, mute, solo, pan, volume, meter, step buttons).
// Step bits are cached per row when the song changes, so painting never scans the notes.

#include <juce_gui_basics/juce_gui_basics.h>

#include "../core/RackProcessor.h"

namespace lynx
{
class RackRow final : public juce::Component
{
public:
    static constexpr int height = 30;

    RackRow (RackProcessor&, int channel);

    void refresh();                           // song / selection / plugin changed
    void setPlayStep (int step);              // -1 = none
    void setLevel (float peak);               // meter, called by the editor's timer

    std::function<void (int)> onSelect, onSlotMenu, onToggleGui;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> stepsArea() const noexcept;
    juce::Rectangle<int> meterArea() const noexcept;
    int stepAt (juce::Point<int>) const noexcept;
    void setStep (int step, bool on);

    RackProcessor& proc;
    const int channel;

    juce::TextButton selectButton, slotButton, guiButton, muteButton { "M" }, soloButton { "S" };
    juce::Slider panSlider, volSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttach, volAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttach, soloAttach;

    std::uint64_t bits = 0;
    int steps = 16, playStep = -1, lastDragStep = -1;
    bool paintValue = true, dragging = false, selected = false, crashed = false;
    float level = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackRow)
};

class RackStrip final : public juce::Component
{
public:
    explicit RackStrip (RackProcessor&);

    void refresh();
    void setPlayStep (int step);
    void tickMeters();
    void resized() override;

    std::function<void (int)> onSelect, onSlotMenu, onToggleGui;

private:
    RackProcessor& proc;
    std::array<std::unique_ptr<RackRow>, maxChannels> rows;
    int lastStep = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackStrip)
};
} // namespace lynx
