// ============================================================================
//  Store.cpp
//  · 只做函数指针绑定，DLL 体积不受 SQLite 影响
//  · 每条采样自动提交（WAL + synchronous=NORMAL），进程被杀也不丢已提交数据
// ============================================================================
#include "Store.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace {

typedef int (*fn_open_v2)(const char *, sqlite3 **, int, const char *);
typedef int (*fn_close_)(sqlite3 *);
typedef int (*fn_exec)(sqlite3 *, const char *, void *, void *, char **);
typedef int (*fn_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (*fn_step)(sqlite3_stmt *);
typedef int (*fn_finalize)(sqlite3_stmt *);
typedef int (*fn_reset)(sqlite3_stmt *);
typedef int (*fn_clear_bindings)(sqlite3_stmt *);
typedef int (*fn_bind_int64)(sqlite3_stmt *, int, long long);
typedef int (*fn_bind_double)(sqlite3_stmt *, int, double);
typedef int (*fn_bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
typedef double (*fn_column_double)(sqlite3_stmt *, int);
typedef long long (*fn_column_int64)(sqlite3_stmt *, int);

#define SQLITE_OK_        0
#define SQLITE_ROW_       100
#define SQLITE_DONE_      101
#define SQLITE_OPEN_RWC   (0x00000002 | 0x00000004)
#define SQLITE_TRANSIENT_ ((void (*)(void *)) - 1)

struct Api {
    HMODULE lib = nullptr;

    fn_open_v2         open_v2 = nullptr;
    fn_close_          close_ = nullptr;
    fn_exec            exec = nullptr;
    fn_prepare         prepare = nullptr;
    fn_step            step = nullptr;
    fn_finalize        finalize = nullptr;
    fn_reset           reset = nullptr;
    fn_clear_bindings  clear_bindings = nullptr;
    fn_bind_int64      bind_int64 = nullptr;
    fn_bind_double     bind_double = nullptr;
    fn_bind_text       bind_text = nullptr;
    fn_column_double   column_double = nullptr;
    fn_column_int64    column_int64 = nullptr;
};

Api g_api;
bool g_apiTried = false;
std::mutex g_mtx;
sqlite3 *g_db = nullptr;
sqlite3_stmt *g_ins = nullptr;
std::wstring g_dir;
bool g_sqlite = false;
bool g_inited = false;
long long g_lastTs = 0;

std::string w2u8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

bool load_api()
{
    if (g_api.open_v2) return true;
    if (g_apiTried) return false;
    g_apiTried = true;

    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) h = LoadLibraryW(L"sqlite3.dll");
    if (!h) return false;
    g_api.lib = h;

#define BIND(field, name) g_api.field = (fn_##field)GetProcAddress(h, name)
    BIND(open_v2, "sqlite3_open_v2");
    BIND(close_, "sqlite3_close_v2");
    BIND(exec, "sqlite3_exec");
    BIND(prepare, "sqlite3_prepare_v2");
    BIND(step, "sqlite3_step");
    BIND(finalize, "sqlite3_finalize");
    BIND(reset, "sqlite3_reset");
    BIND(clear_bindings, "sqlite3_clear_bindings");
    BIND(bind_int64, "sqlite3_bind_int64");
    BIND(bind_double, "sqlite3_bind_double");
    BIND(bind_text, "sqlite3_bind_text");
    BIND(column_double, "sqlite3_column_double");
    BIND(column_int64, "sqlite3_column_int64");
#undef BIND

    if (!g_api.open_v2 || !g_api.close_ || !g_api.exec || !g_api.prepare || !g_api.step ||
        !g_api.finalize || !g_api.reset || !g_api.clear_bindings || !g_api.bind_int64 ||
        !g_api.bind_double || !g_api.bind_text || !g_api.column_double || !g_api.column_int64) {
        FreeLibrary(h);
        g_api = Api();
        return false;
    }
    return true;
}

void run_sql(const char *sql)
{
    g_api.exec(g_db, sql, nullptr, nullptr, nullptr);
}

void close_db_locked()
{
    if (g_ins) { g_api.finalize(g_ins); g_ins = nullptr; }
    if (g_db) {
        run_sql("PRAGMA wal_checkpoint(TRUNCATE);");
        g_api.close_(g_db);
        g_db = nullptr;
    }
    g_sqlite = false;
}

} // namespace

