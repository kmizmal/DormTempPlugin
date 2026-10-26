// ============================================================================
//  Stats.h —— 区间汇总（今天 / 近 24 小时 / 近 7 天）
//
//  · 只由采集线程在拿到新采样时重算，主线程（rainmeter measure）永远只读缓存
//  · 三个窗口固定，不按 ini 动态生成：缓存结构简单，也避免每帧拼 SQL
//  · 窗口起点、窗口解析、摘要文本都是纯函数，测试可直接覆盖
// ============================================================================
#pragma once

#include <string>

namespace stats {

enum Window {
    kToday = 0,        // 今天 00:00 起
    kDay   = 1,        // 近 24 小时（滚动）
    kWeek  = 2,        // 近 7 天（滚动）
    kWindowCount = 3,
};

struct Summary {
    bool      valid = false;
    int       window = kToday;
    int       n = 0;              // 样本条数
    long long fromTs = 0;         // 窗口起点（epoch 秒）
    long long toTs = 0;           // 计算时刻
    long long firstTs = 0;        // 窗口内最早一条
    long long lastTs = 0;         // 窗口内最晚一条
    double    tMin = 0, tMax = 0, tAvg = 0;
    double    hMin = 0, hMax = 0, hAvg = 0;
    double    vAvg = 0;
    long long tMaxTs = 0, tMinTs = 0;
};

// 从 store 重算全部窗口并写入缓存；库不可用时返回 false（保留旧值）
bool refresh();

// 距上次刷新不足 maxAgeSeconds 时直接返回 false，避免高频采样把 SQL 打成满负荷
bool refresh_if_stale(int maxAgeSeconds);

Summary get(int window);

// ---------------------------------------------------------------- 纯函数
long long window_start(int window, long long now);          // 窗口起点
int       window_from_text(const wchar_t *text);            // Today/24h/7d -> Window
const wchar_t *window_label(int window);                    // 今日 / 24 小时 / 7 天
std::wstring clock_hhmm(long long ts);                      // 时间文本（跨天带日期）
std::wstring summary_text(const Summary &s);                // 卡片 tooltip 一行摘要

} // namespace stats
