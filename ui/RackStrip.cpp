#include "RackStrip.h"

#include "Theme.h"

namespace lynx
{
namespace
{
constexpr int selectW = 96, slotW = 150, guiW = 34, toggleW = 26, panW = 52, volW = 86, meterW = 8, gap = 4;
}

RackRow::RackRow (RackProcessor& p, int ch) : proc (p), channel (ch)
{
    for (auto* c : std::initializer_list<juce::Component*> { &selectButton, &slotButton, &guiButton, &muteButton, &soloButton, &panSlider, &volSlider })
        addAndMakeVisible (*c);

    selectButton.onClick = [this] { if (onSelect) onSelect (channel); };
    slotButton.onClick = [this] { if (onSlotMenu) onSlotMenu (channel); };
    guiButton.setButtonText ("GUI");
    guiButton.onClick = [this] { if (onToggleGui) onToggleGui (channel); };
    guiButton.setTooltip ("Show or hide the hosted plugin's window");
    muteButton.setClickingTogglesState (true);
    soloButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, theme::danger);
    soloButton.setColour (juce::TextButton::buttonOnColourId, theme::teal);
    muteButton.setTooltip ("Mute");
    soloButton.setTooltip ("Solo");

    panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    panSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setDoubleClickReturnValue (true, 0.0);
    panSlider.setTooltip ("Pan");
    volSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    volSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volSlider.setDoubleClickReturnValue (true, 0.0);
    volSlider.setTooltip ("Volume");

    auto& apvts = proc.getParameters();
    panAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, RackProcessor::panId (ch), panSlider);
    volAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, RackProcessor::volId (ch), volSlider);
    muteAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, RackProcessor::muteId (ch), muteButton);
    soloAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, RackProcessor::soloId (ch), soloButton);

    refresh();
}

juce::Rectangle<int> RackRow::stepsArea() const noexcept
{
    const int x = selectW + slotW + guiW + 2 * toggleW + panW + volW + meterW + 8 * gap;
    return { x, 3, std::max (0, getWidth() - x - 6), getHeight() - 6 };
}

juce::Rectangle<int> RackRow::meterArea() const noexcept
{
    const int x = selectW + slotW + guiW + 2 * toggleW + panW + volW + 7 * gap;
    return { x, 4, meterW, getHeight() - 8 };
}

void RackRow::resized()
{
    auto r = getLocalBounds().reduced (0, 3);
    selectButton.setBounds (r.removeFromLeft (selectW));       r.removeFromLeft (gap);
    slotButton.setBounds (r.removeFromLeft (slotW));           r.removeFromLeft (gap);
    guiButton.setBounds (r.removeFromLeft (guiW));             r.removeFromLeft (gap);
    muteButton.setBounds (r.removeFromLeft (toggleW));         r.removeFromLeft (gap / 2);
    soloButton.setBounds (r.removeFromLeft (toggleW));         r.removeFromLeft (gap);
    panSlider.setBounds (r.removeFromLeft (panW));             r.removeFromLeft (gap);
    volSlider.setBounds (r.removeFromLeft (volW));
}

void RackRow::refresh()
{
    auto& engine = proc.getEngine();
    const auto& info = engine.song().channels[(size_t) channel];
    const auto& pat = engine.song().patterns[(size_t) proc.activePattern()];

    steps = pat.steps;
    bits = 0;
    for (const auto& n : pat.notes)
        if (n.channel == channel && n.start >= 0 && n.start < pat.lengthTicks())
            bits |= std::uint64_t (1) << (int) std::min<std::int64_t> (63, n.start / ticksPerStep);

    selected = proc.selectedChannel() == channel;
    crashed = engine.hasCrashed (channel);
    selectButton.setButtonText (info.name);
    selectButton.setClickingTogglesState (false);
    selectButton.setToggleState (selected, juce::dontSendNotification);

    auto* plugin = engine.channelPlugin (channel);
    const auto saved = proc.getHost().pluginName (channel);          // the song remembers the name even when the plugin is not loaded
    const auto why = proc.getHost().channelStatus (channel);
    const bool failed = why.isNotEmpty() && plugin == nullptr && ! why.startsWith ("Loading");
    slotButton.setButtonText (crashed ? "(crashed) " + saved
                              : plugin != nullptr ? plugin->getName()
                              : failed ? "(not loaded) " + saved
                              : saved.isNotEmpty() ? saved + "..." : juce::String ("Load a plugin..."));
    slotButton.setTooltip (why);
    slotButton.setColour (juce::TextButton::textColourOffId, crashed || failed ? theme::danger : plugin != nullptr ? theme::text : theme::dim);
    guiButton.setEnabled (plugin != nullptr);
    repaint();
}

void RackRow::setPlayStep (int step)
{
    if (step == playStep)
        return;

    playStep = step;
    repaint (stepsArea());
}

