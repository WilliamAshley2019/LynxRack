#include "PianoRoll.h"

#include "Theme.h"

namespace lynx
{
namespace
{
bool isBlackKey (int key) noexcept
{
    switch (key % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

juce::String keyName (int key)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[key % 12]) + juce::String (key / 12 - 1);
}
} // namespace

PianoRoll::PianoRoll (RackProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    scrollY = (float) (127 - 84) * (float) rowHeight;
}

const Pattern& PianoRoll::pattern() const
{
    return proc.getEngine().song().patterns[(size_t) patternIndex()];
}

juce::Rectangle<int> PianoRoll::gridArea() const noexcept
{
    return { keyboardWidth, rulerHeight, getWidth() - keyboardWidth, getHeight() - rulerHeight - velocityHeight };
}

juce::Rectangle<int> PianoRoll::velocityArea() const noexcept
{
    return { keyboardWidth, getHeight() - velocityHeight, getWidth() - keyboardWidth, velocityHeight };
}

double PianoRoll::xToTick (float x) const noexcept { return scrollTicks + (x - (float) keyboardWidth) / pixelsPerTick; }
float PianoRoll::tickToX (double tick) const noexcept { return (float) keyboardWidth + (float) ((tick - scrollTicks) * pixelsPerTick); }
int PianoRoll::yToKey (float y) const noexcept { return juce::jlimit (0, 127, 127 - (int) std::floor ((y - (float) rulerHeight + scrollY) / (float) rowHeight)); }
float PianoRoll::keyToY (int key) const noexcept { return (float) rulerHeight + (float) (127 - key) * (float) rowHeight - scrollY; }

std::int64_t PianoRoll::snap (double tick) const noexcept
{
    return (std::int64_t) std::llround (tick / (double) snapTicks) * snapTicks;
}

void PianoRoll::clampScroll()
{
    const auto grid = gridArea();
    scrollY = juce::jlimit (0.0f, std::max (0.0f, (float) (128 * rowHeight - grid.getHeight())), scrollY);
    const double visible = (double) grid.getWidth() / pixelsPerTick;
    const double total = (double) pattern().lengthTicks() + ppq;
    scrollTicks = juce::jlimit (0.0, std::max (0.0, total - visible), scrollTicks);
}

void PianoRoll::fitPattern()
{
    const auto grid = gridArea();
    if (grid.getWidth() > 0)
        pixelsPerTick = (double) grid.getWidth() / (double) (pattern().lengthTicks() + ticksPerStep);
    scrollTicks = 0.0;
    clampScroll();
    repaint();
}

void PianoRoll::resized() { clampScroll(); }

void PianoRoll::centreOnNotes()
{
    int lo = 127, hi = 0;
    for (const auto& n : pattern().notes)
        if (n.channel == channel())
        {
            lo = std::min (lo, n.key);
            hi = std::max (hi, n.key);
        }

    const float mid = hi >= lo ? 0.5f * (float) (lo + hi) : 60.0f;
    scrollY = (127.0f - mid) * (float) rowHeight + 0.5f * (float) rowHeight - 0.5f * (float) gridArea().getHeight();
    clampScroll();
    repaint();
}

void PianoRoll::patternOrChannelChanged()
{
    selection.clear();
    drag = Drag::none;
    centreOnNotes();
}

void PianoRoll::setPlayhead (double patternTick, bool playing)
{
    if (playing == isPlaying && std::abs (patternTick - playTick) < 1.0)
        return;

    isPlaying = playing;
    playTick = patternTick;
    repaint (gridArea().withTrimmedLeft (-4));
}

// --- painting ----------------------------------------------------------------------------------------------

void PianoRoll::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto grid = gridArea();
    const auto& pat = pattern();
    const int ch = channel();

    g.fillAll (bg);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (grid);

        const int firstKey = yToKey ((float) grid.getY()), lastKey = yToKey ((float) grid.getBottom());
        for (int key = firstKey; key >= lastKey; --key)
        {
            const auto y = keyToY (key);
            g.setColour (isBlackKey (key) ? panel.darker (0.3f) : panel);
            g.fillRect (juce::Rectangle<float> ((float) grid.getX(), y, (float) grid.getWidth(), (float) rowHeight));
            if (key % 12 == 0)
            {
                g.setColour (line);
                g.drawHorizontalLine ((int) (y + (float) rowHeight - 1.0f), (float) grid.getX(), (float) grid.getRight());
            }
        }