namespace store {

bool init(const std::wstring &source_dir)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (source_dir.empty()) return g_sqlite;
    if (g_inited && source_dir == g_dir) return g_sqlite;

    if (g_sqlite) close_db_locked();
    g_dir = source_dir;
    g_inited = true;

    if (!load_api()) return false;

    std::error_code ec;
    std::filesystem::create_directories(source_dir, ec);

    const std::wstring wpath = source_dir + L"\\dormtemp.db";
    const std::string u8 = w2u8(wpath);

    sqlite3 *db = nullptr;
    if (g_api.open_v2(u8.c_str(), &db, SQLITE_OPEN_RWC, nullptr) != SQLITE_OK_ || !db) {
        if (db) g_api.close_(db);
        return false;
    }
    g_db = db;

    run_sql("PRAGMA journal_mode=WAL;");
    run_sql("PRAGMA synchronous=NORMAL;");
    run_sql("PRAGMA busy_timeout=3000;");
    run_sql("PRAGMA journal_size_limit=1048576;");   // WAL 上限 ~1MB，避免日志文件无限长
    run_sql("CREATE TABLE IF NOT EXISTS samples("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ts INTEGER NOT NULL,"
            "dt TEXT NOT NULL,"
            "temp REAL NOT NULL,"
            "hum REAL NOT NULL,"
            "volt REAL);");
    run_sql("CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts);");

    sqlite3_stmt *ins = nullptr;
    if (g_api.prepare(g_db, "INSERT INTO samples(ts,dt,temp,hum,volt) VALUES(?1,?2,?3,?4,?5)",
                      -1, &ins, nullptr) != SQLITE_OK_) {
        close_db_locked();
        return false;
    }
    g_ins = ins;
    g_sqlite = true;
    g_lastTs = 0;
    return true;
}

bool available()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_sqlite;
}

void close()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sqlite) close_db_locked();
    g_inited = false;
    g_dir.clear();
}

void sample(double temp, double hum, double volt)
{
    sample_at(static_cast<long long>(time(nullptr)), temp, hum, volt);
}

void sample_at(long long ts, double temp, double hum, double volt)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_ins) return;
    if (g_lastTs && ts == g_lastTs) return;      // 同一秒只留一条

    const time_t raw = static_cast<time_t>(ts);
    const struct tm *p = localtime(&raw);
    wchar_t wdt[32] = {0};
    if (p) {
        swprintf(wdt, 32, L"%04d-%02d-%02d %02d:%02d:%02d",
                 p->tm_year + 1900, p->tm_mon + 1, p->tm_mday,
                 p->tm_hour, p->tm_min, p->tm_sec);
    }
    const std::string dt = w2u8(wdt);

    g_api.reset(g_ins);
    g_api.clear_bindings(g_ins);
    g_api.bind_int64(g_ins, 1, ts);
    g_api.bind_text(g_ins, 2, dt.c_str(), -1, SQLITE_TRANSIENT_);
    g_api.bind_double(g_ins, 3, temp);
    g_api.bind_double(g_ins, 4, hum);
    g_api.bind_double(g_ins, 5, volt);
    if (g_api.step(g_ins) == SQLITE_DONE_) g_lastTs = ts;
    g_api.reset(g_ins);
}

long long count()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_db) return -1;

    sqlite3_stmt *st = nullptr;
    if (g_api.prepare(g_db, "SELECT count(*) FROM samples", -1, &st, nullptr) != SQLITE_OK_ || !st)
        return -1;
    long long n = -1;
    if (g_api.step(st) == SQLITE_ROW_) n = g_api.column_int64(st, 0);
    g_api.finalize(st);
    return n;
}

int recent(int limit, Row *out)
{
    if (!out || limit <= 0) return 0;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_db) return 0;

    sqlite3_stmt *st = nullptr;
    if (g_api.prepare(g_db, "SELECT ts,temp,hum,volt FROM samples ORDER BY id DESC LIMIT ?1",
                      -1, &st, nullptr) != SQLITE_OK_ || !st)
        return 0;

    g_api.bind_int64(st, 1, limit);
    int n = 0;
    while (n < limit && g_api.step(st) == SQLITE_ROW_) {
        out[n].ts = g_api.column_int64(st, 0);
        out[n].temp = g_api.column_double(st, 1);
        out[n].hum = g_api.column_double(st, 2);
        out[n].volt = g_api.column_double(st, 3);
        ++n;
    }
    g_api.finalize(st);
    return n;
}

