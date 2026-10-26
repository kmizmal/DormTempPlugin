// ============================================================================
//  DormTemp.dll —— Rainmeter 宿舍温湿度插件（单文件 DLL 版）
//  · 全部逻辑在 DLL 内：C++/WinRT 直接调用 Windows.Devices.Bluetooth 采集 BLE
//  · 不依赖 .NET / Python / 任何外部进程与共享内存
//  · Rainmeter Plugin API v3
//  构建（zig c++，无需 MSVC / Windows SDK）：
//    zig c++ -target x86_64-windows-gnu -std=c++20 -fms-extensions -shared -O2 \
//        -I winrt_gen -o DormTemp.dll DormTempMain.cpp RoShim.cpp -lole32 -loleaut32
// ============================================================================
#include <assert.h>
#ifndef _ASSERTE
#define _ASSERTE(x) ((void)0)
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <filesystem>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include "store.h"

namespace WDB  = winrt::Windows::Devices::Bluetooth;
namespace WDBA = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace WDBG = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

// LYWSD06MMC（小米温湿度计 3 mini）明文 GATT
static const winrt::guid GATT_SERVICE = winrt::guid(L"ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
static const winrt::guid GATT_CHAR    = winrt::guid(L"ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");

// ---------------------------------------------------------------- 状态快照
struct Snapshot {
    double temp  = NAN;
    double hum   = NAN;
    double volt  = NAN;
    ULONGLONG lastSampleFT = 0;   // 0 = 从未采样
    int  status  = 0;             // 0 搜索中 / 1 连接中 / 2 在线
    bool ever    = false;         // 曾经连上过
    wchar_t mac[32] = {0};
};

static SRWLOCK   g_stateLock = SRWLOCK_INIT;
static Snapshot  g_snap;

// ---------------------------------------------------------------- 配置
static SRWLOCK       g_cfgLock = SRWLOCK_INIT;
static std::wstring  g_logDir;          // CSV / 日志目录
static std::wstring  g_devName = L"MJWSD06MMC";
static std::wstring  g_macOpt;          // 可选固定 MAC

static wchar_t       g_dllDir[MAX_PATH] = {0};
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_starting{false};
static std::atomic<int>  g_measureCount{0};
static std::atomic<ULONGLONG> g_lastActive{0};
static std::atomic<bool> g_forceReconnect{false};

// ---------------------------------------------------------------- 小工具
static ULONGLONG now_ft()
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static double seconds_since(ULONGLONG ft)
{
    if (ft == 0) return -1;
    ULONGLONG n = now_ft();
    if (n < ft) return 0;
    return (double)(n - ft) / 10000000.0;
}

static std::wstring mac_text(ULONGLONG a)
{
    wchar_t b[32];
    swprintf(b, 32, L"%012llX", (unsigned long long)a);
    std::wstring s(b);
    for (int i = 10; i > 0; i -= 2) s.insert(s.begin() + i, L':');
    return s;
}

static bool parse_mac(const std::wstring &in, ULONGLONG *out)
{
    std::wstring s;
    for (wchar_t c : in) if (iswxdigit(c)) s.push_back(c);
    if (s.size() < 6 || s.size() > 12) return false;
    if (s.size() % 2) return false;
    wchar_t *end = nullptr;
    ULONGLONG v = wcstoull(s.c_str(), &end, 16);
    if (!end || *end) return false;
    *out = v;
    return true;
}

// ---------------------------------------------------------------- 日志
static void native_log(const wchar_t *msg)
{
    std::wstring dir;
    { AcquireSRWLockShared(&g_cfgLock); dir = g_logDir; ReleaseSRWLockShared(&g_cfgLock); }
    if (dir.empty()) dir = std::wstring(g_dllDir);
    if (dir.empty()) dir = L".";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path p = std::filesystem::path(dir) / L"plugin_debug.log";
    FILE *f = NULL;
    if (_wfopen_s(&f, p.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

static void logf(const wchar_t *fmt, ...)
{
    wchar_t buf[768];
    va_list ap; va_start(ap, fmt);
    vswprintf(buf, 768, fmt, ap);
    va_end(ap);
    native_log(buf);
}

// ---------------------------------------------------------------- 采样解析
static void handle_sample(const uint8_t *b, uint32_t n)
{
    if (n < 5) return;
    int16_t rawT = (int16_t)(b[0] | (b[1] << 8));
    double temp = rawT / 100.0;
    double hum  = b[2];
    double volt = (b[3] | (b[4] << 8)) / 1000.0;

    {
        AcquireSRWLockExclusive(&g_stateLock);
        g_snap.temp = temp;
        g_snap.hum  = hum;
        g_snap.volt = volt;
        g_snap.lastSampleFT = now_ft();
        ReleaseSRWLockExclusive(&g_stateLock);
    }

    store::sample(temp, hum, volt);
}

static void set_status(int s)
{
    AcquireSRWLockExclusive(&g_stateLock);
    g_snap.status = s;
    ReleaseSRWLockExclusive(&g_stateLock);
}

static void set_mac(ULONGLONG a, bool ever)
{
    std::wstring m = mac_text(a);
    AcquireSRWLockExclusive(&g_stateLock);
    wcsncpy(g_snap.mac, m.c_str(), 31); g_snap.mac[31] = 0;
    g_snap.ever = ever;
    ReleaseSRWLockExclusive(&g_stateLock);
}

static Snapshot get_snapshot()
{
    AcquireSRWLockShared(&g_stateLock);
    Snapshot s = g_snap;
    ReleaseSRWLockShared(&g_stateLock);
    return s;
}

// ---------------------------------------------------------------- MAC 记忆
static std::wstring cfg_dir()
{
    std::wstring dir;
    AcquireSRWLockShared(&g_cfgLock); dir = g_logDir; ReleaseSRWLockShared(&g_cfgLock);
    return dir;
}

static ULONGLONG load_remembered_mac()
{
    std::wstring dir = cfg_dir();
    if (dir.empty()) return 0;
    std::filesystem::path p = std::filesystem::path(dir) / L"device_mac.txt";
    FILE *f = NULL;
    if (_wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8") != 0 || !f) return 0;
    wchar_t buf[64] = {0};
    fgetws(buf, 64, f);
    fclose(f);
    ULONGLONG v = 0;
    return parse_mac(buf, &v) ? v : 0;
}

static void save_remembered_mac(ULONGLONG addr)
{
    { AcquireSRWLockShared(&g_cfgLock); if (!g_macOpt.empty()) { ReleaseSRWLockShared(&g_cfgLock); return; } ReleaseSRWLockShared(&g_cfgLock); }
    std::wstring dir = cfg_dir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path p = std::filesystem::path(dir) / L"device_mac.txt";
    FILE *f = NULL;
    if (_wfopen_s(&f, p.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%s", mac_text(addr).c_str());
    fclose(f);
}

// ---------------------------------------------------------------- 采集线程
static std::atomic<ULONGLONG> g_scanFound{0};
static std::mutex g_seenMutex;
static std::vector<std::wstring> g_seenList;
static std::wstring g_lastSeenSig;

// 扫描：返回 0 表示超时未找到
static ULONGLONG scan_for_device(int timeoutMs)
{
    std::wstring want;
    { AcquireSRWLockShared(&g_cfgLock); want = g_devName; ReleaseSRWLockShared(&g_cfgLock); }

    g_scanFound = 0;
    { std::lock_guard<std::mutex> lk(g_seenMutex); g_seenList.clear(); }
    ULONGLONG found = 0;
    try {
        WDBA::BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(WDBA::BluetoothLEScanningMode::Active);
        auto tok = watcher.Received([want](WDBA::BluetoothLEAdvertisementWatcher const &,
                                           WDBA::BluetoothLEAdvertisementReceivedEventArgs const &a) {
            try {
                std::wstring name = a.Advertisement().LocalName().c_str();
                bool match = !want.empty() && _wcsicmp(name.c_str(), want.c_str()) == 0;
                if (!match) {
                    auto uuids = a.Advertisement().ServiceUuids();
                    for (uint32_t i = 0; i < uuids.Size(); i++) {
                        if (uuids.GetAt(i) == GATT_SERVICE) { match = true; break; }
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(g_seenMutex);
                    if (g_seenList.size() < 40) {
                        wchar_t b[128];
                        swprintf(b, 128, L"%012llX %s", (unsigned long long)a.BluetoothAddress(),
                                 name.empty() ? L"-" : name.c_str());
                        std::wstring s(b);
                        bool dup = false;
                        for (auto &x : g_seenList) if (x == s) { dup = true; break; }
                        if (!dup) g_seenList.push_back(s);
                    }
                }
                if (match && g_scanFound.load() == 0) {
                    g_scanFound = (ULONGLONG)a.BluetoothAddress();
                }
            } catch (...) {}
        });
        watcher.Start();
        set_status(0);
        int waited = 0;
        while (!g_stop && waited < timeoutMs) {
            if (g_forceReconnect.exchange(false)) { g_scanFound = 0; }
            found = g_scanFound.load();
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            waited += 120;
        }
        watcher.Stop();
        watcher.Received(tok);
    } catch (winrt::hresult_error const &e) {
        logf(L"scan hr=0x%08X", (unsigned)e.code());
    } catch (...) {}

    if (found) {
        logf(L"scan found %s", mac_text(found).c_str());
    } else {
        // 明细只在设备集合变化时记录，避免日志被刷爆
        std::wstring sig;
        {
            std::lock_guard<std::mutex> lk(g_seenMutex);
            for (auto &s : g_seenList) { sig += s; sig += L"|"; }
        }
        if (sig != g_lastSeenSig) {
            g_lastSeenSig = sig;
            std::lock_guard<std::mutex> lk(g_seenMutex);
            logf(L"scan miss (%d seen)", (int)g_seenList.size());
            for (auto &s : g_seenList) logf(L"  seen %s", s.c_str());
        }
    }
    return found;
}

// 连接 + 订阅 + 保活；返回时设备已断开
static void run_connection(ULONGLONG addr)
{
    WDB::BluetoothLEDevice dev = nullptr;
    WDBG::GattCharacteristic ch = nullptr;
    winrt::event_token chTok{};

    try {
        set_status(1);
        dev = WDB::BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
        if (!dev) { logf(L"connect: dev null"); return; }

        auto svcRes = dev.GetGattServicesForUuidAsync(GATT_SERVICE, WDB::BluetoothCacheMode::Uncached).get();
        if (svcRes.Status() != WDBG::GattCommunicationStatus::Success || svcRes.Services().Size() == 0) {
            logf(L"connect: svc status=%d", (int)svcRes.Status());
            return;
        }
        auto chRes = svcRes.Services().GetAt(0)
                        .GetCharacteristicsForUuidAsync(GATT_CHAR, WDB::BluetoothCacheMode::Uncached).get();
        if (chRes.Status() != WDBG::GattCommunicationStatus::Success || chRes.Characteristics().Size() == 0) {
            logf(L"connect: char status=%d", (int)chRes.Status());
            return;
        }
        ch = chRes.Characteristics().GetAt(0);
        chTok = ch.ValueChanged([](WDBG::GattCharacteristic const &,
                                   WDBG::GattValueChangedEventArgs const &a) {
            try {
                auto buf = a.CharacteristicValue();
                uint32_t n = buf.Length();
                if (n == 0 || n > 32) return;
                uint8_t raw[32] = {0};
                winrt::Windows::Storage::Streams::DataReader reader =
                    winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
                reader.ReadBytes(winrt::array_view<uint8_t>(raw, raw + n));
                handle_sample(raw, n);
            } catch (...) {}
        });

        auto st = ch.WriteClientCharacteristicConfigurationDescriptorAsync(
                      WDBG::GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (st != WDBG::GattCommunicationStatus::Success) {
            st = ch.WriteClientCharacteristicConfigurationDescriptorAsync(
                     WDBG::GattClientCharacteristicConfigurationDescriptorValue::Indicate).get();
        }
        if (st != WDBG::GattCommunicationStatus::Success) {
            logf(L"connect: cccd fail %d", (int)st);
            return;
        }

        set_mac(addr, true);
        save_remembered_mac(addr);
        set_status(2);
        logf(L"connected %s", mac_text(addr).c_str());

        // 立刻读一次
        auto r0 = ch.ReadValueAsync(WDB::BluetoothCacheMode::Uncached).get();
        if (r0.Status() == WDBG::GattCommunicationStatus::Success) {
            auto buf = r0.Value();
            uint32_t n = buf.Length();
            if (n > 0 && n <= 32) {
                uint8_t raw[32] = {0};
                winrt::Windows::Storage::Streams::DataReader rd =
                    winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
                rd.ReadBytes(winrt::array_view<uint8_t>(raw, raw + n));
                handle_sample(raw, n);
            }
        }

        // 保活循环
        ULONGLONG lastRead = GetTickCount64();
        while (!g_stop) {
            if (g_forceReconnect.exchange(false)) { logf(L"bang: reconnect"); break; }
            if (g_measureCount <= 0 && GetTickCount64() - g_lastActive.load() > 60000) { logf(L"idle: detach"); break; }
            bool connected = false;
            try { connected = dev.ConnectionStatus() == WDB::BluetoothConnectionStatus::Connected; }
            catch (...) { break; }
            if (!connected) { logf(L"disconnected"); break; }

            ULONGLONG t = GetTickCount64();
            if (t - lastRead >= 30000) {
                lastRead = t;
                try {
                    auto r = ch.ReadValueAsync(WDB::BluetoothCacheMode::Uncached).get();
                    if (r.Status() == WDBG::GattCommunicationStatus::Success) {
                        auto buf = r.Value();
                        uint32_t n = buf.Length();
                        if (n > 0 && n <= 32) {
                            uint8_t raw[32] = {0};
                            winrt::Windows::Storage::Streams::DataReader rd =
                                winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
                            rd.ReadBytes(winrt::array_view<uint8_t>(raw, raw + n));
                            handle_sample(raw, n);
                        }
                    }
                } catch (...) { break; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (winrt::hresult_error const &e) {
        logf(L"conn hr=0x%08X %s", (unsigned)e.code(), e.message().c_str());
    } catch (std::exception const &e) {
        logf(L"conn std: %hs", e.what());
    } catch (...) {
        logf(L"conn unknown");
    }

    if (ch) { try { ch.ValueChanged(chTok); } catch (...) {} }
    ch = nullptr;
    dev = nullptr;
}

static void collector_thread()
{
    g_running = true;
    g_starting = false;
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (...) { logf(L"init_apartment failed"); }

    ULONGLONG target = 0;
    bool haveTarget = false;

    while (!g_stop) {
        // 皮肤未激活（无 measure 且静默超过 60s）：待机，不断扫蓝牙
        if (g_measureCount <= 0 && GetTickCount64() - g_lastActive.load() > 60000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        // 每轮刷新配置
        std::wstring macOpt, logDir;
        { AcquireSRWLockShared(&g_cfgLock); macOpt = g_macOpt; logDir = g_logDir; ReleaseSRWLockShared(&g_cfgLock); }

        // 数据落地：SQLite（winsqlite3.dll）
        {
            static bool logged = false;
            bool ok = store::init(logDir);
            if (!logged) {
                logged = true;
                if (ok) logf(L"storage: sqlite %s\\dormtemp.db", logDir.c_str());
                else    logf(L"storage: sqlite unavailable (winsqlite3.dll missing)");
            }
        }

        ULONGLONG fixedMac = 0;
        bool hasFixed = parse_mac(macOpt, &fixedMac);

        if (hasFixed) { target = fixedMac; haveTarget = true; }
        else if (!haveTarget) {
            ULONGLONG remembered = load_remembered_mac();
            if (remembered) { target = remembered; haveTarget = true; }
        }

        if (!haveTarget) {
            set_status(0);
            ULONGLONG found = scan_for_device(15000);
            if (g_stop) break;
            if (found) { target = found; haveTarget = true; logf(L"scan found %s", mac_text(found).c_str()); }
            else { std::this_thread::sleep_for(std::chrono::milliseconds(2500)); continue; }
        }

        run_connection(target);

        if (g_stop) break;
        // 断开：固定 MAC 保留目标，否则重新扫描
        if (!hasFixed) haveTarget = false;
        set_status(haveTarget ? 1 : 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }

    g_running = false;
}

static void start_collector()
{
    // 防止 Initialize / Reload 并发启动出两个采集线程
    bool expect = false;
    if (!g_starting.compare_exchange_strong(expect, true)) return;
    if (g_running) { g_starting = false; return; }
    g_stop = false;
    std::thread t(collector_thread);
    t.detach();
}

// ---------------------------------------------------------------- Rainmeter API
struct Measure {
    wchar_t type[32];
    wchar_t strbuf[256];
    void *rm;
};

typedef LPCWSTR(__stdcall *RmReadString_t)(void *rm, LPCWSTR option, LPCWSTR defValue, BOOL replaceMeasures);
static RmReadString_t pRmReadString = NULL;

static void resolve_rm_api()
{
    if (pRmReadString) return;
    HMODULE rmdll = GetModuleHandleW(L"Rainmeter.dll");
    if (rmdll) pRmReadString = (RmReadString_t)GetProcAddress(rmdll, "RmReadString");
}

static LPCWSTR rm_read(void *rm, const wchar_t *key, const wchar_t *def)
{
    resolve_rm_api();
    if (pRmReadString && rm) {
        LPCWSTR v = pRmReadString(rm, key, def, TRUE);
        if (v) return v;
    }
    return def;
}

static void expand_env(const wchar_t *in, std::wstring &out)
{
    wchar_t buf[1024];
    DWORD n = ExpandEnvironmentStringsW(in, buf, 1024);
    if (n > 0 && n <= 1024) out = buf;
    else out = in;
}

static void apply_config(void *rm)
{
    LPCWSTR v = rm_read(rm, L"LogPath", L"");
    if (v && v[0]) {
        std::wstring s; expand_env(v, s);
        AcquireSRWLockExclusive(&g_cfgLock); g_logDir = s; ReleaseSRWLockExclusive(&g_cfgLock);
    }
    v = rm_read(rm, L"DeviceName", L"");
    if (v && v[0]) { AcquireSRWLockExclusive(&g_cfgLock); g_devName = v; ReleaseSRWLockExclusive(&g_cfgLock); }
    v = rm_read(rm, L"MacAddress", L"");
    if (v) { AcquireSRWLockExclusive(&g_cfgLock); g_macOpt = v; ReleaseSRWLockExclusive(&g_cfgLock); }
}

static void get_own_dir()
{
    HMODULE hm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&get_own_dir, &hm);
    DWORD n = GetModuleFileNameW(hm, g_dllDir, MAX_PATH);
    while (n > 0 && g_dllDir[n - 1] != L'\\') n--;
    g_dllDir[n] = 0;
}

static const wchar_t *status_text(const Snapshot &s)
{
    if (s.status == 2) {
        double age = seconds_since(s.lastSampleFT);
        if (age >= 0 && age < 180) return L"在线";
        return L"数据超时";
    }
    if (s.status == 1) return s.ever ? L"重连中" : L"连接中";
    return s.ever ? L"离线" : L"搜索中";
}

extern "C" {

__declspec(dllexport) void __cdecl Initialize(void **data, void *rm)
{
    try {
        if (!g_dllDir[0]) get_own_dir();
        Measure *m = (Measure *)calloc(1, sizeof(Measure));
        if (!m) { if (data) *data = NULL; return; }
        m->rm = rm;
        wcsncpy(m->type, rm_read(rm, L"Type", L"Temperature"), 31);
        m->type[31] = 0;
        if (data) *data = m;
        g_measureCount++;
        g_lastActive = GetTickCount64();

        {
            AcquireSRWLockShared(&g_cfgLock); bool empty = g_logDir.empty(); ReleaseSRWLockShared(&g_cfgLock);
            if (empty) {
                std::wstring d; expand_env(L"%LOCALAPPDATA%\\DormTempPlugin", d);
                AcquireSRWLockExclusive(&g_cfgLock); g_logDir = d; ReleaseSRWLockExclusive(&g_cfgLock);
            }
        }

        apply_config(rm);
        logf(L"init type=%s", m->type);
        start_collector();
    } catch (...) {}
}

__declspec(dllexport) void __cdecl Reload(void *data, void *rm, double *maxValue)
{
    try {
        Measure *m = (Measure *)data;
        if (m && rm) {
            m->rm = rm;
            wcsncpy(m->type, rm_read(rm, L"Type", L"Temperature"), 31);
            m->type[31] = 0;
            apply_config(rm);
        }
        if (maxValue) *maxValue = 100.0;
        start_collector();
    } catch (...) { if (maxValue) *maxValue = 100.0; }
}

__declspec(dllexport) double __cdecl Update(void *data)
{
    Measure *m = (Measure *)data;
    if (!m) return 0;
    Snapshot s = get_snapshot();

    if (_wcsicmp(m->type, L"Temperature") == 0) return (std::isfinite(s.temp) && s.temp > -900) ? s.temp : 0;
    if (_wcsicmp(m->type, L"Humidity")    == 0) return (std::isfinite(s.hum)  && s.hum  > -900) ? s.hum  : 0;
    if (_wcsicmp(m->type, L"Voltage")     == 0) return (std::isfinite(s.volt) && s.volt > -900) ? s.volt : 0;
    if (_wcsicmp(m->type, L"SampleAge")   == 0) return seconds_since(s.lastSampleFT);
    if (_wcsicmp(m->type, L"Online")      == 0) return wcscmp(status_text(s), L"在线") == 0 ? 1 : 0;
    return 0;
}

__declspec(dllexport) LPCWSTR __cdecl GetString(void *data)
{
    Measure *m = (Measure *)data;
    if (!m) return L"";
    wchar_t *b = m->strbuf;
    Snapshot s = get_snapshot();

    if (_wcsicmp(m->type, L"Temperature") == 0) {
        if (std::isfinite(s.temp)) swprintf(b, 256, L"%.1f", s.temp); else wcscpy(b, L"--");
    } else if (_wcsicmp(m->type, L"Humidity") == 0) {
        if (std::isfinite(s.hum)) swprintf(b, 256, L"%.0f", s.hum); else wcscpy(b, L"--");
    } else if (_wcsicmp(m->type, L"Voltage") == 0) {
        if (std::isfinite(s.volt)) swprintf(b, 256, L"%.2f", s.volt); else wcscpy(b, L"--");
    } else if (_wcsicmp(m->type, L"LastUpdate") == 0) {
        if (s.lastSampleFT) {
            FILETIME ft; memcpy(&ft, &s.lastSampleFT, 8);
            FILETIME loc; FileTimeToLocalFileTime(&ft, &loc);
            SYSTEMTIME st; FileTimeToSystemTime(&loc, &st);
            swprintf(b, 256, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        } else wcscpy(b, L"--:--");
    } else if (_wcsicmp(m->type, L"Status") == 0) {
        wcsncpy(b, status_text(s), 255); b[255] = 0;
    } else if (_wcsicmp(m->type, L"Battery") == 0) {
        if (!std::isfinite(s.volt)) wcscpy(b, L"--");
        else if (s.volt >= 3.0) wcscpy(b, L"电量充足");
        else if (s.volt >= 2.7) wcscpy(b, L"电量偏低");
        else wcscpy(b, L"需更换电池");
    } else if (_wcsicmp(m->type, L"Comfort") == 0) {
        if (!std::isfinite(s.temp) || !std::isfinite(s.hum)) wcscpy(b, L"--");
        else if (s.temp < 18) wcscpy(b, L"偏冷");
        else if (s.temp > 28) wcscpy(b, L"偏热");
        else if (s.hum < 40)  wcscpy(b, L"干燥");
        else if (s.hum > 70)  wcscpy(b, L"潮湿");
        else wcscpy(b, L"舒适");
    } else if (_wcsicmp(m->type, L"Mac") == 0) {
        wcsncpy(b, s.mac, 255); b[255] = 0;
    } else {
        b[0] = 0;
    }
    return b;
}

__declspec(dllexport) void __cdecl Finalize(void *data)
{
    try {
        Measure *m = (Measure *)data;
        if (!m) return;
        if (--g_measureCount < 0) g_measureCount = 0;
        g_lastActive = GetTickCount64();
        free(m);
    } catch (...) {}
}

__declspec(dllexport) void __cdecl ExecuteBang(LPCWSTR args, void *data)
{
    (void)data;
    try {
        if (args && _wcsicmp(args, L"Reconnect") == 0) {
            native_log(L"bang: reconnect");
            g_forceReconnect = true;
            if (!g_running) { g_stop = true; Sleep(200); start_collector(); }
        }
    } catch (...) {}
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        get_own_dir();
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop = true;
        // 尽量让采集线程收尾，避免退出时访问已卸载代码
        for (int i = 0; i < 60 && g_running; i++) Sleep(50);
        (void)reserved;
    }
    return TRUE;
}
