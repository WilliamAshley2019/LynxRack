#include "PluginScanner.h"
#include "HostLog.h"
#include "CrashGuard.h"
#include "PluginDb.h"

namespace lemon
{
juce::File PluginScanner::dataFolder() { return CrashGuard::dataFolder(); }

PluginScanner::PluginScanner() : juce::Thread("Lemon plugin scan")
{
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());   // needs JUCE_PLUGINHOST_VST3=1
    dataFolder().createDirectory();
    if (auto xml = juce::XmlDocument::parse(dataFolder().getChildFile("plugins.xml")))
        known.recreateFromXml(*xml);
    blacklistLeftovers(dataFolder().getChildFile("scan.pedal"));      // a scan that died last time
}

PluginScanner::~PluginScanner()
{
    requestStop();                      // makes the scan loop kill its child processes and return
    stopThread(1500);                   // normally already stopped by acquire()'s deleter
    if (dirty.load()) save();
}

std::shared_ptr<PluginScanner> PluginScanner::acquire()
{
    static std::mutex m;
    static std::weak_ptr<PluginScanner> weak;
    std::lock_guard<std::mutex> g(m);
    if (auto s = weak.lock()) return s;
    std::shared_ptr<PluginScanner> s(new PluginScanner(), [](PluginScanner* p)
    {
        const auto t0 = juce::Time::getMillisecondCounter();
        p->requestStop();
        if (p->waitStopped(3000)) { delete p; hostLog("scanner stopped in " + juce::String((int) (juce::Time::getMillisecondCounter() - t0)) + " ms"); }
        else hostLog("scanner did not stop within 3 s: leaving it to finish on its own");   // never kill a thread mid-IO
    });
    weak = s;
    return s;
}

void PluginScanner::save()
{
    dirty = false;
    if (auto xml = known.createXml())
        xml->writeTo(dataFolder().getChildFile("plugins.xml"));
}

juce::File PluginScanner::flRoot() const
{
    auto s = dataFolder().getChildFile("fl_database_path.txt").loadFileAsString().trim();
    return s.isNotEmpty() ? juce::File(s) : defaultFlDatabaseRoot();
}

void PluginScanner::setFlRoot(const juce::File& f)
{
    dataFolder().getChildFile("fl_database_path.txt").replaceWithText(f.getFullPathName());
}

juce::File PluginScanner::orgPath() const
{
    auto t = dataFolder().getChildFile("organizer_db_path.txt").loadFileAsString().trim();
    return t.isNotEmpty() ? juce::File(t) : juce::File();
}

void PluginScanner::setOrgPath(const juce::File& f)
{
    auto file = dataFolder().getChildFile("organizer_db_path.txt");
    if (f == juce::File()) file.deleteFile(); else file.replaceWithText(f.getFullPathName());
}

std::shared_ptr<const data::OrgResult> PluginScanner::orgData() const
{
    std::lock_guard<std::mutex> g(flLock);
    return org;
}

void PluginScanner::loadSources()
{
    auto data = std::make_shared<const FlData>(readFlDatabase(flRoot(), noCancel));
    std::shared_ptr<const data::OrgResult> orgResult;
    if (auto f = orgPath(); f != juce::File())
    {
        auto r = std::make_shared<data::OrgResult>(data::readOrganizerDb(f.getFullPathName().toStdString()));
        if (r->schema.size()) dataFolder().getChildFile("organizer_schema.txt").replaceWithText(juce::String(r->schema));
        orgResult = r;
    }
    std::lock_guard<std::mutex> g(flLock);
    fl = data;
    org = orgResult;
}

std::shared_ptr<const FlData> PluginScanner::flData() const
{
    std::lock_guard<std::mutex> g(flLock);
    return fl;
}

void PluginScanner::blacklistLeftovers(const juce::File& pedal)
{
    if (! pedal.existsAsFile()) return;
    for (auto& line : juce::StringArray::fromLines(pedal.loadFileAsString()))
        if (line.isNotEmpty())
            known.addToBlacklist(line);
    pedal.deleteFile();
}