        const auto endX = tickToX ((double) pat.lengthTicks());
        if (endX < (float) grid.getRight())
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (std::max (endX, (float) grid.getX()), (float) grid.getY(), (float) grid.getRight() - endX, (float) grid.getHeight()));
        }

        const auto firstTick = (std::int64_t) std::floor (xToTick ((float) grid.getX()) / ticksPerStep) * ticksPerStep;
        for (auto t = std::max<std::int64_t> (0, firstTick); tickToX ((double) t) < (float) grid.getRight(); t += ticksPerStep)
        {
            const bool bar = t % (4 * ppq) == 0, beat = t % ppq == 0;
            const auto x = tickToX ((double) t);
            if (! bar && ! beat && pixelsPerTick * ticksPerStep < 8.0)
                continue;
            g.setColour (bar ? text.withAlpha (0.35f) : beat ? line.brighter (0.2f) : line.withAlpha (0.5f));
            g.drawVerticalLine ((int) x, (float) grid.getY(), (float) grid.getBottom());
        }

        // ghost notes of the other channels
        g.setColour (dim.withAlpha (0.28f));
        for (const auto& n : pat.notes)
            if (n.channel != ch)
                g.fillRect (juce::Rectangle<float> (tickToX ((double) n.start), keyToY (n.key) + 2.0f,
                                                    std::max (2.0f, (float) (n.length * pixelsPerTick) - 1.0f), (float) rowHeight - 4.0f));

        for (const auto& n : pat.notes)
        {
            if (n.channel != ch)
                continue;
            const juce::Rectangle<float> r (tickToX ((double) n.start), keyToY (n.key) + 1.0f,
                                            std::max (3.0f, (float) (n.length * pixelsPerTick) - 1.0f), (float) rowHeight - 2.0f);
            if (! r.intersects (grid.toFloat()))
                continue;
            const bool sel = selection.count (n.id) != 0;
            auto c = (sel ? accentHi : accent).withMultipliedBrightness (0.55f + 0.45f * n.velocity);
            if (n.muted)
                c = c.withSaturation (0.1f).withAlpha (0.5f);
            g.setColour (c);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (sel ? juce::Colours::white : bg.withAlpha (0.7f));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
        }

        if (drag == Drag::band)
        {
            g.setColour (accent.withAlpha (0.18f));
            g.fillRect (band);
            g.setColour (accent);
            g.drawRect (band, 1.0f);
        }

        if (playTick >= 0.0 && isPlaying)
        {
            g.setColour (teal);
            g.fillRect (juce::Rectangle<float> (tickToX (playTick), (float) grid.getY(), 2.0f, (float) grid.getHeight()));
        }
    }

    // ruler
    g.setColour (panelHi);
    g.fillRect (juce::Rectangle<int> (keyboardWidth, 0, getWidth() - keyboardWidth, rulerHeight));
    g.setColour (dim);
    g.setFont (juce::FontOptions (11.0f));
    for (std::int64_t t = 0; t <= pat.lengthTicks(); t += ppq)
    {
        const auto x = tickToX ((double) t);
        if (x < (float) keyboardWidth || x > (float) getWidth())
            continue;
        g.drawVerticalLine ((int) x, (float) rulerHeight - 6.0f, (float) rulerHeight);
        if (t % (4 * ppq) == 0 || pixelsPerTick * ppq > 28.0)
            g.drawText (juce::String (t / ppq / 4 + 1) + (t % (4 * ppq) == 0 ? "" : "." + juce::String (t / ppq % 4 + 1)),
                        (int) x + 3, 1, 36, rulerHeight - 4, juce::Justification::centredLeft);
    }

    // keyboard
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (juce::Rectangle<int> (0, grid.getY(), keyboardWidth, grid.getHeight()));
        const int firstKey = yToKey ((float) grid.getY()), lastKey = yToKey ((float) grid.getBottom());
        g.setFont (juce::FontOptions (10.0f));
        for (int key = firstKey; key >= lastKey; --key)
        {
            const auto y = keyToY (key);
            const juce::Rectangle<float> r (0.0f, y, (float) keyboardWidth - 1.0f, (float) rowHeight);
            g.setColour (key == previewKey ? accent : isBlackKey (key) ? blackKey : whiteKey);
            g.fillRect (r.withTrimmedBottom (1.0f));
            if (key % 12 == 0 || key == previewKey)
            {
                g.setColour (isBlackKey (key) ? whiteKey : bg);
                g.drawText (keyName (key), r.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft);
            }
        }
    }

    // velocity lane
    const auto vel = velocityArea();
    g.setColour (panel.darker (0.2f));
    g.fillRect (vel);
    g.setColour (line);
    g.drawHorizontalLine (vel.getY(), 0.0f, (float) getWidth());
    g.setColour (dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("Velocity", 4, vel.getY() + 2, keyboardWidth - 6, 14, juce::Justification::centredLeft);
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (vel);
        for (const auto& n : pat.notes)
        {
            if (n.channel != ch)
                continue;
            const auto x = tickToX ((double) n.start);
            if (x < (float) vel.getX() - 4.0f || x > (float) vel.getRight())
                continue;
            const auto h = n.velocity * (float) (vel.getHeight() - 8);
            g.setColour (selection.count (n.id) != 0 ? accentHi : accent);
            g.fillRect (juce::Rectangle<float> (x, (float) vel.getBottom() - 2.0f - h, 3.0f, h));
        }
    }
}

