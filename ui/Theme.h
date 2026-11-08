#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lynx::theme
{
inline const juce::Colour bg { 0xff14161a }, panel { 0xff1c1f25 }, panelHi { 0xff262a33 }, line { 0xff343a46 },
    text { 0xffd9dde5 }, dim { 0xff8992a3 }, accent { 0xffe8a33d }, accentHi { 0xfff7d38c }, teal { 0xff4fb3a8 },
    danger { 0xffe0523f }, blackKey { 0xff191b20 }, whiteKey { 0xffd6d9df };

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, bg);
        setColour (juce::TextButton::buttonColourId, panelHi);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, bg);
        setColour (juce::ComboBox::backgroundColourId, panelHi);
        setColour (juce::ComboBox::outlineColourId, line);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, bg);
        setColour (juce::Slider::thumbColourId, accent);
        setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
        setColour (juce::Slider::backgroundColourId, panelHi);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, panelHi);
        setColour (juce::Label::textColourId, text);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ScrollBar::thumbColourId, line);
    }
};
} // namespace lynx::theme
