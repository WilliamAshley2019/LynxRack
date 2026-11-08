#include "FlDatabase.h"

namespace lemon
{
juce::File defaultFlDatabaseRoot()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Image-Line").getChildFile("FL Studio").getChildFile("Presets").getChildFile("Plugin database");
}

juce::String normalisePath(const juce::String& p)
{
    return p.replaceCharacter('\\', '/').trim().trimCharactersAtEnd("/").toLowerCase();
}

namespace
{
void readIni(const juce::File& ini, FlData& out, const std::atomic<bool>& cancel)
{
    juce::String section;
    std::map<juce::String, juce::String> kv;

    auto flush = [&]
    {
        if (section.isEmpty()) return;
        auto parts = juce::StringArray::fromTokens(section, "\\", "");
        parts.removeEmptyStrings();
        const bool isGen = parts.size() >= 2 && parts[0].equalsIgnoreCase("Generators");
        if (parts.size() < 2 || (! isGen && ! parts[0].equalsIgnoreCase("Effects"))) { kv.clear(); return; }

        const int files = kv["ps_files"].getIntValue();
        for (int i = 0; i < files; ++i)
        {
            const auto sfx = "_" + juce::String(i);
            FlPlugin p;
            p.path = kv["ps_file_filename" + sfx];
            if (p.path.isEmpty() || p.path.contains("%")) continue;      // FL's own native plugins
            p.name = kv["ps_name"].isNotEmpty() ? kv["ps_name"] : kv["ps_file_name" + sfx];
            p.vendor = kv["ps_file_vendorname" + sfx];
            p.category = kv["ps_file_category" + sfx];
            p.format = parts[1];
            p.instrument = isGen;
            out.installed.push_back(std::move(p));
        }
        kv.clear();
    };

    for (auto& raw : juce::StringArray::fromLines(ini.loadFileAsString()))
    {
        if (cancel.load()) return;
        auto line = raw.trim();
        if (line.isEmpty() || line.startsWithChar(';') || line.startsWithChar('#')) continue;
        if (line.startsWithChar('[') && line.endsWithChar(']'))
        {
            flush();
            section = line.substring(1, line.length() - 1);
            continue;
        }
        const int eq = line.indexOfChar('=');
        if (eq > 0 && section.isNotEmpty())
            kv[line.substring(0, eq).trim()] = line.substring(eq + 1).trim();
    }
    flush();
}
}

FlData readFlDatabase(const juce::File& root, const std::atomic<bool>& cancel)
{
    FlData d;
    d.root = root;
    if (! root.isDirectory()) return d;
    d.found = true;

    auto installed = root.getChildFile("Installed");
    for (auto& ini : { installed.getChildFile(".Plugins.ini"),
                       installed.getChildFile("Effects").getChildFile(".Plugins.ini"),
                       installed.getChildFile("Generators").getChildFile(".Plugins.ini") })
        if (ini.existsAsFile()) readIni(ini, d, cancel);

    // favourites: Effects\<folder>\Name.fst
    for (auto* sub : { "Effects", "Generators" })
    {
        auto fx = root.getChildFile(sub);
        if (! fx.isDirectory()) continue;
        for (auto& f : fx.findChildFiles(juce::File::findFiles, true, "*.fst"))
        {
            if (cancel.load()) break;
            auto rel = f.getParentDirectory().getRelativePathFrom(fx);
            d.favourite[f.getFileNameWithoutExtension().toLowerCase()] = rel == "." ? juce::String() : rel;
        }
    }
    return d;
}
}
