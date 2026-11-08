#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>

namespace lemon
{
// Read-only view of FL Studio's own plugin database
//   <Documents>/Image-Line/FL Studio/Presets/Plugin database/
// Installed/.../.Plugins.ini : every plugin FL has scanned (path, vendor, category).
// Effects/<folder>/*.fst      : the user's favourite folders (e.g. "Delay reverb").
// Nothing here is ever written back.
struct FlPlugin
{
    juce::String name, vendor, category, format, path;
    bool instrument = false;                         // listed under FL's Generators
};

struct FlData
{
    juce::File root;
    bool found = false;
    std::vector<FlPlugin> installed;                 // effects and generators (see FlPlugin::instrument)
    std::map<juce::String, juce::String> favourite;  // lower-case plugin name -> favourite folder ("" = top level)
};

juce::File defaultFlDatabaseRoot();
FlData readFlDatabase(const juce::File& root, const std::atomic<bool>& cancel);
juce::String normalisePath(const juce::String& path);     // for comparing FL paths with JUCE plugin paths
}
