// ============================================================================
//  Stats.cpp
//  · 缓存三份 Summary，SRWLOCK 保护：写只发生在采集线程，读只在主线程
//  · 十分钟才变一次的数据没必要每采样都查库，refresh_if_stale 做节流
// ============================================================================
#include "Stats.h"

#include "Store.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
stats::Summary g_cache[stats::kWindowCount];
std::atomic<unsigned long long> g_lastRefresh{0};

// 只在本模块内调用；localtime 的静态缓冲在采集线程串行使用，足够安全
long long local_midnight(long long now)
{
    const time_t t = static_cast<time_t>(now);
    const struct tm *p = localtime(&t);
    if (!p) return 0;

    struct tm lt = *p;
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;

    const time_t m = mktime(&lt);
    return m > 0 ? static_cast<long long>(m) : 0;
}

} // namespace

namespace stats {

long long window_start(int window, long long now)
{
    switch (window) {
    case kToday: return local_midnight(now);
    case kDay:   return now - 24LL * 3600;
    case kWeek:  return now - 7LL * 86400;
    default:     return now - 24LL * 3600;
    }
}

int window_from_text(const wchar_t *text)
{
    if (!text || !*text) return kToday;
    if (_wcsicmp(text, L"Today") == 0 || _wcsicmp(text, L"今日") == 0) return kToday;
    if (_wcsicmp(text, L"24h") == 0 || _wcsicmp(text, L"Day") == 0) return kDay;
    if (_wcsicmp(text, L"7d") == 0 || _wcsicmp(text, L"Week") == 0) return kWeek;
    return kToday;
}

const wchar_t *window_label(int window)
{
    switch (window) {
    case kDay:  return L"24 小时";
    case kWeek: return L"7 天";
    default:    return L"今日";
    }
}

std::wstring clock_hhmm(long long ts)
{
    if (ts <= 0) return L"--:--";

    const time_t t = static_cast<time_t>(ts);
    const struct tm *p = localtime(&t);
    if (!p) return L"--:--";

    const struct tm lt = *p;
    const time_t now = time(nullptr);
    const struct tm *q = localtime(&now);
    const bool same_day = q && q->tm_year == lt.tm_year && q->tm_yday == lt.tm_yday;

    wchar_t buf[32];
    if (same_day) swprintf(buf, 32, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    else          swprintf(buf, 32, L"%02d-%02d %02d:%02d",
                           lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return std::wstring(buf);
}

bool refresh()
{
    if (!store::available()) return false;

    const long long now = static_cast<long long>(time(nullptr));
    for (int w = 0; w < kWindowCount; ++w) {
        store::Agg a;
        Summary s;
        s.window = w;
        s.fromTs = window_start(w, now);
        s.toTs   = now;

        if (store::aggregate(s.fromTs, &a) && a.n > 0) {
            s.valid  = true;
            s.n      = static_cast<int>(a.n);
            s.firstTs = a.firstTs;
            s.lastTs  = a.lastTs;
            s.tMin = a.tMin; s.tMax = a.tMax; s.tAvg = a.tAvg;
            s.hMin = a.hMin; s.hMax = a.hMax; s.hAvg = a.hAvg;
            s.vAvg = a.vAvg;
            s.tMinTs = a.tMinTs; s.tMaxTs = a.tMaxTs;
        }

        AcquireSRWLockExclusive(&g_lock);
        g_cache[w] = s;
        ReleaseSRWLockExclusive(&g_lock);
    }

    g_lastRefresh = GetTickCount64();
    return true;
}

bool refresh_if_stale(int maxAgeSeconds)
{
    if (maxAgeSeconds < 0) maxAgeSeconds = 0;
    const unsigned long long last = g_lastRefresh.load();
    if (last != 0) {
        const unsigned long long now = GetTickCount64();
        if (now >= last && now - last < static_cast<unsigned long long>(maxAgeSeconds) * 1000ULL)
            return false;
    }
    return refresh();
}

Summary get(int window)
{
    if (window < 0 || window >= kWindowCount) window = kToday;

    AcquireSRWLockShared(&g_lock);
    const Summary s = g_cache[window];
    ReleaseSRWLockShared(&g_lock);
    return s;
}

std::wstring summary_text(const Summary &s)
{
    if (!s.valid) return L"窗口内暂无数据";

    wchar_t buf[256];
    swprintf(buf, 256,
             L"%s　最高 %.1f ℃（%s）　最低 %.1f ℃（%s）　均 %.1f ℃　湿度均 %.0f%%　样本 %d 条",
             window_label(s.window),
             s.tMax, clock_hhmm(s.tMaxTs).c_str(),
             s.tMin, clock_hhmm(s.tMinTs).c_str(),
             s.tAvg, s.hAvg, s.n);
    return std::wstring(buf);
}

} // namespace stats