void PluginScanner::start(Mode m, std::function<void(const juce::String&)> progress, std::function<void()> done)
{
    if (isThreadRunning()) return;
    mode = m;
    onProgress = std::move(progress);
    onDone = std::move(done);
    startThread();
}

void PluginScanner::scan(const juce::StringArray& folders, std::function<void(const juce::String&)> progress, std::function<void()> done)
{
    foldersToScan = folders;
    start(Mode::Folders, std::move(progress), std::move(done));
}

void PluginScanner::importFromFl(std::function<void(const juce::String&)> progress, std::function<void()> done)
{
    start(Mode::ImportFl, std::move(progress), std::move(done));
}

void PluginScanner::refreshFl(std::function<void()> done)
{
    start(Mode::RefreshFl, {}, std::move(done));
}

std::vector<juce::String> PluginScanner::missingPaths() const
{
    std::set<juce::String> have;
    for (auto& t : known.getTypes()) have.insert(normalisePath(t.fileOrIdentifier));
    for (auto& b : known.getBlacklistedFiles()) have.insert(normalisePath(b));

    std::set<juce::String> seen;
    std::vector<juce::String> out;
    auto consider = [&](const juce::String& path)
    {
        if (noCancel.load() || ! path.endsWithIgnoreCase(".vst3") || path.contains("%")) return;     // only VST3 can be hosted here
        const auto n = normalisePath(path);
        if (have.count(n) || ! seen.insert(n).second) return;
        if (! juce::File(path).exists()) return;
        out.push_back(path);
    };
    if (auto d = flData()) for (auto& p : d->installed) consider(p.path);
    if (auto o = orgData()) for (auto& p : o->plugins) consider(juce::String(p.path));
    return out;
}


juce::String PluginScanner::lastMessage() const { std::lock_guard<std::mutex> g(msgLock); return message; }
void PluginScanner::setMessage(const juce::String& m) { std::lock_guard<std::mutex> g(msgLock); message = m; }

void PluginScanner::recomputeMissing()
{
    missingCount = (int) missingPaths().size();
}

juce::File PluginScanner::findScanExe()
{
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);   // the plugin DLL when running inside a host
   #if JUCE_WINDOWS
    const char* name = "LemonScan.exe";
   #else
    const char* name = "LemonScan";
   #endif
    // plugin DLL = <bundle>/Contents/<arch>/X.vst3, so also look next to the bundle and three levels up
    for (auto& f : { self.getSiblingFile(name), dataFolder().getChildFile(name),
                     self.getParentDirectory().getParentDirectory().getSiblingFile(name),
                     self.getParentDirectory().getParentDirectory().getParentDirectory().getSiblingFile(name) })
        if (f.existsAsFile()) return f;
    return {};
}

