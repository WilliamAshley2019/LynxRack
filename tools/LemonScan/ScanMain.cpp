// LemonScan - describes ONE plugin file in its own process.
//   LemonScan.exe <plugin.vst3> <out.xml>
// Writes <LEMONSCAN><PLUGIN .../>...</LEMONSCAN>; exit code 0 = file examined (even if it holds no plugins).
// A crash or hang here is the scanner's problem (Lemon Mixer Rack kills it on timeout and blacklists the file),
// never the DAW's.
#include <juce_audio_processors/juce_audio_processors.h>

class LemonScanApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override { return "LemonScan"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void systemRequestedQuit() override { quit(); }
    void suspended() override {}
    void resumed() override {}
    void shutdown() override {}
    void unhandledException(const std::exception*, const juce::String&, int) override { setApplicationReturnValue(3); quit(); }

    void initialise(const juce::String&) override
    {
        auto args = getCommandLineParameterArray();
        if (args.size() < 2) { setApplicationReturnValue(2); quit(); return; }
        const auto file = args[0];
        const juce::File out(args[1]);
        juce::MessageManager::callAsync([this, file, out]
        {
            int code = 1;
            try
            {
                juce::VST3PluginFormat fmt;
                juce::OwnedArray<juce::PluginDescription> found;
                fmt.findAllTypesForFile(found, file);
                juce::XmlElement root("LEMONSCAN");
                for (auto* d : found) root.addChildElement(d->createXml().release());
                code = root.writeTo(out) ? 0 : 1;
            }
            catch (...) { code = 3; }
            setApplicationReturnValue(code);
            quit();
        });
    }
};

START_JUCE_APPLICATION(LemonScanApp)
