// ============================================================================
//  SharedState.cpp
// ============================================================================
#include "SharedState.h"

#include "Config.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace {

SRWLOCK  g_lock = SRWLOCK_INIT;
Snapshot g_snap;

std::atomic<int>           g_measures{0};
std::atomic<unsigned long long> g_lastActive{0};
std::atomic<state::PublishHook> g_hook{nullptr};
void *g_hookUser = nullptr;

constexpr double kStaleSeconds = 180.0;   // 超过 3 分钟没新数据即视为超时

} // namespace

namespace state {

Snapshot get()
{
    AcquireSRWLockShared(&g_lock);
    Snapshot s = g_snap;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

void publish(const Reading &r, unsigned long long sampleFt)
{
    if (!r.valid) return;
    {
        AcquireSRWLockExclusive(&g_lock);
        g_snap.temp = r.temp;
        g_snap.hum  = r.hum;
        g_snap.volt = r.volt;
        g_snap.lastSampleFT = sampleFt;
        ReleaseSRWLockExclusive(&g_lock);
    }

    if (PublishHook hook = g_hook.load()) hook(r, g_hookUser);
}

void set_status(int s)
{
    AcquireSRWLockExclusive(&g_lock);
    g_snap.status = s;
    ReleaseSRWLockExclusive(&g_lock);
}

void set_mac(unsigned long long addr, bool ever)
{
    const std::wstring text = config::format_mac(addr);
    AcquireSRWLockExclusive(&g_lock);
    wcsncpy(g_snap.mac, text.c_str(), 31);
    g_snap.mac[31] = 0;
    if (ever) g_snap.ever = true;
    ReleaseSRWLockExclusive(&g_lock);
}

void touch() { g_lastActive = GetTickCount64(); }

long long idle_ms()
{
    const unsigned long long last = g_lastActive.load();
    if (last == 0) return 0;
    const unsigned long long now = GetTickCount64();
    return now >= last ? static_cast<long long>(now - last) : 0;
}

int  measure_count() { return g_measures.load(); }
void measure_add() { ++g_measures; }
void measure_remove()
{
    int n = --g_measures;
    if (n < 0) g_measures = 0;
}

void set_publish_hook(PublishHook hook, void *user)
{
    g_hookUser = user;
    g_hook = hook;
}

} // namespace state

// ---------------------------------------------------------------- 纯函数

double seconds_since(unsigned long long filetime)
{
    if (filetime == 0) return -1;
    const unsigned long long n = now_ft();
    if (n < filetime) return 0;
    return static_cast<double>(n - filetime) / 10000000.0;
}

unsigned long long now_ft()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring status_text(int status, bool ever, double age_seconds)
{
    if (status == kOnline) {
        if (age_seconds >= 0 && age_seconds < kStaleSeconds) return L"在线";
        return L"数据超时";
    }
    if (status == kConnecting) return ever ? L"重连中" : L"连接中";
    return ever ? L"离线" : L"搜索中";
}

std::wstring battery_text(double volt)
{
    if (!std::isfinite(volt)) return L"--";
    if (volt >= 3.0) return L"电量充足";
    if (volt >= 2.7) return L"电量偏低";
    return L"需更换电池";
}

std::wstring comfort_text(double temp, double hum)
{
    if (!std::isfinite(temp) || !std::isfinite(hum)) return L"--";
    if (temp < 18.0) return L"偏冷";
    if (temp > 28.0) return L"偏热";
    if (hum < 40.0) return L"干燥";
    if (hum > 70.0) return L"潮湿";
    return L"舒适";
}

std::wstring number_text(double value, int decimals, const wchar_t *fallback)
{
    if (!std::isfinite(value)) return fallback ? fallback : L"--";
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, value);
    return std::wstring(buf);
}

std::wstring clock_text(unsigned long long filetime)
{
    if (filetime == 0) return L"--:--";
    FILETIME ft;
    memcpy(&ft, &filetime, sizeof(ft));
    FILETIME local;
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st;
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}
