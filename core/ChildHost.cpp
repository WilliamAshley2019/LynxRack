#include "ChildHost.h"

#include "../common/CrashGuard.h"
#include "../common/HostLog.h"

#include <algorithm>
#include <map>

namespace lynx
{
namespace
{
constexpr const char* ownName = "Lynx Rack";   // never offer ourselves as a child (no rack inside a rack)
}

// --- plugin editor window ----------------------------------------------------------------------------

class ChildHost::PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessorEditor* editor, const juce::String& title, std::function<void()> onClose)
        : juce::DocumentWindow (title, juce::Colours::darkgrey, juce::DocumentWindow::closeButton), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (closeCallback)
            closeCallback();
    }

private:
    std::function<void()> closeCallback;
};

// --- library ---------------------------------------------------------------------------------------------

ChildHost::ChildHost (RackEngine& e) : engine (e) {}

ChildHost::~ChildHost()
{
    closeAllEditors();
    lemon::CrashGuard::instance().endOwned (this);     // any mark we still hold is not a crash: we are shutting down normally
    scanner.reset();                                   // the last rack to go stops the scan thread
}

lemon::PluginScanner& ChildHost::sc()
{
    if (scanner == nullptr)
        scanner = lemon::PluginScanner::acquire();

    return *scanner;
}

void ChildHost::warmUp()
{
    auto& s = sc();
    if (s.flData() == nullptr && ! s.isScanning())
        s.refreshFl ({});                              // reads FL's database in the background (vendors / favourites)
}

juce::File ChildHost::dataFolder() const { return lemon::PluginScanner::dataFolder(); }
bool ChildHost::isScanning() const noexcept { return scanner != nullptr && scanner->isScanning(); }
bool ChildHost::scannerAvailable() { return lemon::PluginScanner::findScanExe().existsAsFile(); }
int ChildHost::quarantinedCount() const { return lemon::CrashGuard::instance().count(); }
void ChildHost::releaseQuarantine() { lemon::CrashGuard::instance().releaseAll(); }

int ChildHost::instrumentCount()
{
    int n = 0;
    for (auto& t : sc().list().getTypes())
        n += t.isInstrument && t.name != ownName ? 1 : 0;
    return n;
}

void ChildHost::scanFolders (Report report)
{
    auto& s = sc();
    if (s.isScanning())
    {
        if (report)
            report ("The scanner is busy, try again in a moment", true);
        return;
    }

    if (! scannerAvailable())
    {
        if (report)
            report ("LemonScan not found: copy it next to Lynx Rack.vst3 or into " + dataFolder().getFullPathName(), true);
        return;
    }

    juce::WeakReference<ChildHost> weak (this);
    s.scan ({}, [weak, report] (const juce::String& file) {
                if (weak != nullptr && report)
                    report ("Scanning " + juce::File (file).getFileName(), false);
            },
            [weak, report] {
                if (weak == nullptr || ! report)
                    return;
                const auto msg = weak->sc().lastMessage();
                report ((msg.isNotEmpty() ? msg + " - " : juce::String()) + weak->libraryHint(), true);
            });
}

void ChildHost::importFromFl (Report report)
{
    auto& s = sc();
    if (s.isScanning())
    {
        if (report)
            report ("The scanner is busy, try again in a moment", true);
        return;
    }

    if (! scannerAvailable())
    {
        if (report)
            report ("LemonScan not found: copy it next to Lynx Rack.vst3 or into " + dataFolder().getFullPathName(), true);
        return;
    }

    juce::WeakReference<ChildHost> weak (this);
    s.importFromFl ([weak, report] (const juce::String& file) {
                        if (weak != nullptr && report)
                            report ("Importing " + juce::File (file).getFileName(), false);
                    },
                    [weak, report] {
                        if (weak == nullptr || ! report)
                            return;
                        const auto msg = weak->sc().lastMessage();
                        report ((msg.isNotEmpty() ? msg + " - " : juce::String()) + weak->libraryHint(), true);
                    });
}

juce::String ChildHost::libraryHint()
{
    const auto n = instrumentCount();
    return n == 0 ? juce::String ("No instruments known yet: use Scan plugins")
                  : juce::String (n) + (n == 1 ? " instrument" : " instruments") + " in the library";
}

