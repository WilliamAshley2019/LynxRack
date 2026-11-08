#pragma once
// A tiny append-only diagnostic log (%APPDATA%\AlphaAudio\LemonMixerRack\host_events.log).
// Records what a hosted plugin asks of its host (latency / parameter / state change requests) and the stages of
// loading and unloading, so a plugin that misbehaves (e.g. one that loads assets or checks a licence in the
// background) can be diagnosed from a file instead of guessed at.
#include <juce_core/juce_core.h>
#include <mutex>

namespace lemon
{
inline void hostLog(const juce::String& line)
{
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    auto f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                 .getChildFile("AlphaAudio").getChildFile("LemonMixerRack").getChildFile("host_events.log");
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 512 * 1024) f.deleteFile();
    f.appendText(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "."
                     + juce::String(juce::Time::getMillisecondCounter() % 1000).paddedLeft('0', 3)
                     + "  [" + juce::String::toHexString((juce::int64) ((juce::pointer_sized_uint) juce::Thread::getCurrentThreadId() & 0xffff)) + "]  "
                     + line + "\n");
}
}
