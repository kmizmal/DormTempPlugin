// ============================================================================
//  Chart.h —— 历史曲线（渲染成 PNG 交给皮肤的 Image meter）
//
//  · 为什么出图：Rainmeter 的 Line meter 只能画「面板自己累出来的点」，
//    读不到 SQLite 里的历史；把曲线画成 PNG 才能一次给出几天的全貌
//  · 渲染放在独立线程：Update / GetString 只读一份现成的路径，绝不阻塞宿主
//  · 同一个 key 全局只保留一份：皮肤里若干 measure（图 + 坐标轴）共享一次渲染
// ============================================================================
#pragma once

#include <string>

namespace chart {

enum Series {
    kTemp = 0,
    kHum  = 1,
};

struct Spec {
    int      window  = -1;    // >=0 用 stats 的窗口（0 今日 / 1 24h / 2 7d），-1 用 hours
    int      hours   = 24;
    int      buckets = 96;
    int      width   = 180;
    int      height  = 92;
    int      series  = kTemp;
    unsigned rgb     = 0x78C4FF;   // 0xRRGGBB
};

struct View {
    bool         valid = false;
    std::wstring path;            // PNG 绝对路径（还没渲染出来时为空）
    double lo = 0, mid = 0, hi = 0;   // Y 轴上下界（已取整，给坐标轴标签用）
    double tMin = 0, tMax = 0, hMin = 0, hMax = 0;
    long long tMinTs = 0, tMaxTs = 0;
    long long hMinTs = 0, hMaxTs = 0;   // 湿度极值时刻按桶心估算，不额外查库
    int      n = 0;               // 窗口内样本条数
};

// 注册/更新一条曲线（key 相同则覆盖规格）；返回 id，0 表示失败
int  open(const wchar_t *key, const Spec &spec);
// 只看不建：坐标轴、标题这些 measure 靠 key 找到同一个 id
int  find(const wchar_t *key);
void close(int id);

// 只查不改：拿到 id 的当前快照（没有数据 / 还没渲染时 valid = false）
View view(int id);

// 供坐标轴、时间轴文案使用
std::wstring span_text(int id);
std::wstring series_text(int id);

void start();
void stop();

// ---------------------------------------------------------------- 纯函数
// 根据数据范围算 Y 轴上下界：留白 12%，限制最小跨度，按刻度取整
void axis_bounds(double mn, double mx, int series, double *lo, double *hi, double *mid);

} // namespace chart
