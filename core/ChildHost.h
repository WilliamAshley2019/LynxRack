#pragma once

// ChildHost: finding, loading and showing the plugins hosted inside the rack channels.
// Message thread only. Finding plugins is the shared AlphaAudio scanner (common/PluginScanner): one process-wide
// library (plugins.xml + lemon_plugins.db + plugin_catalog.json in %APPDATA%/AlphaAudio/LemonMixerRack) that
// Lemon Mixer Rack and FLP Organizer use too, scanned out of process by LemonScan so a bad plugin cannot take the
// DAW down. This is a consolidated approach to merge a common system to all Alpha Audio Plugins using a common architecture for plugin scans.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <memory>

#include "../common/PluginScanner.h"
#include "RackEngine.h"

namespace lynx
{
class ChildHost
{
public:
    explicit ChildHost (RackEngine&);
    ~ChildHost();

    using Report = std::function<void (const juce::String& text, bool finished)>;

    // --- library (the shared scanner is acquired on first use, not when the rack is constructed) ---------
    void scanFolders (Report report);        // default VST3 folders, one LemonScan process per file
    void importFromFl (Report report);       // seed from FL Studio's own plugin database (+ Organizer db)
    void warmUp();                           // acquire the library and read FL's database in the background
    bool isScanning() const noexcept;
    bool scannerAvailable();                    
    int  instrumentCount();
    juce::String libraryHint();              // status bar
    juce::File dataFolder() const;
    int  quarantinedCount() const;
    void releaseQuarantine();

    // IDs for the plugin menu start here (so they can never clash with the slot menu's own IDs).
    static constexpr int menuBase = 10000;
    juce::PopupMenu makePluginMenu();
    bool descriptionForMenuResult (int menuResult, juce::PluginDescription& out) const;

    // Loads (async) `desc` into the channel and records it in the song. `done(error)`: empty error = success.
    // restoreState: apply the channel's saved plugin state (project load) instead of starting fresh.
    void loadPlugin (int channel, const juce::PluginDescription& desc, std::function<void (const juce::String&)> done = {},
                     bool restoreState = false);
    void unloadPlugin (int channel);
    void restoreFromSong();                

    void toggleEditor (int channel);
    void closeEditor (int channel);
    void closeAllEditors();
    bool isEditorOpen (int channel) const noexcept;

    juce::String pluginName (int channel) const;       
    juce::String channelStatus (int channel) const;    

private:
    class PluginWindow;

    lemon::PluginScanner& sc();
    void setError (int channel, const juce::String&);

    RackEngine& engine;
    std::shared_ptr<lemon::PluginScanner> scanner;
    std::vector<lemon::CatalogEntry> menuEntries;
    std::array<std::unique_ptr<PluginWindow>, maxChannels> windows;
    std::array<juce::String, maxChannels> errors;
    std::array<std::uint32_t, maxChannels> loadSerial {};    

    JUCE_DECLARE_WEAK_REFERENCEABLE (ChildHost)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChildHost)
};
} // namespace lynx
