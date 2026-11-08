#pragma once

// Data model of the Lynx Rack sequencer module: channels, patterns, notes, and their versioned
// ValueTree form. No audio, no UI, no hosting: this header is the contract other modules
// (hub app, mixer, multitrack) can read without linking the engine.

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace lynx
{
constexpr int ppq = 960;                  // ticks per quarter note
constexpr int ticksPerStep = ppq / 4;     // one 1/16 step
constexpr int maxChannels = 16;
constexpr int maxPatterns = 16;
constexpr int maxSteps = 64;
constexpr int schemaVersion = 1;
constexpr const char* moduleId = "alphaaudio.lynx.rack";

struct Note
{
    std::uint32_t id = 0;       // stable while editing (selection survives sorting); not saved
    std::int64_t start = 0;     // ticks
    std::int64_t length = ticksPerStep;
    int channel = 0;
    int key = 60;
    float velocity = 0.8f;
    bool muted = false;

    std::int64_t end() const noexcept { return start + length; }
};

struct Pattern
{
    juce::String name;
    int steps = 16;
    std::vector<Note> notes;    // sorted by start, then key
    std::uint32_t nextId = 1;

    std::int64_t lengthTicks() const noexcept { return (std::int64_t) steps * ticksPerStep; }

    void sortNotes()
    {
        std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) {
            return a.start != b.start ? a.start < b.start : a.key < b.key;
        });
    }

    std::uint32_t add (Note n)
    {
        n.id = nextId++;
        notes.push_back (n);
        return n.id;
    }

    Note* find (std::uint32_t id) noexcept
    {
        for (auto& n : notes)
            if (n.id == id)
                return &n;
        return nullptr;
    }

    const Note* find (std::uint32_t id) const noexcept
    {
        for (const auto& n : notes)
            if (n.id == id)
                return &n;
        return nullptr;
    }

    void removeIf (const std::function<bool (const Note&)>& pred)
    {
        notes.erase (std::remove_if (notes.begin(), notes.end(), pred), notes.end());
    }
};

struct ChannelInfo
{
    juce::String name;
    juce::String pluginXml;         // juce::PluginDescription as XML; empty = no plugin
    juce::MemoryBlock pluginState;  // the hosted plugin's own state
    int midiOut = 0;                // 0 = off, 1..16: also send this channel's notes to the host as MIDI
    int transpose = 0;              // semitones
    int stepKey = 60;               // key the step buttons write
};

struct RackSong
{
    std::array<ChannelInfo, maxChannels> channels;
    std::array<Pattern, maxPatterns> patterns;

    RackSong()
    {
        for (int i = 0; i < maxChannels; ++i)
            channels[(size_t) i].name = "Channel " + juce::String (i + 1);
        for (int i = 0; i < maxPatterns; ++i)
            patterns[(size_t) i].name = "Pattern " + juce::String (i + 1);
    }

    juce::ValueTree toValueTree() const
    {
        juce::ValueTree root ("Song");
        root.setProperty ("module", moduleId, nullptr);
        root.setProperty ("schema", schemaVersion, nullptr);

        for (int i = 0; i < maxChannels; ++i)
        {
            const auto& c = channels[(size_t) i];
            juce::ValueTree v ("Channel");
            v.setProperty ("index", i, nullptr);
            v.setProperty ("name", c.name, nullptr);
            v.setProperty ("plugin", c.pluginXml, nullptr);
            if (c.pluginState.getSize() > 0)
                v.setProperty ("state", juce::var (c.pluginState), nullptr);
            v.setProperty ("midiOut", c.midiOut, nullptr);
            v.setProperty ("transpose", c.transpose, nullptr);
            v.setProperty ("stepKey", c.stepKey, nullptr);
            root.appendChild (v, nullptr);
        }

        for (int i = 0; i < maxPatterns; ++i)
        {
            const auto& p = patterns[(size_t) i];
            juce::ValueTree v ("Pattern");
            v.setProperty ("index", i, nullptr);
            v.setProperty ("name", p.name, nullptr);
            v.setProperty ("steps", p.steps, nullptr);

            juce::MemoryOutputStream out;
            out.writeInt ((int) p.notes.size());
            for (const auto& n : p.notes)
            {
                out.writeInt ((int) n.start);
                out.writeInt ((int) n.length);
                out.writeByte ((char) n.channel);
                out.writeByte ((char) n.key);
                out.writeByte ((char) (n.muted ? 1 : 0));
                out.writeFloat (n.velocity);
            }
            v.setProperty ("notes", juce::var (out.getMemoryBlock()), nullptr);
            root.appendChild (v, nullptr);
        }

        return root;
    }

    static bool fromValueTree (const juce::ValueTree& root, RackSong& out)
    {
        if (! root.hasType ("Song") || (int) root.getProperty ("schema", 0) > schemaVersion)
            return false;

        for (const auto& v : root)
        {
            const int index = v.getProperty ("index", -1);

            if (v.hasType ("Channel") && juce::isPositiveAndBelow (index, maxChannels))
            {
                auto& c = out.channels[(size_t) index];
                c.name = v.getProperty ("name", c.name).toString();
                c.pluginXml = v.getProperty ("plugin", {}).toString();
                const auto stateVar = v.getProperty ("state", {});   // keep the var alive: getBinaryData() points into it
                if (auto* mb = stateVar.getBinaryData())
                    c.pluginState = *mb;
                c.midiOut = juce::jlimit (0, 16, (int) v.getProperty ("midiOut", 0));
                c.transpose = juce::jlimit (-48, 48, (int) v.getProperty ("transpose", 0));
                c.stepKey = juce::jlimit (0, 127, (int) v.getProperty ("stepKey", 60));
            }
            else if (v.hasType ("Pattern") && juce::isPositiveAndBelow (index, maxPatterns))
            {
                auto& p = out.patterns[(size_t) index];
                p.name = v.getProperty ("name", p.name).toString();
                p.steps = juce::jlimit (1, maxSteps, (int) v.getProperty ("steps", 16));
                p.notes.clear();

                const auto notesVar = v.getProperty ("notes", {});
                if (auto* mb = notesVar.getBinaryData())
                {
                    juce::MemoryInputStream in (*mb, false);
                    const int count = juce::jlimit (0, 1000000, in.readInt());

                    for (int i = 0; i < count && ! in.isExhausted(); ++i)
                    {
                        Note n;
                        n.start = in.readInt();
                        n.length = std::max (1, in.readInt());
                        n.channel = juce::jlimit (0, maxChannels - 1, (int) (unsigned char) in.readByte());
                        n.key = juce::jlimit (0, 127, (int) (unsigned char) in.readByte());
                        n.muted = in.readByte() != 0;
                        n.velocity = juce::jlimit (0.0f, 1.0f, in.readFloat());
                        p.add (n);
                    }
                }
                p.sortNotes();
            }
        }
        return true;
    }
};
} // namespace lynx
