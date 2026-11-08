#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <mutex>
#include <set>
#include "FlDatabase.h"
#include "PluginDb.h"

namespace lemon
{
// One row of the plugin picker: what JUCE found, enriched with what FL Studio knows.
struct CatalogEntry
{
    juce::PluginDescription desc;
    juce::String id;                 // desc.createIdentifierString()
    juce::String vendor;             // JUCE's manufacturer, else FL's vendor
    juce::String flCategory;         // FL's category string, if FL has it
    juce::String favouriteFolder;    // FL favourite folder, if the user filed it in one
    juce::String orgCategory;        // category from your FLP Organizer database, if linked
    bool inFl = false, inOrganizer = false, favourite = false, quarantined = false;
    bool instrument = false;
};

// Owns the format manager + known-plugin list, persists the list, scans on a background thread
// (dead-man's-pedal file so a plugin that crashes the scan is blacklisted next time) and can seed the
// list from FL Studio's own plugin database instead of walking folders.
class PluginScanner : private juce::Thread
{
public:
    PluginScanner();
    ~PluginScanner() override;

    // One scanner per process, shared by every rack instance (FL creates and destroys instances constantly).
    // It is created by the first instance and stopped by the last; stopping never blocks for long and never
    // kills the thread: a thread that does not stop in time is left to finish on its own.
    static std::shared_ptr<PluginScanner> acquire();
    void requestStop() { noCancel = true; signalThreadShouldExit(); notify(); }
    bool waitStopped(int ms) { return waitForThreadToExit(ms); }

    juce::AudioPluginFormatManager& formats() noexcept { return formatManager; }
    juce::KnownPluginList& list() noexcept { return known; }
    bool isScanning() const noexcept { return isThreadRunning(); }

    // Callbacks run on the message thread.
    void scan(const juce::StringArray& folders, std::function<void(const juce::String&)> progress, std::function<void()> done);
    void importFromFl(std::function<void(const juce::String&)> progress, std::function<void()> done);
    void refreshFl(std::function<void()> done);

    juce::File orgPath() const;                 // FLP Organizer .db (read-only), or an empty File
    void setOrgPath(const juce::File&);
    std::shared_ptr<const data::OrgResult> orgData() const;

    juce::File flRoot() const;
    void setFlRoot(const juce::File&);
    std::shared_ptr<const FlData> flData() const;

    enum class Kind { effects, instruments, all };
    std::vector<CatalogEntry> catalog (Kind kind = Kind::effects) const;   // default keeps Lemon Mixer Rack behaviour
    int missingFromKnown() const noexcept { return missingCount.load(); }   // cached; FL VST3s this list does not have yet
    juce::String lastMessage() const;          // e.g. why a scan could not start / how many files failed
    static juce::File findScanExe();           // LemonScan(.exe) (next to the plugin, or in the data folder)

    static juce::File dataFolder();

private:
    enum class Mode { Folders, ImportFl, RefreshFl };
    void start(Mode, std::function<void(const juce::String&)>, std::function<void()>);
    void run() override;
    void save();
    void exportCatalog();
    void blacklistLeftovers(const juce::File& pedal);
    void loadSources();                         // FL database + Organizer database
    std::vector<juce::String> missingPaths() const;       // slow (touches the disk): scan thread only
    void recomputeMissing();
    void scanIsolated(const juce::StringArray& files, bool& anyAdded);   // one child process per file, timeouts, kill
    void setMessage(const juce::String&);

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList known;
    juce::StringArray foldersToScan;
    std::function<void(const juce::String&)> onProgress;
    std::function<void()> onDone;
    Mode mode = Mode::Folders;

    mutable std::mutex flLock;
    std::shared_ptr<const FlData> fl;
    std::shared_ptr<const data::OrgResult> org;
    std::atomic<bool> noCancel { false };
    std::atomic<int> missingCount { 0 };
    std::atomic<bool> dirty { false };       // plugins.xml needs writing
    mutable std::mutex msgLock;
    juce::String message;
};
}
