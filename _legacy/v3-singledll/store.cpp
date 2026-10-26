// ============================================================================
//  store.cpp —— 采样数据落地：系统自带的 winsqlite3.dll（Win10/11 组件）
//  · 零外部依赖、零体积代价：只做函数指针绑定
//  · 每条采样自动提交（WAL + synchronous=NORMAL），进程被杀也不丢已提交数据
// ============================================================================
#include "store.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <filesystem>

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

#define SQLITE_OK_          0
#define SQLITE_DONE_        101
#define SQLITE_OPEN_RWC     (0x00000002 | 0x00000004)
#define SQLITE_TRANSIENT_   ((void (*)(void *)) - 1)

struct Api {
    HMODULE lib = nullptr;
    fn_open_v2        open_v2 = nullptr;
    fn_close_         close_ = nullptr;
    fn_exec           exec = nullptr;
    fn_prepare        prepare = nullptr;
    fn_step           step = nullptr;
    fn_finalize       finalize = nullptr;
    fn_reset          reset = nullptr;
    fn_clear_bindings clear_bindings = nullptr;
    fn_bind_int64     bind_int64 = nullptr;
    fn_bind_double    bind_double = nullptr;
    fn_bind_text      bind_text = nullptr;
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
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
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
#define BIND(field, name) g_api.field = (fn_##field) GetProcAddress(h, name)
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
#undef BIND
    if (!g_api.open_v2 || !g_api.close_ || !g_api.exec || !g_api.prepare || !g_api.step ||
        !g_api.finalize || !g_api.reset || !g_api.bind_int64 || !g_api.bind_double || !g_api.bind_text) {
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
    if (g_db) { run_sql("PRAGMA wal_checkpoint(TRUNCATE);"); g_api.close_(g_db); g_db = nullptr; }
    g_sqlite = false;
}

} // namespace

namespace store {

bool init(const std::wstring &dir)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (dir.empty()) return g_sqlite;
    if (g_inited && dir == g_dir) return g_sqlite;

    if (g_sqlite) close_db_locked();
    g_dir = dir;
    g_inited = true;

    if (!load_api()) return false;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::wstring wpath = dir + L"\\dormtemp.db";
    std::string u8 = w2u8(wpath);

    sqlite3 *db = nullptr;
    if (g_api.open_v2(u8.c_str(), &db, SQLITE_OPEN_RWC, nullptr) != SQLITE_OK_ || !db) {
        if (db && g_api.close_) g_api.close_(db);
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

void sample(double temp, double hum, double volt)
{
    long long ts = (long long)time(nullptr);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sqlite || !g_ins) return;
    if (g_lastTs && ts == g_lastTs) return;      // 同一秒只留一条

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t wdt[32];
    swprintf(wdt, 32, L"%04u-%02u-%02u %02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string dt = w2u8(wdt);

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

bool available()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_sqlite;
}

} // namespace store
