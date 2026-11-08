#include "PluginDb.h"

#include <algorithm>
#include <cctype>
#include <map>

#if defined(LEMON_NO_SQLITE)
 #define LEMON_HAS_SQLITE 0
#elif __has_include("../ThirdParty/sqlite/sqlite3.h")
 #include "../ThirdParty/sqlite/sqlite3.h"
 #define LEMON_HAS_SQLITE 1
#elif __has_include("../ThirdParty/sqlite/sqlite.h")
 #include "../ThirdParty/sqlite/sqlite.h"
 #define LEMON_HAS_SQLITE 1
#elif __has_include(<sqlite3.h>)
 #include <sqlite3.h>
 #define LEMON_HAS_SQLITE 1
#else
 #define LEMON_HAS_SQLITE 0
#endif

namespace lemon::data
{
bool sqliteAvailable() { return LEMON_HAS_SQLITE != 0; }

#if LEMON_HAS_SQLITE
namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string quoteIdent(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

struct Stmt
{
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const std::string& sql) { if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr; }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    std::string text(int col) const
    {
        auto* p = sqlite3_column_text(s, col);
        return p ? std::string((const char*) p) : std::string();
    }
};

struct Column { std::string name; };
struct Table { std::string name; std::vector<std::string> columns; long long rows = 0; };

std::vector<Table> readTables(sqlite3* db)
{
    std::vector<Table> out;
    {
        Stmt st(db, "SELECT name FROM sqlite_master WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' ORDER BY name");
        if (st) while (sqlite3_step(st.s) == SQLITE_ROW) { Table t; t.name = st.text(0); out.push_back(std::move(t)); }
    }
    for (auto& t : out)
    {
        Stmt ci(db, "PRAGMA table_info(" + quoteIdent(t.name) + ")");
        if (ci) while (sqlite3_step(ci.s) == SQLITE_ROW) t.columns.push_back(ci.text(1));
        Stmt cn(db, "SELECT count(*) FROM " + quoteIdent(t.name));
        if (cn && sqlite3_step(cn.s) == SQLITE_ROW) t.rows = sqlite3_column_int64(cn.s, 0);
    }
    return out;
}

// first column of `t` whose lower-case name equals one of `wanted` (in the order of `wanted`)
std::string pick(const Table& t, std::initializer_list<const char*> wanted)
{
    for (auto* w : wanted)
        for (auto& c : t.columns)
            if (lower(c) == w) return c;
    return {};
}

struct Mapping { std::string name, path, vendor, category, format; };

Mapping mapColumns(const Table& t)
{
    Mapping m;
    m.path     = pick(t, { "path", "file_path", "filepath", "plugin_path", "binary_path", "filename", "file", "location" });
    if (m.path.empty())
        for (auto& c : t.columns) if (lower(c).find("path") != std::string::npos) { m.path = c; break; }
    m.name     = pick(t, { "name", "plugin_name", "pluginname", "ps_name", "display_name", "title" });
    m.vendor   = pick(t, { "vendor", "vendor_name", "vendorname", "manufacturer", "developer", "company" });
    m.category = pick(t, { "category", "plugin_category", "kind", "class", "type" });
    m.format   = pick(t, { "format", "plugin_format", "fmt" });
    return m;
}

// share of the first 50 path values that end in a plugin binary extension
double pluginPathShare(sqlite3* db, const Table& t, const Mapping& m)
{
    Stmt st(db, "SELECT COALESCE(CAST(" + quoteIdent(m.path) + " AS TEXT),'') FROM " + quoteIdent(t.name) + " LIMIT 50");
    if (! st) return 0.0;
    int total = 0, hits = 0;
    while (sqlite3_step(st.s) == SQLITE_ROW)
    {
        const auto v = lower(st.text(0));
        ++total;
        for (auto* ext : { ".vst3", ".vst", ".dll", ".clap" })
        {
            const std::string e(ext);
            if (v.size() >= e.size() && v.compare(v.size() - e.size(), e.size(), e) == 0) { ++hits; break; }
        }
    }
    return total > 0 ? (double) hits / total : 0.0;
}

int score(sqlite3* db, const Table& t, const Mapping& m)
{
    if (m.name.empty() || m.path.empty()) return -1;
    int s = 0;
    const auto n = lower(t.name);
    const bool nameHit = n.find("plugin") != std::string::npos || n.find("vst") != std::string::npos
                         || n.find("effect") != std::string::npos || n.find("fx") != std::string::npos;
    const bool pathsLookLikePlugins = pluginPathShare(db, t, m) >= 0.5;
    if (! nameHit && ! pathsLookLikePlugins) return -1;          // e.g. a projects table also has name + path
    if (nameHit) s += 10;
    if (pathsLookLikePlugins) s += 8;
    if (! m.vendor.empty()) s += 2;
    if (! m.category.empty()) s += 1;
    if (t.rows > 0) s += 1;
    return s;
}
}

namespace
{
// Declared before any statement so it is destroyed after them: sqlite3_close on a connection that still
// has live statements fails and leaks the connection.
struct Conn
{
    sqlite3* db = nullptr;
    ~Conn() { if (db) sqlite3_close_v2(db); }
};
}