void RackRow::setLevel (float peak)
{
    const float next = std::max (peak, level * 0.82f);
    if (std::abs (next - level) < 0.004f)
        return;

    level = next < 0.001f ? 0.0f : next;
    repaint (meterArea());
}

void RackRow::paint (juce::Graphics& g)
{
    using namespace theme;
    g.setColour (selected ? panelHi : panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.0f, 1.0f), 3.0f);

    const auto m = meterArea().toFloat();
    g.setColour (bg);
    g.fillRect (m);
    const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
    const auto h = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f) * m.getHeight();
    g.setColour (level > 1.0f ? danger : teal);
    g.fillRect (m.withTop (m.getBottom() - h));

    const auto area = stepsArea();
    if (area.isEmpty() || steps <= 0)
        return;

    const float cell = (float) area.getWidth() / (float) steps;
    for (int s = 0; s < steps; ++s)
    {
        const auto r = juce::Rectangle<float> ((float) area.getX() + (float) s * cell, (float) area.getY(), cell, (float) area.getHeight()).reduced (1.0f);
        const bool on = ((bits >> s) & 1u) != 0;
        const bool beatGroup = (s / 4) % 2 == 0;
        g.setColour (on ? accent : beatGroup ? panelHi.brighter (0.15f) : panelHi.darker (0.2f));
        g.fillRoundedRectangle (r, 2.0f);

        if (s == playStep)
        {
            g.setColour (teal.withAlpha (on ? 0.9f : 0.5f));
            g.drawRoundedRectangle (r, 2.0f, 1.5f);
        }
    }
}

int RackRow::stepAt (juce::Point<int> p) const noexcept
{
    const auto area = stepsArea();
    if (! area.contains (p) || steps <= 0)
        return -1;
    return juce::jlimit (0, steps - 1, (int) ((float) (p.x - area.getX()) / ((float) area.getWidth() / (float) steps)));
}

void RackRow::setStep (int step, bool on)
{
    auto& engine = proc.getEngine();
    const int pattern = proc.activePattern(), ch = channel;
    const int key = engine.song().channels[(size_t) ch].stepKey;

    engine.editPattern (pattern, [=] (Pattern& pat) {
        const auto from = (std::int64_t) step * ticksPerStep, to = from + ticksPerStep;
        pat.removeIf ([=] (const Note& n) { return n.channel == ch && n.start >= from && n.start < to; });

        if (on)
        {
            Note n;
            n.start = from;
            n.length = ticksPerStep;
            n.channel = ch;
            n.key = key;
            pat.add (n);
        }
    });
}

void RackRow::mouseDown (const juce::MouseEvent& e)
{
    const int step = stepAt (e.getPosition());

    if (step < 0)
    {
        if (onSelect)
            onSelect (channel);
        return;
    }

    proc.getEngine().beginEdit (proc.activePattern());
    paintValue = ((bits >> step) & 1u) == 0;
    lastDragStep = step;
    dragging = true;
    setStep (step, paintValue);

    if (paintValue)
    {
        auto& engine = proc.getEngine();
        engine.liveNote (channel, engine.song().channels[(size_t) channel].stepKey, 0.8f, true);
        juce::Timer::callAfterDelay (120, [w = juce::Component::SafePointer<RackRow> (this), key = engine.song().channels[(size_t) channel].stepKey] {
            if (w != nullptr)
                w->proc.getEngine().liveNote (w->channel, key, 0.0f, false);
        });
    }
}

void RackRow::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const int step = stepAt (e.getPosition());
    if (step < 0 || step == lastDragStep)
        return;

    lastDragStep = step;
    setStep (step, paintValue);
}

void RackRow::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    lastDragStep = -1;
}

// --- RackStrip --------------------------------------------------------------------------------------------------

RackStrip::RackStrip (RackProcessor& p) : proc (p)
{
    for (int i = 0; i < maxChannels; ++i)
    {
        auto row = std::make_unique<RackRow> (proc, i);
        row->onSelect = [this] (int ch) { if (onSelect) onSelect (ch); };
        row->onSlotMenu = [this] (int ch) { if (onSlotMenu) onSlotMenu (ch); };
        row->onToggleGui = [this] (int ch) { if (onToggleGui) onToggleGui (ch); };
        addAndMakeVisible (*row);
        rows[(size_t) i] = std::move (row);
    }
}

void RackStrip::resized()
{
    for (int i = 0; i < maxChannels; ++i)
        rows[(size_t) i]->setBounds (0, i * RackRow::height, getWidth(), RackRow::height);
}

void RackStrip::refresh()
{
    for (auto& r : rows)
        r->refresh();
}

void RackStrip::setPlayStep (int step)
{
    if (step == lastStep)
        return;

    lastStep = step;
    for (auto& r : rows)
        r->setPlayStep (step);
}

void RackStrip::tickMeters()
{
    for (int i = 0; i < maxChannels; ++i)
        rows[(size_t) i]->setLevel (proc.getEngine().takePeak (i));
}
} // namespace lynx
