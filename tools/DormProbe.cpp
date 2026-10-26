// ============================================================================
//  DormProbe.cpp —— BLE 探测工具（对应 MCHOSE 项目的 HidProbe）
//
//  用法：
//    DormProbe.exe scan [秒=8]           列出周边广播：MAC / RSSI / 名称 / 是否命中
//    DormProbe.exe read [MAC] [秒=20]    连接并打印通知（不给 MAC 就先扫描）
//    DormProbe.exe db [数据目录]          看 SQLite 里已有的记录
//    DormProbe.exe chart [数据目录] [小时] [temp|hum|both]
//                                         把历史采样渲染成一张 PNG（验证曲线外观）
//
//  刻意不写正式数据目录：探测期间落在 %TEMP%\DormProbe，不污染皮肤在用的库。
//  输出统一转成 UTF-8 再 printf —— 控制台 code page 与 C locale 都不参与，
//  重定向到文件/管道时中文也不会变成问号。
// ============================================================================
#include <assert.h>
#ifndef _ASSERTE
#define _ASSERTE(x) ((void)0)
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include "BleSession.h"
#include "Chart.h"
#include "Config.h"
#include "DormProtocol.h"
#include "Log.h"
#include "SharedState.h"
#include "Stats.h"
#include "Store.h"

namespace WDBA = winrt::Windows::Devices::Bluetooth::Advertisement;

namespace {

std::string u8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

void say(const std::wstring &line) { printf("%s\n", u8(line).c_str()); }

void sayf(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    say(buf);
}

std::wstring probe_dir()
{
    wchar_t tmp[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return L".";
    return std::wstring(tmp) + L"DormProbe";
}

std::wstring service_tag(const WDBA::BluetoothLEAdvertisement &adv)
{
    const winrt::guid wanted(protocol::kServiceUuid);
    const auto uuids = adv.ServiceUuids();
    for (uint32_t i = 0; i < uuids.Size(); ++i) {
        if (uuids.GetAt(i) == wanted) return L" [含温湿度服务]";
    }
    return L"";
}

void on_sample(const Reading &r, void *)
{
    sayf(L"  采样  %.2f ℃   %.0f %%RH   %.3f V", r.temp, r.hum, r.volt);
}

int cmd_scan(int seconds)
{
    const std::wstring want = config::get().deviceName;
    std::mutex mtx;
    std::set<unsigned long long> seen;
    int matched = 0;

    sayf(L"扫描 %d 秒（目标广播名 = %s）…", seconds, want.c_str());

    WDBA::BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(WDBA::BluetoothLEScanningMode::Active);
    auto token = watcher.Received(
        [&](WDBA::BluetoothLEAdvertisementWatcher const &,
            WDBA::BluetoothLEAdvertisementReceivedEventArgs const &args) {
            const unsigned long long addr =
                static_cast<unsigned long long>(args.BluetoothAddress());
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!seen.insert(addr).second) return;
            }
            try {
                const std::wstring name = args.Advertisement().LocalName().c_str();
                const bool hit = protocol::name_matches(name.c_str(), want.c_str());
                if (hit) ++matched;
                sayf(L"  %s  RSSI %4d  %-20s%s%s",
                     config::format_mac(addr).c_str(),
                     static_cast<int>(args.RawSignalStrengthInDBm()),
                     name.empty() ? L"(无名称)" : name.c_str(),
                     service_tag(args.Advertisement()).c_str(),
                     hit ? L"  <= 匹配" : L"");
            } catch (...) {
            }
        });

    watcher.Start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    watcher.Stop();
    watcher.Received(token);

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(mtx);
        total = seen.size();
    }
    sayf(L"共 %zu 个设备，命中 %d 个。", total, matched);
    if (matched == 0) {
        say(L"提示：传感器休眠时不广播，按一下侧键唤醒后重试；");
        sayf(L"      若名字不是 %s，把皮肤里的 DeviceName 改成上面实际的名字即可。",
             want.c_str());
    }
    return matched > 0 ? 0 : 1;
}

int cmd_read(unsigned long long addr, int seconds)
{
    // 有 measure 才不会触发「空闲 60 秒就断开」
    state::measure_add();
    state::touch();
    state::set_publish_hook(&on_sample, nullptr);

    sayf(L"连接 %s，监听 %d 秒…", config::format_mac(addr).c_str(), seconds);
    ble::run_connection(addr, seconds * 1000);

    const Snapshot s = state::get();
    sayf(L"结果：状态=%s  MAC=%s  最近采样 %.2f ℃ / %.0f %%RH / %.3f V",
         status_text(s.status, s.ever, seconds_since(s.lastSampleFT)).c_str(),
         s.mac, s.temp, s.hum, s.volt);

    state::set_publish_hook(nullptr, nullptr);
    state::measure_remove();
    return s.lastSampleFT ? 0 : 1;
}