// --- menu -----------------------------------------------------------------------------------------------

juce::PopupMenu ChildHost::makePluginMenu()
{
    using Kind = lemon::PluginScanner::Kind;
    menuEntries = sc().catalog (Kind::instruments);
    menuEntries.erase (std::remove_if (menuEntries.begin(), menuEntries.end(),
                                       [] (const lemon::CatalogEntry& e) { return e.desc.name == ownName; }),
                       menuEntries.end());

    std::sort (menuEntries.begin(), menuEntries.end(), [] (const lemon::CatalogEntry& a, const lemon::CatalogEntry& b) {
        return a.desc.name.compareNatural (b.desc.name, false) < 0;
    });

    juce::PopupMenu menu;

    if (menuEntries.empty())
    {
        menu.addItem (-1, "No instruments known: use Scan plugins", false);
        return menu;
    }

    auto addEntry = [] (juce::PopupMenu& m, const lemon::CatalogEntry& e, int index) {
        auto label = e.desc.name;
        if (e.quarantined)
            label << "  (quarantined)";
        m.addItem (menuBase + index, label, ! e.quarantined);
    };

    // Favourites (the FL favourite folders), then vendors.
    std::map<juce::String, juce::PopupMenu> favourites, vendors;
    for (int i = 0; i < (int) menuEntries.size(); ++i)
    {
        const auto& e = menuEntries[(size_t) i];
        if (e.favourite)
            addEntry (favourites[e.favouriteFolder.isEmpty() ? juce::String ("Favourites") : e.favouriteFolder], e, i);
        addEntry (vendors[e.vendor.isEmpty() ? juce::String ("Unknown vendor") : e.vendor], e, i);
    }

    for (auto& [name, sub] : favourites)
        menu.addSubMenu (name == "Favourites" ? name : "Favourites / " + name, std::move (sub));

    if (! favourites.empty())
        menu.addSeparator();

    for (auto& [name, sub] : vendors)
        menu.addSubMenu (name, std::move (sub));

    return menu;
}

bool ChildHost::descriptionForMenuResult (int menuResult, juce::PluginDescription& out) const
{
    const auto index = menuResult - menuBase;

    if (! juce::isPositiveAndBelow (index, (int) menuEntries.size()))
        return false;

    out = menuEntries[(size_t) index].desc;
    return true;
}

// --- loading --------------------------------------------------------------------------------------------

void ChildHost::setError (int channel, const juce::String& text)
{
    errors[(size_t) channel] = text;
    engine.sendChangeMessage();
}

void ChildHost::loadPlugin (int channel, const juce::PluginDescription& desc, std::function<void (const juce::String&)> done,
                            bool restoreState)
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return;

    if (desc.name == ownName)
    {
        if (done)
            done ("The rack cannot host itself.");
        return;
    }

    const auto id = desc.createIdentifierString();
    auto& guard = lemon::CrashGuard::instance();

    if (guard.isQuarantined (id))
    {
        const auto why = "Quarantined after a crash: " + desc.name + " (" + guard.reason (id) + ")";
        setError (channel, why);
        lemon::hostLog ("load refused: " + why);
        if (done)
            done (why);
        return;
    }

    closeEditor (channel);
    errors[(size_t) channel] = {};
    const auto serial = ++loadSerial[(size_t) channel];
    const auto state = restoreState ? engine.song().channels[(size_t) channel].pluginState : juce::MemoryBlock();
    const auto xml = desc.createXml() != nullptr ? desc.createXml()->toString() : juce::String();
    juce::WeakReference<ChildHost> weak (this);

    lemon::hostLog ("load ch" + juce::String (channel + 1) + " " + desc.name);
    guard.begin (this, id, "load", desc.name);

    sc().formats().createPluginInstanceAsync (desc, engine.sampleRateNow(), engine.blockSizeNow(),
        [weak, channel, serial, state, xml, id, name = desc.name, done] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
            auto& g = lemon::CrashGuard::instance();
            g.end (weak.get(), id, "load");

            if (weak == nullptr || weak->loadSerial[(size_t) channel] != serial)
                return;    

            if (instance == nullptr)
            {
                const auto why = error.isEmpty() ? "Could not load " + name : error;
                weak->setError (channel, why);
                lemon::hostLog ("load failed ch" + juce::String (channel + 1) + ": " + why);
                if (done)
                    done (why);
                return;
            }

            
            weak->engine.prepareChild (*instance);

            if (state.getSize() > 0)
            {
                g.begin (weak.get(), id, "state", name);
                instance->setStateInformation (state.getData(), (int) state.getSize());
                g.end (weak.get(), id, "state");
            }

            weak->engine.editChannel (channel, [&] (ChannelInfo& c) {
                if (c.name.startsWith ("Channel "))
                    c.name = name;
                c.pluginXml = xml;
                if (state.getSize() == 0)
                    c.pluginState = {};
            });
            weak->engine.setChannelPlugin (channel, std::move (instance), true);
            lemon::hostLog ("loaded ch" + juce::String (channel + 1) + " " + name);

            if (done)
                done ({});
        });
}

