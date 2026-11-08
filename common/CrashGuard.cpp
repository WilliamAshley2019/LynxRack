#include "CrashGuard.h"

#include <algorithm>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <errno.h>
 #include <signal.h>
#endif

namespace lemon
{
namespace
{
bool processAlive(juce::int64 pid)
{
    if (pid <= 0) return false;
   #if JUCE_WINDOWS
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
    if (h == nullptr) return false;
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
   #else
    return kill((pid_t) pid, 0) == 0 || errno == EPERM;
   #endif
}

juce::int64 thisPid()
{
   #if JUCE_WINDOWS
    return (juce::int64) GetCurrentProcessId();
   #else
    return (juce::int64) getpid();
   #endif
}
}

juce::File CrashGuard::dataFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("AlphaAudio").getChildFile("LemonMixerRack");
}

CrashGuard& CrashGuard::instance()
{
    static CrashGuard g;
    return g;
}

juce::String CrashGuard::idFor(juce::AudioProcessor* p)
{
    if (auto* inst = dynamic_cast<juce::AudioPluginInstance*>(p))
        return inst->getPluginDescription().createIdentifierString();
    return {};
}

CrashGuard::CrashGuard()
{
    auto dir = dataFolder();
    dir.createDirectory();
    activeFile = dir.getChildFile("active_" + juce::String(thisPid()) + ".txt");
    quarantineFile = dir.getChildFile("quarantine.txt");
    loadQuarantine();
    adoptCrashedSessions();
}

// One line per entry, tab separated.
void CrashGuard::loadQuarantine()
{
    for (auto& line : juce::StringArray::fromLines(quarantineFile.loadFileAsString()))
    {
        auto f = juce::StringArray::fromTokens(line, "\t", "");
        if (f.size() >= 3 && f[0].isNotEmpty())
            quarantined[f[0]] = { f[1], f[2] };
    }
}

void CrashGuard::saveQuarantine()
{
    juce::String s;
    for (auto& [id, q] : quarantined)
        s << id << "\t" << q.name << "\t" << q.why << "\n";
    if (s.isEmpty()) quarantineFile.deleteFile();
    else quarantineFile.replaceWithText(s);
}

void CrashGuard::adoptCrashedSessions()
{
    bool changed = false;
    for (auto& f : dataFolder().findChildFiles(juce::File::findFiles, false, "active_*.txt"))
    {
        const auto pid = f.getFileNameWithoutExtension().fromFirstOccurrenceOf("active_", false, false).getLargeIntValue();
        if (pid == thisPid() || processAlive(pid))
            continue;                                   // a live host: its marks are in use right now
        for (auto& line : juce::StringArray::fromLines(f.loadFileAsString()))
        {
            auto t = juce::StringArray::fromTokens(line, "\t", "");
            if (t.size() >= 3 && t[0].isNotEmpty() && quarantined.find(t[0]) == quarantined.end())
            {
                quarantined[t[0]] = { t[2], t[1] == "gui" ? "its window was open when the host last crashed"
                                                          : "it was loading when the host last crashed" };
                changed = true;
            }
        }
        f.deleteFile();
    }
    if (changed) saveQuarantine();
}

void CrashGuard::writeActive()
{
    if (marks.empty()) { activeFile.deleteFile(); return; }
    juce::String s;
    for (auto& m : marks) s << m.id << "\t" << m.stage << "\t" << m.name << "\n";
    // The id is first so adoptCrashedSessions() can read it; stage second.
    activeFile.replaceWithText(s);
}

void CrashGuard::begin(const void* owner, const juce::String& id, const juce::String& stage, const juce::String& name)
{
    if (id.isEmpty()) return;
    std::lock_guard<std::mutex> g(lock);
    marks.push_back({ owner, id, stage, name });
    writeActive();
}

void CrashGuard::end(const void* owner, const juce::String& id, const juce::String& stage)
{
    std::lock_guard<std::mutex> g(lock);
    for (size_t i = 0; i < marks.size(); ++i)
        if (marks[i].owner == owner && marks[i].id == id && marks[i].stage == stage)
        {
            marks.erase(marks.begin() + (std::ptrdiff_t) i);
            break;
        }
    writeActive();
}

void CrashGuard::endOwned(const void* owner)
{
    std::lock_guard<std::mutex> g(lock);
    marks.erase(std::remove_if(marks.begin(), marks.end(), [owner](const Mark& m) { return m.owner == owner; }), marks.end());
    writeActive();
}

bool CrashGuard::isQuarantined(const juce::String& id) const
{
    std::lock_guard<std::mutex> g(lock);
    return quarantined.find(id) != quarantined.end();
}

juce::String CrashGuard::reason(const juce::String& id) const
{
    std::lock_guard<std::mutex> g(lock);
    auto it = quarantined.find(id);
    return it == quarantined.end() ? juce::String() : it->second.name + ": " + it->second.why;
}

void CrashGuard::quarantine(const juce::String& id, const juce::String& name, const juce::String& why)
{
    std::lock_guard<std::mutex> g(lock);
    quarantined[id] = { name, why };
    saveQuarantine();
}

void CrashGuard::release(const juce::String& id)
{
    std::lock_guard<std::mutex> g(lock);
    quarantined.erase(id);
    saveQuarantine();
}

void CrashGuard::releaseAll()
{
    std::lock_guard<std::mutex> g(lock);
    quarantined.clear();
    saveQuarantine();
}

int CrashGuard::count() const
{
    std::lock_guard<std::mutex> g(lock);
    return (int) quarantined.size();
}
}
