#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <mutex>

namespace lemon
{
// Process-wide crash guard for hosted plugins.
//
// Before anything risky (instantiating a plugin, restoring its state, opening its GUI) the caller
// marks it with begin(); end() clears the mark. The marks live in active_<pid>.txt. If the host
// crashes, that file survives. The next time any Lemon Mixer Rack starts it finds active files whose
// process is gone and quarantines every plugin named in them, so a project does not crash-loop.
class CrashGuard
{
public:
    static CrashGuard& instance();

    static juce::String idFor(juce::AudioProcessor*);                  // stable id of a hosted instance
    static juce::File dataFolder();

    void begin(const void* owner, const juce::String& id, const juce::String& stage, const juce::String& name);
    void end(const void* owner, const juce::String& id, const juce::String& stage);
    void endOwned(const void* owner);                                  // owner is going away

    bool isQuarantined(const juce::String& id) const;
    juce::String reason(const juce::String& id) const;
    void quarantine(const juce::String& id, const juce::String& name, const juce::String& why);
    void release(const juce::String& id);
    void releaseAll();
    int  count() const;

private:
    CrashGuard();
    struct Mark { const void* owner; juce::String id, stage, name; };
    struct Q { juce::String name, why; };

    void writeActive();       // caller holds the lock
    void saveQuarantine();
    void loadQuarantine();
    void adoptCrashedSessions();

    mutable std::mutex lock;
    std::vector<Mark> marks;
    std::map<juce::String, Q> quarantined;
    juce::File activeFile, quarantineFile;
};
}
