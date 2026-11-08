#pragma once
// SQLite plugin database tools. No JUCE.
//
// Uses your own sqlite3 amalgamation when it is present:
//   Source/ThirdParty/sqlite/sqlite3.h  (+ sqlite3.c or sqlite3.cpp added to the project)
// Without it everything here still compiles and reports sqliteAvailable() == false.
//
//  * LemonDb          : the rack's own catalog (lemon_plugins.db). The rack owns and writes this one.
//  * readOrganizerDb  : READ-ONLY look into your FLP Organizer database. Never writes to it. It does not
//                       assume a schema: it lists tables/columns, picks the plugin-like table by name and
//                       columns, and returns a human-readable schema dump so a mismatch is easy to fix.

#include <string>
#include <vector>

namespace lemon::data
{
bool sqliteAvailable();

struct OrgPlugin { std::string name, path, vendor, category, format; };

struct OrgResult
{
    bool ok = false;
    std::string error;
    std::string table;                 // the table the rows came from
    std::string schema;                // every table, its columns and row count
    std::vector<OrgPlugin> plugins;
};
OrgResult readOrganizerDb(const std::string& dbPath);

struct PluginRecord
{
    std::string id, name, vendor, format, path, flCategory, flFolder, orgCategory, reason;
    bool inFl = false, inOrg = false, quarantined = false;
    bool instrument = false;                 // stored as kind = 'instrument' (additive column; older tools ignore it)
};

class LemonDb
{
public:
    explicit LemonDb(const std::string& path);
    ~LemonDb();
    LemonDb(const LemonDb&) = delete;
    LemonDb& operator=(const LemonDb&) = delete;

    bool ok() const noexcept { return db != nullptr; }
    const std::string& error() const noexcept { return err; }
    bool sync(const std::vector<PluginRecord>&);     // upsert in one transaction
    int  count();

private:
    void* db = nullptr;
    std::string err;
};
}