void ChildHost::unloadPlugin (int channel)
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return;

    ++loadSerial[(size_t) channel];
    closeEditor (channel);
    errors[(size_t) channel] = {};
    engine.editChannel (channel, [] (ChannelInfo& c) {
        c.pluginXml = {};
        c.pluginState = {};
    });
    engine.setChannelPlugin (channel, nullptr);
}

void ChildHost::restoreFromSong()
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        errors[(size_t) ch] = {};
        const auto xmlText = engine.song().channels[(size_t) ch].pluginXml;

        if (xmlText.isEmpty())
        {
            ++loadSerial[(size_t) ch];
            engine.setChannelPlugin (ch, nullptr);
            continue;
        }

        if (auto xml = juce::XmlDocument::parse (xmlText))
        {
            juce::PluginDescription desc;

            if (desc.loadFromXml (*xml))
            {
                loadPlugin (ch, desc, {}, true);
                continue;
            }
        }
        setError (ch, "Saved plugin description could not be read");
    }
}

juce::String ChildHost::pluginName (int channel) const
{
    if (auto* p = engine.channelPlugin (channel))
        return p->getName();

    if (juce::isPositiveAndBelow (channel, maxChannels))
        if (auto xml = juce::XmlDocument::parse (engine.song().channels[(size_t) channel].pluginXml))
            return xml->getStringAttribute ("name");

    return {};
}

juce::String ChildHost::channelStatus (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return {};

    if (engine.hasCrashed (channel))
        return "Stopped after a crash: " + pluginName (channel);

    if (errors[(size_t) channel].isNotEmpty())
        return errors[(size_t) channel];

    if (engine.channelPlugin (channel) == nullptr && pluginName (channel).isNotEmpty())
        return "Loading " + pluginName (channel) + "...";

    return {};
}

// --- editor windows -------------------------------------------------------------------------------------

bool ChildHost::isEditorOpen (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxChannels) && windows[(size_t) channel] != nullptr;
}

void ChildHost::toggleEditor (int channel)
{
    if (! juce::isPositiveAndBelow (channel, maxChannels))
        return;

    if (windows[(size_t) channel] != nullptr)
    {
        closeEditor (channel);
        return;
    }

    auto* plugin = engine.channelPlugin (channel);

    if (plugin == nullptr)
        return;

    const auto id = lemon::CrashGuard::idFor (plugin);
    auto& guard = lemon::CrashGuard::instance();
    guard.begin (this, id, "gui", plugin->getName());
    juce::AudioProcessorEditor* editor = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*plugin);

    juce::WeakReference<ChildHost> weak (this);
    windows[(size_t) channel] = std::make_unique<PluginWindow> (editor, plugin->getName() + "  -  " + engine.song().channels[(size_t) channel].name,
                                                                [weak, channel] {
                                                                    juce::MessageManager::callAsync ([weak, channel] {
                                                                        if (weak != nullptr)
                                                                            weak->closeEditor (channel);
                                                                    });
                                                                });
    guard.end (this, id, "gui");
}

void ChildHost::closeEditor (int channel)
{
    if (juce::isPositiveAndBelow (channel, maxChannels))
        windows[(size_t) channel].reset();    
}

void ChildHost::closeAllEditors()
{
    for (auto& w : windows)
        w.reset();
}
} // namespace lynx