int cmd_db(const std::wstring &dir)
{
    if (!store::init(dir)) {
        sayf(L"打不开数据库：%s", dir.c_str());
        return 1;
    }
    const long long n = store::count();
    sayf(L"%s\\dormtemp.db", dir.c_str());
    sayf(L"记录数：%lld", n);

    store::Row rows[5] = {};
    const int got = store::recent(5, rows);
    for (int i = 0; i < got; ++i) {
        sayf(L"  ts=%lld  %.2f ℃  %.0f %%RH  %.3f V",
             rows[i].ts, rows[i].temp, rows[i].hum, rows[i].volt);
    }

    stats::refresh();
    say(L"汇总：");
    for (int w = 0; w < stats::kWindowCount; ++w) {
        sayf(L"  [%s] %s", stats::window_label(w), stats::summary_text(stats::get(w)).c_str());
    }
    store::close();
    return 0;
}

int cmd_chart(const std::wstring &dir, int hours, int series, bool dual)
{
    if (!store::init(dir)) {
        sayf(L"打不开数据库：%s", dir.c_str());
        return 1;
    }
    config::set_log_dir(dir);   // 图落在 <数据目录>\chart

    chart::Spec spec;
    spec.hours   = hours > 0 ? hours : 24;
    spec.buckets = 96;
    spec.width   = 180;
    spec.height  = 92;
    spec.series  = series;
    spec.dual    = dual;

    chart::start();
    const int id = chart::open(L"probe", spec);
    if (!id) {
        say(L"注册曲线失败");
        return 1;
    }

    chart::View v;
    const unsigned long long t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 8000) {
        v = chart::view(id);
        if (v.valid) break;
        Sleep(100);
    }

    if (!v.valid) {
        say(L"渲染超时：库里没有这个区间的数据？");
        chart::stop();
        return 1;
    }

    sayf(L"PNG：%s", v.path.c_str());
    sayf(L"样本 %d 条，Y 轴 %.1f ~ %.1f（中 %.1f）", v.n, v.lo, v.hi, v.mid);
    if (dual) sayf(L"右轴 %.0f ~ %.0f（中 %.0f）", v.lo2, v.hi2, v.mid2);
    sayf(L"温度 %.1f ~ %.1f ℃　湿度 %.0f ~ %.0f %%RH", v.tMin, v.tMax, v.hMin, v.hMax);
    chart::stop();
    Sleep(300);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    logx::set_console(false);

    const std::wstring cmd = argc > 1 ? argv[1] : L"scan";

    if (cmd == L"db") {
        const std::wstring dir =
            argc > 2 ? argv[2] : config::expand_env(L"%LOCALAPPDATA%\\DormTempPlugin");
        return cmd_db(dir);
    }

    if (cmd == L"chart") {
        const std::wstring dir =
            argc > 2 ? argv[2] : config::expand_env(L"%LOCALAPPDATA%\\DormTempPlugin");
        const int hours = argc > 3 ? _wtoi(argv[3]) : 24;
        const bool hum = argc > 4 && _wcsicmp(argv[4], L"hum") == 0;
        const bool both = argc > 4 && (_wcsicmp(argv[4], L"both") == 0 || _wcsicmp(argv[4], L"all") == 0);
        return cmd_chart(dir, hours, hum ? chart::kHum : chart::kTemp, both);
    }

    config::set_log_dir(probe_dir());
    config::ensure_log_dir();

    if (cmd == L"scan") {
        const int seconds = argc > 2 ? _wtoi(argv[2]) : 8;
        return cmd_scan(seconds > 0 ? seconds : 8);
    }

    if (cmd == L"read") {
        unsigned long long addr = 0;
        int arg = 2;
        if (argc > 2 && config::parse_mac(argv[2], &addr)) {
            ++arg;
        } else {
            say(L"未指定 MAC，先扫描…");
            addr = ble::scan(8000);
            if (!addr) {
                say(L"没扫到设备，先按一下传感器侧键再试。");
                return 1;
            }
        }
        const int seconds = argc > arg ? _wtoi(argv[arg]) : 20;
        return cmd_read(addr, seconds > 0 ? seconds : 20);
    }

    say(L"用法：");
    say(L"  DormProbe.exe scan [秒]");
    say(L"  DormProbe.exe read [MAC] [秒]");
    say(L"  DormProbe.exe db [数据目录]");
    return 2;
}
