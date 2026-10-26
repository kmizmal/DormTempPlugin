// ============================================================================
//  SharedState.h —— 采集线程与 Rainmeter 主线程之间唯一的数据交换面
//  · 采集线程只写快照，measure 只读快照，SRWLOCK 保护
//  · 文本/状态判定全是纯函数，测试可直接覆盖，不必起蓝牙
// ============================================================================
#pragma once

#include "DormProtocol.h"

#include <string>

enum Status : int {
    kSearching  = 0,
    kConnecting = 1,
    kOnline     = 2,
};

struct Snapshot {
    double             temp = std::numeric_limits<double>::quiet_NaN();
    double             hum  = std::numeric_limits<double>::quiet_NaN();
    double             volt = std::numeric_limits<double>::quiet_NaN();
    unsigned long long lastSampleFT = 0;   // 0 = 从未采样
    int                status = kSearching;
    bool               ever   = false;     // 曾经连上过
    wchar_t            mac[32] = {0};
};

namespace state {

Snapshot get();

void publish(const Reading &r, unsigned long long sampleFt);
void set_status(int s);
void set_mac(unsigned long long addr, bool ever);

// 有 measure 存在 / 最近一次被读取的时间，用于判断皮肤是否还活着
void touch();
long long idle_ms();
int  measure_count();
void measure_add();
void measure_remove();

// 探测工具用来把每次采样打到控制台
using PublishHook = void (*)(const Reading &, void *);
void set_publish_hook(PublishHook hook, void *user);

} // namespace state

// ---------------------------------------------------------------- 纯函数
std::wstring status_text(int status, bool ever, double age_seconds);
std::wstring battery_text(double volt);
std::wstring comfort_text(double temp, double hum);
std::wstring number_text(double value, int decimals, const wchar_t *fallback = L"--");
std::wstring clock_text(unsigned long long filetime);

double         seconds_since(unsigned long long filetime);
unsigned long long now_ft();