// --- mouse ---------------------------------------------------------------------------------------------------

int PianoRoll::hitNote (juce::Point<float> p, bool& onRightEdge) const
{
    const auto& notes = pattern().notes;
    onRightEdge = false;

    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto& n = notes[(size_t) i];
        if (n.channel != channel())
            continue;
        const juce::Rectangle<float> r (tickToX ((double) n.start), keyToY (n.key), std::max (3.0f, (float) (n.length * pixelsPerTick)), (float) rowHeight);
        if (r.contains (p))
        {
            onRightEdge = p.x > r.getRight() - std::min (6.0f, r.getWidth() * 0.4f);
            return i;
        }
    }
    return -1;
}

void PianoRoll::edit (const std::function<void (Pattern&)>& fn)
{
    auto& engine = proc.getEngine();
    if (! undoPushed)
    {
        engine.beginEdit (patternIndex());
        undoPushed = true;
    }
    engine.editPattern (patternIndex(), fn);
}

void PianoRoll::setPreview (int key)
{
    if (key == previewKey)
        return;

    auto& engine = proc.getEngine();
    if (previewKey >= 0)
        engine.liveNote (channel(), previewKey, 0.0f, false);
    previewKey = key;
    if (previewKey >= 0)
        engine.liveNote (channel(), previewKey, 0.8f, true);
    repaint (juce::Rectangle<int> (0, rulerHeight, keyboardWidth, getHeight()));
}

void PianoRoll::applyVelocity (juce::Point<int> p)
{
    const auto area = velocityArea();
    const float value = juce::jlimit (0.02f, 1.0f, 1.0f - (float) (p.y - area.getY() - 2) / (float) (area.getHeight() - 8));
    const auto& notes = pattern().notes;
    std::uint32_t target = 0;
    float best = 7.0f;

    for (const auto& n : notes)
        if (n.channel == channel())
            if (const float d = std::abs (tickToX ((double) n.start) + 1.0f - (float) p.x); d < best)
            {
                best = d;
                target = n.id;
            }

    if (target == 0)
        return;

    const bool all = selection.count (target) != 0;
    edit ([&] (Pattern& pat) {
        for (auto& n : pat.notes)
            if (n.id == target || (all && selection.count (n.id) != 0))
                n.velocity = value;
    });
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    undoPushed = false;
    dragKeyDelta = 0;
    const auto grid = gridArea();
    const auto pos = e.position;

    if (e.x < keyboardWidth && e.y >= rulerHeight && e.y < grid.getBottom())
    {
        drag = Drag::keyboard;
        setPreview (yToKey (pos.y));
        return;
    }

    if (velocityArea().contains (e.getPosition()))
    {
        drag = Drag::velocity;
        applyVelocity (e.getPosition());
        return;
    }

    if (! grid.contains (e.getPosition()))
        return;

    bool edge = false;
    const int hit = hitNote (pos, edge);

    if (e.mods.isRightButtonDown())
    {
        drag = Drag::erase;
        if (hit >= 0)
        {
            const auto id = pattern().notes[(size_t) hit].id;
            edit ([id] (Pattern& pat) { pat.removeIf ([id] (const Note& n) { return n.id == id; }); });
            selection.erase (id);
        }
        return;
    }

    if (hit < 0)
    {
        if (e.mods.isShiftDown())
        {
            drag = Drag::band;
            band = { pos, pos };
            if (! e.mods.isCtrlDown())
                selection.clear();
            repaint();
            return;
        }

        // draw a new note; dragging sets its length
        const auto start = std::max<std::int64_t> (0, snap (xToTick (pos.x) - snapTicks * 0.5));
        if (start >= pattern().lengthTicks())
            return;

        Note n;
        n.start = start;
        n.length = std::min (lastLength, pattern().lengthTicks() - start);
        n.channel = channel();
        n.key = yToKey (pos.y);
        std::uint32_t id = 0;
        edit ([&] (Pattern& pat) { id = pat.add (n); });
        selection = { id };
        grabbedId = id;
        originals.clear();
        if (const auto* created = pattern().find (id))
            originals.push_back (*created);
        grabTick = (double) originals.front().end();
        grabKey = n.key;
        drag = Drag::resize;
        setPreview (n.key);
        return;
    }

    const auto& hitNoteRef = pattern().notes[(size_t) hit];
    const auto id = hitNoteRef.id;

    if (e.mods.isShiftDown())
    {
        if (! selection.erase (id))
            selection.insert (id);
        repaint();
        return;
    }

    if (selection.count (id) == 0)
        selection = { id };

    if (e.mods.isCtrlDown() && ! edge)
        duplicateSelected();   // the copies become the selection and are the ones dragged

    originals.clear();
    for (const auto& n : pattern().notes)
        if (selection.count (n.id) != 0)
            originals.push_back (n);

    grabbedId = id;
    grabTick = xToTick (pos.x);
    grabKey = hitNoteRef.key;
    drag = edge ? Drag::resize : Drag::move;

    if (edge)
        grabTick = (double) hitNoteRef.end();
    else
        setPreview (hitNoteRef.key);

    repaint();
}