OrgResult readOrganizerDb(const std::string& dbPath)
{
    OrgResult r;
    Conn conn;
    // READONLY: this is another tool's database; we never write to it.
    if (sqlite3_open_v2(dbPath.c_str(), &conn.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        r.error = std::string("cannot open: ") + (conn.db ? sqlite3_errmsg(conn.db) : "out of memory");
        return r;
    }
    sqlite3* db = conn.db;
    sqlite3_busy_timeout(db, 1500);          // the Organizer may be using it

    auto tables = readTables(db);
    for (auto& t : tables)
    {
        r.schema += t.name + "  (" + std::to_string(t.rows) + " rows)\n";
        for (auto& c : t.columns) r.schema += "    " + c + "\n";
    }
    if (tables.empty()) { r.error = "the database has no tables"; return r; }

    const Table* best = nullptr; Mapping bm; int bs = -1;
    for (auto& t : tables)
    {
        auto m = mapColumns(t);
        const int sc = score(db, t, m);
        if (sc > bs) { bs = sc; best = &t; bm = m; }
    }
    if (best == nullptr || bs < 0)
    {
        r.error = "no table with a plugin name and path column was found";
        return r;
    }
    r.table = best->name;

    auto col = [](const std::string& c) { return c.empty() ? std::string("''") : "COALESCE(CAST(" + quoteIdent(c) + " AS TEXT),'')"; };
    Stmt q(db, "SELECT " + col(bm.name) + "," + col(bm.path) + "," + col(bm.vendor) + "," + col(bm.category) + "," + col(bm.format)
                   + " FROM " + quoteIdent(best->name));
    if (! q) { r.error = std::string("query failed: ") + sqlite3_errmsg(db); return r; }
    while (sqlite3_step(q.s) == SQLITE_ROW)
    {
        OrgPlugin p { q.text(0), q.text(1), q.text(2), q.text(3), q.text(4) };
        if (! p.path.empty()) r.plugins.push_back(std::move(p));
    }
    r.ok = true;
    return r;
}

// --------------------------------------------------------------------------- LemonDb
LemonDb::LemonDb(const std::string& path)
{
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(path.c_str(), &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        err = h ? sqlite3_errmsg(h) : "out of memory";
        if (h) sqlite3_close(h);
        return;
    }
    sqlite3_busy_timeout(h, 2000);
    const char* schema =
        "CREATE TABLE IF NOT EXISTS plugins("
        " id TEXT PRIMARY KEY, name TEXT, vendor TEXT, format TEXT, path TEXT,"
        " fl_category TEXT, fl_folder TEXT, org_category TEXT,"
        " in_fl INTEGER DEFAULT 0, in_org INTEGER DEFAULT 0, quarantined INTEGER DEFAULT 0, reason TEXT,"
        " first_seen TEXT, last_seen TEXT);"
        "CREATE INDEX IF NOT EXISTS plugins_path ON plugins(path);";
    char* msg = nullptr;
    if (sqlite3_exec(h, schema, nullptr, nullptr, &msg) != SQLITE_OK)
    {
        err = msg ? msg : "schema error";
        sqlite3_free(msg);
        sqlite3_close(h);
        return;
    }
    sqlite3_exec(h, "ALTER TABLE plugins ADD COLUMN kind TEXT DEFAULT 'effect'", nullptr, nullptr, nullptr);   // additive migration; fails harmlessly when present
    db = h;
}

LemonDb::~LemonDb() { if (db) sqlite3_close((sqlite3*) db); }

bool LemonDb::sync(const std::vector<PluginRecord>& recs)
{
    if (! db) return false;
    auto* h = (sqlite3*) db;
    if (sqlite3_exec(h, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return false;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO plugins(id,name,vendor,format,path,fl_category,fl_folder,org_category,in_fl,in_org,quarantined,reason,kind,first_seen,last_seen)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,datetime('now'),datetime('now'))"
        " ON CONFLICT(id) DO UPDATE SET name=?2,vendor=?3,format=?4,path=?5,fl_category=?6,fl_folder=?7,org_category=?8,"
        " in_fl=?9,in_org=?10,quarantined=?11,reason=?12,kind=?13,last_seen=datetime('now')";
    if (sqlite3_prepare_v2(h, sql, -1, &st, nullptr) != SQLITE_OK) { sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr); return false; }

    for (auto& r : recs)
    {
        sqlite3_reset(st);
        auto txt = [&](int i, const std::string& s) { sqlite3_bind_text(st, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        txt(1, r.id); txt(2, r.name); txt(3, r.vendor); txt(4, r.format); txt(5, r.path);
        txt(6, r.flCategory); txt(7, r.flFolder); txt(8, r.orgCategory);
        sqlite3_bind_int(st, 9, r.inFl); sqlite3_bind_int(st, 10, r.inOrg); sqlite3_bind_int(st, 11, r.quarantined);
        txt(12, r.reason);
        txt(13, r.instrument ? "instrument" : "effect");
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr); return false; }
    }
    sqlite3_finalize(st);
    return sqlite3_exec(h, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

int LemonDb::count()
{
    if (! db) return 0;
    Stmt s((sqlite3*) db, "SELECT count(*) FROM plugins");
    return s && sqlite3_step(s.s) == SQLITE_ROW ? sqlite3_column_int(s.s, 0) : 0;
}

#else   // ------------------------------------------------------------ no sqlite available
OrgResult readOrganizerDb(const std::string&)
{
    OrgResult r;
    r.error = "SQLite is not part of this build (add Source/ThirdParty/sqlite/sqlite3.h and the amalgamation)";
    return r;
}
LemonDb::LemonDb(const std::string&) { err = "SQLite is not part of this build"; }
LemonDb::~LemonDb() = default;
bool LemonDb::sync(const std::vector<PluginRecord>&) { return false; }
int LemonDb::count() { return 0; }
#endif
}
