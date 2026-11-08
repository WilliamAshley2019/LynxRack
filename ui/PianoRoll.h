#pragma once

// Piano roll for the selected channel of the active pattern. Left click on empty grid draws a note (drag to set its
// length), drag a note to move, drag its right edge to resize, right click / right drag deletes, Shift+drag
// rubber-band selects, Ctrl+drag copies. The bottom lane edits velocity.
// Keys: Delete, Ctrl+A, Ctrl+Z, Ctrl+D duplicate, Ctrl+Q quantize starts, M mute, arrows nudge / transpose.
// Wheel: scroll; Shift = horizontal; Ctrl = zoom time; Alt = zoom keys.

#include <juce_gui_basics/juce_gui_basics.h>

#include <limits>
#include <set>

#include "../core/RackProcessor.h"

namespace lynx
{
class PianoRoll final : public juce::Component
{
public:
    explicit PianoRoll (RackProcessor&);

    void setPlayhead (double patternTick, bool playing);
    void setSnap (int ticks) noexcept { snapTicks = std::max (1, ticks); repaint(); }
    void patternOrChannelChanged();
    void fitPattern();
    void centreOnNotes();                 // vertical scroll to the channel's notes (or middle C)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag { none, move, resize, band, erase, velocity, keyboard };

    static constexpr int keyboardWidth = 56, rulerHeight = 20, velocityHeight = 64;

    juce::Rectangle<int> gridArea() const noexcept;
    juce::Rectangle<int> velocityArea() const noexcept;
    double xToTick (float x) const noexcept;
    float tickToX (double tick) const noexcept;
    int yToKey (float y) const noexcept;
    float keyToY (int key) const noexcept;
    std::int64_t snap (double tick) const noexcept;
    void clampScroll();

    const Pattern& pattern() const;
    int channel() const noexcept { return proc.selectedChannel(); }
    int patternIndex() const noexcept { return proc.activePattern(); }
    int hitNote (juce::Point<float> p, bool& onRightEdge) const;
    void edit (const std::function<void (Pattern&)>& fn);
    void deleteSelected();
    void duplicateSelected();
    void nudgeSelected (std::int64_t dTicks, int dKeys);
    void setPreview (int key);
    void applyVelocity (juce::Point<int> p);
    void applyDrag (const juce::MouseEvent&);

    RackProcessor& proc;
    double pixelsPerTick = 0.1, scrollTicks = 0.0;
    int rowHeight = 14;
    float scrollY = 0.0f;
    int snapTicks = ticksPerStep;
    std::int64_t lastLength = ticksPerStep;

    std::set<std::uint32_t> selection;
    std::vector<Note> originals;          // the selection at the start of a drag
    std::uint32_t grabbedId = 0;
    double grabTick = 0.0;
    int grabKey = 60, previewKey = -1, dragKeyDelta = 0;
    Drag drag = Drag::none;
    juce::Rectangle<float> band;
    double playTick = -1.0;
    bool isPlaying = false, undoPushed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
} // namespace lynx
