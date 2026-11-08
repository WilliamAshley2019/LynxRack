// End-to-end test of the scanner + ChildHost + engine with an isolated HOME and a real VST3 instrument:
//   argv[1] = LemonScan executable, argv[2] = the binary folder inside LynxTestSynth.vst3 (Contents/<arch>)
#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdlib>
#include <iostream>

#include "../common/CrashGuard.h"
#include "../core/ChildHost.h"
#include "../core/RackEngine.h"

using namespace lynx;

namespace
{
int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "FAIL: " << msg << "  (" #cond ")\n"; } else std::cout << "ok   " << msg << "\n"; } while (0)

template <typename F>
bool waitFor (F&& cond, int ms = 60000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (! cond() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    return cond();
}

struct Rig
{
    static constexpr double sr = 48000.0;
    static constexpr int block = 256;
    RackEngine engine;
    ParamRefs refs;
    std::array<std::atomic<float>, maxChannels> vol, pan, mute, solo;
    std::atomic<float> pattern { 0 }, swing { 0 }, mode { 1 }, tempo { 120 };
    juce::AudioBuffer<float> buffer { 2, block };
    std::array<int, maxChannels + 1> first {};

    Rig()
    {
        for (int i = 0; i < maxChannels; ++i)
        {
            vol[(size_t) i] = 0.0f; pan[(size_t) i] = 0.0f; mute[(size_t) i] = 0.0f; solo[(size_t) i] = 0.0f;
            refs.volDb[(size_t) i] = &vol[(size_t) i]; refs.pan[(size_t) i] = &pan[(size_t) i];
            refs.mute[(size_t) i] = &mute[(size_t) i]; refs.solo[(size_t) i] = &solo[(size_t) i];
        }
        refs.pattern = &pattern; refs.swing = &swing; refs.mode = &mode; refs.tempo = &tempo;
        engine.setParams (refs);
        engine.prepare (sr, block);
        first.fill (-1);
        first[0] = 0;
    }

    // renders `blocks` blocks, returns index of the first non-silent left sample (or -1) and the peak
    std::pair<long, float> render (int blocks)
    {
        long firstNonZero = -1, pos = 0;
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            juce::MidiBuffer m;
            engine.process (buffer, first, m, {});
            for (int i = 0; i < block; ++i, ++pos)
            {
                const auto v = std::abs (buffer.getSample (0, i));
                peak = std::max (peak, v);
                if (v > 1.0e-4f && firstNonZero < 0) firstNonZero = pos;
            }
        }
        return { firstNonZero, peak };
    }
};
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3) { std::cout << "usage: HostTest <LemonScan> <LynxTestSynth.vst3>\n"; return 2; }

    const auto home = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lynx_hosttest_" + juce::String (juce::Time::currentTimeMillis()));
    home.createDirectory();
    setenv ("HOME", home.getFullPathName().toRawUTF8(), 1);

    juce::ScopedJuceInitialiser_GUI init;
    CHECK (juce::File::getSpecialLocation (juce::File::userHomeDirectory) == home, "HOME is isolated");

    const juce::File scanExe (argv[1]), bundle = juce::File (argv[2]).getParentDirectory().getParentDirectory();   // <x>.vst3/Contents/<arch>/
    const auto data = lemon::PluginScanner::dataFolder();
    data.createDirectory();
    CHECK (data.getFullPathName().startsWith (home.getFullPathName()), "data folder lives inside the isolated HOME");
    CHECK (bundle.copyDirectoryTo (home.getChildFile (".vst3").getChildFile (bundle.getFileName())), "test synth installed in ~/.vst3");

    data.getChildFile ("active_2147483000.txt").replaceWithText ("FakeDeadPlugin-id\tload\tFakeDead\n");   // a host (pid that cannot exist) died mid-load
    // 0. no LemonScan anywhere: the scan refuses politely, nothing crashes
    {
        Rig r;
        ChildHost host (r.engine);
        juce::String msg;
        bool finished = false;
        host.scanFolders ([&] (const juce::String& t, bool f) { msg = t; finished = f; });
        CHECK (finished && msg.contains ("LemonScan not found"), "missing LemonScan is reported: " + msg);
        CHECK (host.instrumentCount() == 0, "library starts empty");
        CHECK (host.libraryHint().contains ("No instruments"), "hint tells the user to scan");
    }

    CHECK (scanExe.copyFileTo (data.getChildFile (scanExe.getFileName())), "LemonScan placed in the data folder");
    data.getChildFile (scanExe.getFileName()).setExecutePermission (true);   // copyFileTo drops the +x bit on Linux

    // (set up before the first CrashGuard use) a previous host died while loading a plugin: adopted on start
    {
        auto& guard = lemon::CrashGuard::instance();
        CHECK (guard.isQuarantined ("FakeDeadPlugin-id"), "plugin that was loading when a host died is quarantined at next start");
        CHECK (guard.reason ("FakeDeadPlugin-id").contains ("loading when the host last crashed"), "with a readable reason: " + guard.reason ("FakeDeadPlugin-id"));
        CHECK (! data.getChildFile ("active_2147483000.txt").existsAsFile(), "dead host's mark file is consumed");
        guard.release ("FakeDeadPlugin-id");
    }

    Rig rig;
    ChildHost host (rig.engine);

    // 1. out-of-process scan finds the instrument
    {
        host.warmUp();
        CHECK (waitFor ([&] { return ! host.isScanning(); }), "background FL-database read finishes");
        juce::String last;
        bool finished = false;
        host.scanFolders ([&] (const juce::String& t, bool f) { last = t; finished = f; });
        CHECK (waitFor ([&] { return finished; }), "scan finishes");
        std::cout << "     " << last << "\n";
        CHECK (host.instrumentCount() == 1, "exactly one instrument found");
        CHECK (data.getChildFile ("plugins.xml").existsAsFile(), "plugins.xml written");
        CHECK (data.getChildFile ("plugin_catalog.json").existsAsFile(), "plugin_catalog.json written");
        const auto json = juce::JSON::parse (data.getChildFile ("plugin_catalog.json"));
        CHECK (json["instruments"].getArray() != nullptr && json["instruments"].getArray()->size() == 1, "catalog JSON has the instrument under \"instruments\"");
        CHECK (json["plugins"].getArray() != nullptr && json["plugins"].getArray()->isEmpty(), "\"plugins\" (effects) key stays effects-only");
        if (lemon::data::sqliteAvailable())
        {
            lemon::data::LemonDb db (data.getChildFile ("lemon_plugins.db").getFullPathName().toStdString());
            CHECK (db.ok() && db.count() == 1, "SQLite catalog has one row");
        }
        else
            std::cout << "skip SQLite catalog (not in this build)\n";

        // a second scan does not rescan or duplicate
        finished = false;
        host.scanFolders ([&] (const juce::String&, bool f) { finished = f; });
        CHECK (waitFor ([&] { return finished; }), "second scan finishes");
        CHECK (host.instrumentCount() == 1, "second scan adds no duplicate");
    }

    // 2. the slot menu offers it
    juce::PluginDescription desc;
    {
        auto menu = host.makePluginMenu();
        CHECK (menu.getNumItems() > 0, "plugin menu is not empty");
        CHECK (host.descriptionForMenuResult (ChildHost::menuBase, desc) && desc.name == "LynxTestSynth", "menu id maps back to the plugin");
        CHECK (! host.descriptionForMenuResult (ChildHost::menuBase + 5, desc) && ! host.descriptionForMenuResult (3, desc), "foreign / out-of-range ids are rejected");
        host.descriptionForMenuResult (ChildHost::menuBase, desc);
    }

    // 3. load into channel 1 and play a note at step 2 (2 steps = 480 ticks = 0.25 s at 120 bpm = 12000 samples)
    {
        juce::String err = "pending";
        host.loadPlugin (0, desc, [&] (const juce::String& e) { err = e; });
        CHECK (waitFor ([&] { return err != "pending"; }) && err.isEmpty(), "plugin loads: " + err);
        CHECK (rig.engine.channelPlugin (0) != nullptr, "engine holds the plugin");
        CHECK (rig.engine.song().channels[0].name == "LynxTestSynth", "channel took the plugin's name");

        rig.engine.editPattern (0, [] (Pattern& p) {
            Note n; n.start = 2 * ticksPerStep; n.length = 2 * ticksPerStep; n.channel = 0; n.key = 69; p.add (n);
        });
        rig.engine.setInternalPlaying (true);
        const auto [firstSample, peak] = rig.render (80);
        std::cout << "     first sound at sample " << firstSample << ", peak " << peak << "\n";
        CHECK (std::abs (firstSample - 12000) <= 1, "hosted instrument sounds on the exact sample");
        CHECK (peak > 0.2f && peak < 0.3f, "output level is the synth's 0.25");
        rig.engine.setInternalPlaying (false);
    }

    // 4. state round trip: applied after prepare, before publishing
    {
        auto* p = rig.engine.channelPlugin (0);
        rig.engine.captureStates();
        const auto saved = rig.engine.song();                            // what getStateInformation would persist
        CHECK (saved.channels[0].pluginState.getSize() > 0, "state captured");

        Rig rig2;
        ChildHost host2 (rig2.engine);
        RackSong copy = saved;
        rig2.engine.setSong (std::move (copy));
        host2.restoreFromSong();
        CHECK (waitFor ([&] { return rig2.engine.channelPlugin (0) != nullptr; }), "project reload restores the plugin");
        if (auto* q = rig2.engine.channelPlugin (0))
        {
            juce::MemoryBlock mb;
            q->getStateInformation (mb);
            // the VST3 wrapper embeds the plugin's own 5 bytes (payload + "applied before prepare" flag) in its chunk
            const auto find = [] (const juce::MemoryBlock& m, const char* tag) -> int {
                for (size_t i = 0; i + 4 < m.getSize(); ++i)
                    if (std::memcmp (static_cast<const char*> (m.getData()) + i, tag, 4) == 0) return (int) i;
                return -1;
            };
            // JUCE's VST3 host wrapper stores the plugin's own state as base64 inside an XML chunk <IComponent>
            juce::MemoryBlock inner;
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()))
                if (auto* c = xml->getChildByName ("IComponent"))
                    inner.fromBase64Encoding (c->getAllSubText());
            const int at = find (inner, "SYNT");
            CHECK (at >= 0, "plugin state restored (payload found in the plugin's state)");
            CHECK (at >= 0 && inner[(size_t) at + 5] == 1, "saved state was actually applied to the reloaded plugin");
            CHECK (at >= 0 && inner[(size_t) at + 4] == 0, "state was applied after prepareToPlay");
        }
        CHECK (host2.pluginName (0) == "LynxTestSynth", "name available");
        CHECK (host2.channelStatus (0).isEmpty(), "no status text when healthy");
    }

    // 5. quarantine
    {
        auto& guard = lemon::CrashGuard::instance();
        const auto id = desc.createIdentifierString();
        guard.quarantine (id, desc.name, "test");
        juce::String err = "pending";
        host.loadPlugin (1, desc, [&] (const juce::String& e) { err = e; });
        CHECK (err.contains ("Quarantined"), "quarantined plugin refuses to load: " + err);
        CHECK (host.channelStatus (1).contains ("Quarantined"), "channel shows why");
        CHECK (host.quarantinedCount() == 1, "count reported");
        host.releaseQuarantine();
        err = "pending";
        host.loadPlugin (1, desc, [&] (const juce::String& e) { err = e; });
        CHECK (waitFor ([&] { return err != "pending"; }) && err.isEmpty(), "released plugin loads again");
    }

    // 7. unload
    host.unloadPlugin (0);
    CHECK (rig.engine.channelPlugin (0) == nullptr && rig.engine.song().channels[0].pluginXml.isEmpty(), "unload clears plugin and song entry");

    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES: " + juce::String (failures) + "\n");
    home.deleteRecursively();
    return failures == 0 ? 0 : 1;
}