// Every plugin file is examined by its own LemonScan.exe process: a crash, a hang or a modal dialog inside a
// plugin (or a plugin that needs a different CRT heap) cannot touch the host, and a file that misbehaves is
// blacklisted instead of taking FL Studio down. Several run at once; each has a timeout and is killed when it expires.
void PluginScanner::scanIsolated(const juce::StringArray& files, bool& anyAdded)
{
    if (files.isEmpty()) { setMessage("Nothing new to scan"); return; }
    const auto exe = findScanExe();
    if (! exe.existsAsFile())
    {
        setMessage("LemonScan.exe not found: copy it next to the plugin or into " + dataFolder().getFullPathName()
                   + ". Scanning is done out of process so a bad plugin cannot crash your DAW.");
        return;
    }

    struct Job
    {
        juce::ChildProcess proc;
        juce::String file;
        juce::File out;
        juce::uint32 startMs = 0;
        juce::uint32 timeoutMs = 30000;
    };
    constexpr int kParallel = 3;
    std::vector<std::unique_ptr<Job>> active;
    juce::StringArray failures;
    size_t next = 0;
    int sinceSave = 0, done = 0;
    auto failLog = dataFolder().getChildFile("scan_failures.txt");

    auto finish = [&](Job& j, bool timedOut)
    {
        bool ok = false;
        // LemonScan only writes its result file on success, so the file is the verdict (ChildProcess::getExitCode() is
        // unreliable on Linux/macOS once isRunning() has reaped the child).
        if (! timedOut && j.out.existsAsFile())
            if (auto xml = juce::XmlDocument::parse(j.out))
            {
                ok = true;
                for (auto* e : xml->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (d.loadFromXml(*e)) { known.addType(d); anyAdded = true; dirty = true; }
                }
            }
        j.out.deleteFile();
        if (! ok)
        {
            known.addToBlacklist(j.file);
            dirty = true;
            failures.add(j.file + (timedOut ? "  (timed out)" : "  (exit " + juce::String(j.proc.getExitCode()) + ")"));
        }
        if (++sinceSave >= 40) { save(); sinceSave = 0; }
    };

    while (! threadShouldExit() && ! noCancel.load() && (next < (size_t) files.size() || ! active.empty()))
    {
        while ((int) active.size() < kParallel && next < (size_t) files.size() && ! threadShouldExit())
        {
            auto j = std::make_unique<Job>();
            j->file = files[(int) next++];
            j->out = dataFolder().getChildFile("scan_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) + ".xml");
            const auto lower = j->file.toLowerCase();
            // Shell-style bundles (e.g. Waves) declare hundreds of plugins and need far longer than a normal file.
            if (lower.contains("waveshell") || lower.contains("shell")) j->timeoutMs = 20 * 60 * 1000;
            j->startMs = juce::Time::getMillisecondCounter();
            if (onProgress) juce::MessageManager::callAsync([cb = onProgress, f = j->file] { cb(f); });
            if (j->proc.start(juce::StringArray { exe.getFullPathName(), j->file, j->out.getFullPathName() }, 0))
                active.push_back(std::move(j));
            else { known.addToBlacklist(j->file); failures.add(j->file + "  (could not start scanner)"); }
        }
        for (size_t i = 0; i < active.size();)
        {
            auto& j = *active[i];
            const bool running = j.proc.isRunning();
            const bool expired = running && juce::Time::getMillisecondCounter() - j.startMs > j.timeoutMs;
            if (! running || expired)
            {
                if (expired) j.proc.kill();
                finish(j, expired);
                ++done;
                active.erase(active.begin() + (long) i);
            }
            else ++i;
        }
        wait(40);
    }
    for (auto& j : active) { j->proc.kill(); j->out.deleteFile(); }

    if (failures.size() > 0)
        failLog.appendText(juce::Time::getCurrentTime().toString(true, true) + "\n" + failures.joinIntoString("\n") + "\n");
    setMessage(juce::String(done) + " files examined, " + juce::String(failures.size()) + " failed (see scan_failures.txt)");
}

void PluginScanner::run()
{
    auto pedal = dataFolder().getChildFile("scan.pedal");

    if (mode == Mode::RefreshFl)
    {
        loadSources();
    }
    else if (mode == Mode::ImportFl)
    {
        loadSources();
        bool added = false;
        juce::StringArray files;
        for (auto& p : missingPaths()) files.add(p);
        scanIsolated(files, added);
    }
    else
    {
        blacklistLeftovers(pedal);
        bool added = false;
        juce::StringArray files;
        for (int i = 0; i < formatManager.getNumFormats() && ! threadShouldExit(); ++i)
        {
            auto* fmt = formatManager.getFormat(i);
            juce::FileSearchPath paths;
            if (foldersToScan.isEmpty()) paths = fmt->getDefaultLocationsToSearch();
            else for (auto& f : foldersToScan) paths.add(juce::File(f));

            auto blacklisted = known.getBlacklistedFiles();
            for (auto& f : fmt->searchPathsForPlugins(paths, true, false))
            {
                if (blacklisted.contains(f) || known.isListingUpToDate(f, *fmt)) continue;
                files.add(f);
            }
        }
        scanIsolated(files, added);
    }

    recomputeMissing();
    if (! threadShouldExit() && mode != Mode::RefreshFl)      // never do heavy exports while being shut down / on a plain refresh
    {
        save();
        exportCatalog();
    }
    if (onDone)
        juce::MessageManager::callAsync([cb = onDone] { cb(); });
}

std::vector<CatalogEntry> PluginScanner::catalog (Kind kind) const
{
    std::vector<CatalogEntry> out;
    auto d = flData();

    std::map<juce::String, std::vector<const FlPlugin*>> byPath;
    if (d) for (auto& p : d->installed) byPath[normalisePath(p.path)].push_back(&p);

    auto org = orgData();
    std::map<juce::String, const data::OrgPlugin*> orgByPath;
    if (org) for (auto& p : org->plugins) orgByPath[normalisePath(juce::String(p.path))] = &p;

    auto& guard = CrashGuard::instance();
    for (auto& t : known.getTypes())
    {
        const bool isInst = t.isInstrument;
        if (kind == Kind::effects && (isInst || t.numInputChannels <= 0)) continue;   // effects only (Lemon Mixer Rack)
        if (kind == Kind::instruments && ! isInst) continue;
        CatalogEntry e;
        e.instrument = isInst;
        e.desc = t;
        e.id = t.createIdentifierString();
        e.vendor = t.manufacturerName;
        e.quarantined = guard.isQuarantined(e.id);

        if (d)
        {
            auto it = byPath.find(normalisePath(t.fileOrIdentifier));
            if (it != byPath.end())
            {
                const FlPlugin* hit = it->second.front();
                for (auto* p : it->second) if (p->name.equalsIgnoreCase(t.name)) { hit = p; break; }
                e.inFl = true;
                e.flCategory = hit->category;
                if (e.vendor.isEmpty()) e.vendor = hit->vendor;
            }
            auto fav = d->favourite.find(t.name.toLowerCase());
            if (fav != d->favourite.end()) { e.favourite = true; e.favouriteFolder = fav->second; }
        }
        if (auto it = orgByPath.find(normalisePath(t.fileOrIdentifier)); it != orgByPath.end())
        {
            e.inOrganizer = true;
            e.orgCategory = juce::String(it->second->category);
            if (e.vendor.isEmpty()) e.vendor = juce::String(it->second->vendor);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// A plain JSON snapshot other AlphaAudio tools (e.g. FLP Organizer) can read without a shared schema.
void PluginScanner::exportCatalog()
{
    juce::Array<juce::var> arr, arrInst;
    std::vector<data::PluginRecord> records;
    auto cat = catalog (Kind::all);
    for (auto& e : cat)
    {
        data::PluginRecord r;
        r.id = e.id.toStdString(); r.name = e.desc.name.toStdString(); r.vendor = e.vendor.toStdString();
        r.format = e.desc.pluginFormatName.toStdString(); r.path = e.desc.fileOrIdentifier.toStdString();
        r.flCategory = e.flCategory.toStdString(); r.flFolder = e.favouriteFolder.toStdString();
        r.orgCategory = e.orgCategory.toStdString();
        r.inFl = e.inFl; r.inOrg = e.inOrganizer; r.quarantined = e.quarantined; r.instrument = e.instrument;
        if (e.quarantined) r.reason = CrashGuard::instance().reason(e.id).toStdString();
        records.push_back(std::move(r));

        auto* o = new juce::DynamicObject();
        o->setProperty("name", e.desc.name);
        o->setProperty("vendor", e.vendor);
        o->setProperty("format", e.desc.pluginFormatName);
        o->setProperty("path", e.desc.fileOrIdentifier);
        o->setProperty("flCategory", e.flCategory);
        o->setProperty("flFavouriteFolder", e.favouriteFolder);
        o->setProperty("inFlDatabase", e.inFl);
        o->setProperty("inOrganizerDatabase", e.inOrganizer);
        o->setProperty("organizerCategory", e.orgCategory);
        o->setProperty("quarantined", e.quarantined);
        (e.instrument ? arrInst : arr).add(juce::var(o));
    }
    if (data::sqliteAvailable())
    {
        data::LemonDb db(dataFolder().getChildFile("lemon_plugins.db").getFullPathName().toStdString());
        if (db.ok()) db.sync(records);
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("generatedBy", "AlphaAudio shared scanner");
    root->setProperty("plugins", juce::var(arr));            // effects: unchanged for existing readers
    root->setProperty("instruments", juce::var(arrInst));    // additive
    dataFolder().getChildFile("plugin_catalog.json").replaceWithText(juce::JSON::toString(juce::var(root)));
}
}