void PianoRoll::applyDrag (const juce::MouseEvent& e)
{
    const auto it = std::find_if (originals.begin(), originals.end(), [this] (const Note& n) { return n.id == grabbedId; });
    if (it == originals.end())
        return;

    const auto& g = *it;

    if (drag == Drag::move)
    {
        const auto raw = xToTick (e.position.x) - grabTick;
        const auto delta = snap ((double) g.start + raw) - g.start;
        const int dKey = yToKey (e.position.y) - grabKey;
        const auto limit = pattern().lengthTicks();

        edit ([&] (Pattern& pat) {
            for (const auto& o : originals)
                if (auto* n = pat.find (o.id))
                {
                    n->start = juce::jlimit<std::int64_t> (0, std::max<std::int64_t> (0, limit - 1), o.start + delta);
                    n->key = juce::jlimit (0, 127, o.key + dKey);
                }
        });

        if (dKey != dragKeyDelta)
        {
            dragKeyDelta = dKey;
            setPreview (juce::jlimit (0, 127, g.key + dKey));
        }
    }
    else if (drag == Drag::resize)
    {
        const auto raw = xToTick (e.position.x) - grabTick;
        const auto delta = snap ((double) g.end() + raw) - g.end();
        const std::int64_t minimum = std::max<std::int64_t> (snapTicks > 1 ? snapTicks : 1, ppq / 24);

        edit ([&] (Pattern& pat) {
            for (const auto& o : originals)
                if (auto* n = pat.find (o.id))
                    n->length = std::max (minimum, o.length + delta);
        });

        if (const auto* n = pattern().find (grabbedId))
            lastLength = n->length;
    }
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::keyboard:
            setPreview (yToKey (e.position.y));
            break;

        case Drag::velocity:
            applyVelocity (e.getPosition());
            break;

        case Drag::move:
        case Drag::resize:
            applyDrag (e);
            break;

        case Drag::erase:
        {
            bool edge = false;
            if (const int hit = hitNote (e.position, edge); hit >= 0)
            {
                const auto id = pattern().notes[(size_t) hit].id;
                edit ([id] (Pattern& pat) { pat.removeIf ([id] (const Note& n) { return n.id == id; }); });
                selection.erase (id);
            }
            break;
        }

        case Drag::band:
        {
            band = juce::Rectangle<float> (e.getMouseDownPosition().toFloat(), e.position);
            const auto t0 = xToTick (band.getX()), t1 = xToTick (band.getRight());
            const auto k1 = yToKey (band.getY()), k0 = yToKey (band.getBottom());

            if (! e.mods.isCtrlDown())
                selection.clear();
            for (const auto& n : pattern().notes)
                if (n.channel == channel() && (double) n.end() > t0 && (double) n.start < t1 && n.key >= k0 && n.key <= k1)
                    selection.insert (n.id);
            repaint();
            break;
        }

        case Drag::none:
            break;
    }
}

void PianoRoll::mouseUp (const juce::MouseEvent&)
{
    setPreview (-1);
    drag = Drag::none;
    originals.clear();
    repaint();
}