bool aggregate(long long sinceTs, Agg *out)
{
    if (!out) return false;
    *out = Agg();

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_db) return false;

    // 极值时间用子查询带回：同一遍扫描里 SQLite 会复用 ts 索引过滤条件
    static const char *kSql =
        "SELECT count(*), min(temp), max(temp), avg(temp),"
        "       min(hum),  max(hum),  avg(hum),  avg(volt),"
        "       min(ts),   max(ts),"
        "       (SELECT ts FROM samples WHERE ts>=?1 ORDER BY temp ASC  LIMIT 1),"
        "       (SELECT ts FROM samples WHERE ts>=?1 ORDER BY temp DESC LIMIT 1) "
        "FROM samples WHERE ts>=?1";

    sqlite3_stmt *st = nullptr;
    if (g_api.prepare(g_db, kSql, -1, &st, nullptr) != SQLITE_OK_ || !st) return false;

    g_api.bind_int64(st, 1, sinceTs);
    bool ok = false;
    if (g_api.step(st) == SQLITE_ROW_) {
        const long long n = g_api.column_int64(st, 0);
        if (n > 0) {
            out->n = n;
            out->tMin = g_api.column_double(st, 1);
            out->tMax = g_api.column_double(st, 2);
            out->tAvg = g_api.column_double(st, 3);
            out->hMin = g_api.column_double(st, 4);
            out->hMax = g_api.column_double(st, 5);
            out->hAvg = g_api.column_double(st, 6);
            out->vAvg = g_api.column_double(st, 7);
            out->firstTs = g_api.column_int64(st, 8);
            out->lastTs = g_api.column_int64(st, 9);
            out->tMinTs = g_api.column_int64(st, 10);
            out->tMaxTs = g_api.column_int64(st, 11);
        }
        ok = true;
    }
    g_api.finalize(st);
    return ok;
}

bool series(long long sinceTs, long long untilTs, int buckets, Slot *out)
{
    if (!out || buckets <= 0) return false;
    for (int i = 0; i < buckets; ++i) out[i] = Slot();
    if (untilTs <= sinceTs) return false;

    // 桶宽向上取整：宁可最后一段略短，也不要因为截断丢掉尾部采样
    const long long width = (untilTs - sinceTs + buckets - 1) / buckets;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_db) return false;

    // ts 与桶宽都是整数，SQLite 的 / 就是整除，直接 group by 桶号
    static const char *kSql =
        "SELECT (ts-?1)/?2 AS b, avg(temp), avg(hum), count(*) "
        "FROM samples WHERE ts>=?1 AND ts<?3 GROUP BY b ORDER BY b";

    sqlite3_stmt *st = nullptr;
    if (g_api.prepare(g_db, kSql, -1, &st, nullptr) != SQLITE_OK_ || !st) return false;

    g_api.bind_int64(st, 1, sinceTs);
    g_api.bind_int64(st, 2, width);
    g_api.bind_int64(st, 3, untilTs);

    while (g_api.step(st) == SQLITE_ROW_) {
        long long b = g_api.column_int64(st, 0);
        if (b < 0) b = 0;
        if (b >= buckets) b = buckets - 1;   // 防御：极端取整下最后一条可能越界
        out[b].t = g_api.column_double(st, 1);
        out[b].h = g_api.column_double(st, 2);
        out[b].n = static_cast<int>(g_api.column_int64(st, 3));
    }
    g_api.finalize(st);
    return true;
}

long long latest_ts()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_db) return 0;

    sqlite3_stmt *st = nullptr;
    if (g_api.prepare(g_db, "SELECT max(ts) FROM samples", -1, &st, nullptr) != SQLITE_OK_ || !st)
        return 0;

    long long ts = 0;
    if (g_api.step(st) == SQLITE_ROW_) ts = g_api.column_int64(st, 0);   // NULL -> 0
    g_api.finalize(st);
    return ts;
}

} // namespace store
