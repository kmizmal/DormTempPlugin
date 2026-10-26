// ============================================================================
//  Store.h —— 采样落地（SQLite）
//  引擎用系统自带的 winsqlite3.dll，不打包 sqlite、不引入任何第三方依赖
// ============================================================================
#pragma once

#include <string>

namespace store {

// 目录为空时返回当前是否可用；同一目录重复调用不会重开库
bool init(const std::wstring &dir);
bool available();
void close();

// 同一条采样只在秒级去重（BLE 通知可能一秒内来多条）
void sample(double temp, double hum, double volt);

// 指定时间戳写入：测试与历史导入用，正常采样走上面那个
void sample_at(long long ts, double temp, double hum, double volt);

struct Row {
    long long ts = 0;
    double    temp = 0;
    double    hum = 0;
    double    volt = 0;
};

long long count();
int recent(int limit, Row *out);   // 返回实际写入 out 的条数（按 id 倒序）

// 区间汇总：统计 ts >= sinceTs 的记录（sinceTs = 0 表示全库）
struct Agg {
    long long n = 0;
    long long firstTs = 0;      // 窗口内最早一条
    long long lastTs = 0;       // 窗口内最晚一条
    double    tMin = 0, tMax = 0, tAvg = 0;
    double    hMin = 0, hMax = 0, hAvg = 0;
    double    vAvg = 0;
    long long tMinTs = 0, tMaxTs = 0;   // 温度最低 / 最高出现的时刻
};

// 一条 SQL 取完（子查询顺便带回极值时间），无数据时 n = 0 但仍返回 true
bool aggregate(long long sinceTs, Agg *out);

// 区间分桶：把 [sinceTs, untilTs) 等分成 buckets 段，每段取温湿度均值
// n = 0 表示该段没有采样（曲线在这里断线，不做插值填补）
struct Slot {
    double t = 0;
    double h = 0;
    int    n = 0;
};
bool series(long long sinceTs, long long untilTs, int buckets, Slot *out);

// 全库最新一条的 ts（没有数据返回 0）：曲线靠它判断要不要重画
long long latest_ts();

} // namespace store