void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    bool edge = false;
    const int hit = gridArea().contains (e.getPosition()) ? hitNote (e.position, edge) : -1;
    setMouseCursor (hit >= 0 && edge ? juce::MouseCursor::LeftRightResizeCursor
                    : hit >= 0 ? juce::MouseCursor::DraggingHandCursor
                               : juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown())
    {
        const auto anchor = xToTick (e.position.x);
        pixelsPerTick = juce::jlimit (0.01, 1.5, pixelsPerTick * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
        scrollTicks = anchor - (e.position.x - (float) keyboardWidth) / pixelsPerTick;
    }
    else if (e.mods.isAltDown())
    {
        rowHeight = juce::jlimit (8, 28, rowHeight + (w.deltaY > 0 ? 1 : -1));
    }
    else if (e.mods.isShiftDown())
    {
        scrollTicks -= (double) (! juce::exactlyEqual (w.deltaY, 0.0f) ? w.deltaY : w.deltaX) * 600.0 / pixelsPerTick * 0.1;
    }
    else
    {
        scrollY -= w.deltaY * 120.0f;
    }

    clampScroll();
    repaint();
}

// --- keyboard ------------------------------------------------------------------------------------------------

void PianoRoll::deleteSelected()
{
    if (selection.empty())
        return;

    auto doomed = selection;
    undoPushed = false;
    edit ([&] (Pattern& pat) { pat.removeIf ([&] (const Note& n) { return doomed.count (n.id) != 0; }); });
    selection.clear();
    undoPushed = false;
}

void PianoRoll::duplicateSelected()
{
    if (selection.empty())
        return;

    std::vector<Note> copies;
    std::int64_t first = std::numeric_limits<std::int64_t>::max(), last = 0;

    for (const auto& n : pattern().notes)
        if (selection.count (n.id) != 0)
        {
            copies.push_back (n);
            first = std::min (first, n.start);
            last = std::max (last, n.end());
        }

    const auto shift = std::max<std::int64_t> (snapTicks, last - first);
    std::set<std::uint32_t> added;

    edit ([&] (Pattern& pat) {
        for (auto n : copies)
        {
            n.start += shift;
            if (n.start < pat.lengthTicks())
                added.insert (pat.add (n));
        }
    });

    if (! added.empty())
        selection = added;
}

void PianoRoll::nudgeSelected (std::int64_t dTicks, int dKeys)
{
    if (selection.empty())
        return;

    const auto limit = pattern().lengthTicks();
    undoPushed = false;
    edit ([&] (Pattern& pat) {
        for (auto& n : pat.notes)
            if (selection.count (n.id) != 0)
            {
                n.start = juce::jlimit<std::int64_t> (0, limit - 1, n.start + dTicks);
                n.key = juce::jlimit (0, 127, n.key + dKeys);
            }
    });
    undoPushed = false;
}

bool PianoRoll::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto code = k.getKeyCode();
    const bool ctrl = mods.isCtrlDown() || mods.isCommandDown();

    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        deleteSelected();
        return true;
    }

    if (ctrl && code == 'A')
    {
        selection.clear();
        for (const auto& n : pattern().notes)
            if (n.channel == channel())
                selection.insert (n.id);
        repaint();
        return true;
    }

    if (ctrl && code == 'Z')
    {
        proc.getEngine().undo();
        return true;
    }

    if (ctrl && code == 'D')
    {
        undoPushed = false;
        duplicateSelected();
        undoPushed = false;
        return true;
    }

    if (ctrl && code == 'Q' && snapTicks > 1)
    {
        undoPushed = false;
        edit ([&] (Pattern& pat) {
            for (auto& n : pat.notes)
                if (selection.count (n.id) != 0)
                    n.start = std::clamp<std::int64_t> (snap ((double) n.start), 0, pat.lengthTicks() - 1);
        });
        undoPushed = false;
        return true;
    }

    if (code == 'M' && ! ctrl)
    {
        undoPushed = false;
        edit ([&] (Pattern& pat) {
            for (auto& n : pat.notes)
                if (selection.count (n.id) != 0)
                    n.muted = ! n.muted;
        });
        undoPushed = false;
        return true;
    }

    if (code == juce::KeyPress::leftKey)  { nudgeSelected (-(std::int64_t) snapTicks, 0); return true; }
    if (code == juce::KeyPress::rightKey) { nudgeSelected (snapTicks, 0); return true; }
    if (code == juce::KeyPress::upKey)    { nudgeSelected (0, mods.isShiftDown() ? 12 : 1); return true; }
    if (code == juce::KeyPress::downKey)  { nudgeSelected (0, mods.isShiftDown() ? -12 : -1); return true; }

    return false;
}
} // namespace lynx
