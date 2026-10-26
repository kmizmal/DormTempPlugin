// ============================================================================
//  BleSession.cpp —— C++/WinRT 直读 BLE（不依赖 .NET / Python / 外部进程）
// ============================================================================
#include "BleSession.h"

#include <assert.h>
#ifndef _ASSERTE
#define _ASSERTE(x) ((void)0)
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include "Config.h"
#include "DormProtocol.h"
#include "Log.h"
#include "SharedState.h"
#include "Stats.h"
#include "Store.h"

namespace WDB  = winrt::Windows::Devices::Bluetooth;
namespace WDBA = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace WDBG = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_starting{false};
std::atomic<bool> g_forceReconnect{false};

std::atomic<unsigned long long> g_scanFound{0};
std::mutex g_seenMutex;
std::vector<std::wstring> g_seenList;
std::wstring g_lastSeenSig;

constexpr int kScanTimeoutMs   = 15000;
constexpr int kKeepAliveMs     = 30000;
constexpr int kIdleDetachMs    = 60000;
constexpr int kReconnectGapMs  = 2000;
constexpr int kScanMissGapMs   = 2500;

winrt::guid service_uuid() { return winrt::guid(protocol::kServiceUuid); }
winrt::guid characteristic_uuid() { return winrt::guid(protocol::kCharacteristicUuid); }

// IBuffer -> 字节数组；超过 32 字节的帧一律忽略（协议只有 5 字节）
bool buffer_to_bytes(const winrt::Windows::Storage::Streams::IBuffer &buf, uint8_t *out, uint32_t *len)
{
    if (!buf) return false;
    const uint32_t n = buf.Length();
    if (n == 0 || n > 32) return false;

    auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
    reader.ReadBytes(winrt::array_view<uint8_t>(out, out + n));
    *len = n;
    return true;
}

void take_sample(const winrt::Windows::Storage::Streams::IBuffer &buf)
{
    uint8_t raw[32] = {0};
    uint32_t n = 0;
    if (!buffer_to_bytes(buf, raw, &n)) return;

    Reading r;
    if (!protocol::parse(raw, n, &r)) return;

    state::publish(r, now_ft());
    store::sample(r.temp, r.hum, r.volt);

    // 汇总卡片只在采集线程重算，主线程读缓存；20 秒节流，采样再密也不会把 SQL 打满
    stats::refresh_if_stale(20);
}

} // namespace

namespace ble {

unsigned long long scan(int timeoutMs)
{
    const std::wstring want = config::get().deviceName;

    g_scanFound = 0;
    {
        std::lock_guard<std::mutex> lk(g_seenMutex);
        g_seenList.clear();
    }

    unsigned long long found = 0;
    try {
        WDBA::BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(WDBA::BluetoothLEScanningMode::Active);

        auto token = watcher.Received(
            [want](WDBA::BluetoothLEAdvertisementWatcher const &,
                   WDBA::BluetoothLEAdvertisementReceivedEventArgs const &args) {
                try {
                    const std::wstring name = args.Advertisement().LocalName().c_str();
                    bool match = protocol::name_matches(name.c_str(), want.c_str());

                    // 很多温湿度计把名字放在 scan response 里，取不到名时靠服务 UUID 认人
                    if (!match) {
                        const auto uuids = args.Advertisement().ServiceUuids();
                        const winrt::guid svc = service_uuid();
                        for (uint32_t i = 0; i < uuids.Size(); ++i) {
                            if (uuids.GetAt(i) == svc) { match = true; break; }
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lk(g_seenMutex);
                        if (g_seenList.size() < 40) {
                            wchar_t buf[128];
                            swprintf(buf, 128, L"%012llX %s",
                                     static_cast<unsigned long long>(args.BluetoothAddress()),
                                     name.empty() ? L"-" : name.c_str());
                            const std::wstring line(buf);
                            bool dup = false;
                            for (const auto &it : g_seenList) {
                                if (it == line) { dup = true; break; }
                            }
                            if (!dup) g_seenList.push_back(line);
                        }
                    }

                    if (match && g_scanFound.load() == 0) {
                        g_scanFound = static_cast<unsigned long long>(args.BluetoothAddress());
                    }
                } catch (...) {
                }
            });

        watcher.Start();
        state::set_status(kSearching);

        int waited = 0;
        while (!g_stop && waited < timeoutMs) {
            if (g_forceReconnect.exchange(false)) g_scanFound = 0;
            found = g_scanFound.load();
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            waited += 120;
        }

        watcher.Stop();
        watcher.Received(token);
    } catch (winrt::hresult_error const &e) {
        logx::writef(L"scan hr=0x%08X", static_cast<unsigned>(e.code()));
    } catch (...) {
    }

    if (found) {
        logx::writef(L"scan found %s", config::format_mac(found).c_str());
    } else {
        // 明细只在「扫到的设备集合」变化时记录，否则日志每 15 秒刷一遍
        std::wstring sig;
        {
            std::lock_guard<std::mutex> lk(g_seenMutex);
            for (const auto &s : g_seenList) { sig += s; sig += L"|"; }
        }
        if (sig != g_lastSeenSig) {
            g_lastSeenSig = sig;
            std::lock_guard<std::mutex> lk(g_seenMutex);
            logx::writef(L"scan miss (%d seen)", static_cast<int>(g_seenList.size()));
            for (const auto &s : g_seenList) logx::writef(L"  seen %s", s.c_str());
        }
    }
    return found;
}

void run_connection(unsigned long long addr, int maxMs)
{
    WDB::BluetoothLEDevice device = nullptr;
    WDBG::GattCharacteristic characteristic = nullptr;
    winrt::event_token token{};
    const unsigned long long started = GetTickCount64();

    try {
        state::set_status(kConnecting);
        device = WDB::BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
        if (!device) {
            logx::write(L"connect: dev null");
            return;
        }

        auto services = device.GetGattServicesForUuidAsync(service_uuid(),
                                                           WDB::BluetoothCacheMode::Uncached).get();
        if (services.Status() != WDBG::GattCommunicationStatus::Success ||
            services.Services().Size() == 0) {
            logx::writef(L"connect: svc status=%d", static_cast<int>(services.Status()));
            return;
        }

        auto chars = services.Services().GetAt(0)
                         .GetCharacteristicsForUuidAsync(characteristic_uuid(),
                                                         WDB::BluetoothCacheMode::Uncached).get();
        if (chars.Status() != WDBG::GattCommunicationStatus::Success ||
            chars.Characteristics().Size() == 0) {
            logx::writef(L"connect: char status=%d", static_cast<int>(chars.Status()));
            return;
        }

        characteristic = chars.Characteristics().GetAt(0);
        token = characteristic.ValueChanged(
            [](WDBG::GattCharacteristic const &, WDBG::GattValueChangedEventArgs const &args) {
                try {
                    take_sample(args.CharacteristicValue());
                } catch (...) {
                }
            });

        auto status = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                          WDBG::GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status != WDBG::GattCommunicationStatus::Success) {
            status = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                         WDBG::GattClientCharacteristicConfigurationDescriptorValue::Indicate).get();
        }
        if (status != WDBG::GattCommunicationStatus::Success) {
            logx::writef(L"connect: cccd fail %d", static_cast<int>(status));
            return;
        }

        state::set_mac(addr, true);
        config::save_remembered_mac(addr);
        state::set_status(kOnline);
        logx::writef(L"connected %s", config::format_mac(addr).c_str());

        // 订阅成功后立刻读一次，不必等第一次通知
        auto first = characteristic.ReadValueAsync(WDB::BluetoothCacheMode::Uncached).get();
        if (first.Status() == WDBG::GattCommunicationStatus::Success) {
            take_sample(first.Value());
        }

        unsigned long long lastRead = GetTickCount64();
        while (!g_stop) {
            if (g_forceReconnect.exchange(false)) {
                logx::write(L"bang: reconnect");
                break;
            }
            if (maxMs > 0 && GetTickCount64() - started > static_cast<unsigned long long>(maxMs)) {
                logx::write(L"probe: timeout");
                break;
            }
            if (state::measure_count() <= 0 && state::idle_ms() > kIdleDetachMs) {
                logx::write(L"idle: detach");
                break;
            }

            bool connected = false;
            try {
                connected = device.ConnectionStatus() == WDB::BluetoothConnectionStatus::Connected;
            } catch (...) {
                break;
            }
            if (!connected) {
                logx::write(L"disconnected");
                break;
            }

            const unsigned long long now = GetTickCount64();
            if (now - lastRead >= kKeepAliveMs) {
                lastRead = now;
                try {
                    auto res = characteristic.ReadValueAsync(WDB::BluetoothCacheMode::Uncached).get();
                    if (res.Status() == WDBG::GattCommunicationStatus::Success) take_sample(res.Value());
                } catch (...) {
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (winrt::hresult_error const &e) {
        logx::writef(L"conn hr=0x%08X %s", static_cast<unsigned>(e.code()), e.message().c_str());
    } catch (std::exception const &e) {
        logx::writef(L"conn std: %hs", e.what());
    } catch (...) {
        logx::write(L"conn unknown");
    }

    if (characteristic) {
        try { characteristic.ValueChanged(token); } catch (...) {}
    }
    characteristic = nullptr;
    device = nullptr;
}

void start()
{
    // Initialize 与 Reload 都会调这里：用 CAS 保证只起一个采集线程
    bool expected = false;
    if (!g_starting.compare_exchange_strong(expected, true)) return;
    if (g_running) {
        g_starting = false;
        return;
    }
    g_stop = false;

    std::thread([] {
        g_running = true;
        g_starting = false;

        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            logx::write(L"init_apartment failed");
        }

        unsigned long long target = 0;
        bool haveTarget = false;

        while (!g_stop) {
            // 皮肤没加载（无 measure 且长时间没人读）：待机，别一直扫蓝牙
            if (state::measure_count() <= 0 && state::idle_ms() > kIdleDetachMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            const config::Values cfg = config::get();

            {
                static bool logged = false;
                const bool ok = store::init(cfg.logDir);
                if (!logged) {
                    logged = true;
                    if (ok) logx::writef(L"storage: sqlite %s\\dormtemp.db", cfg.logDir.c_str());
                    else    logx::write(L"storage: sqlite unavailable (winsqlite3.dll missing)");
                }
                // 启动即把历史汇总算一遍，皮肤一加载卡片就有数（不必等下一次采样）
                if (ok) stats::refresh_if_stale(30);
            }

            unsigned long long fixedMac = 0;
            const bool hasFixed = config::parse_mac(cfg.macOpt, &fixedMac);

            if (hasFixed) {
                target = fixedMac;
                haveTarget = true;
            } else if (!haveTarget) {
                const unsigned long long remembered = config::load_remembered_mac();
                if (remembered) {
                    target = remembered;
                    haveTarget = true;
                }
            }

            if (!haveTarget) {
                state::set_status(kSearching);
                const unsigned long long found = scan(kScanTimeoutMs);
                if (g_stop) break;
                if (found) {
                    target = found;
                    haveTarget = true;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kScanMissGapMs));
                    continue;
                }
            }

            run_connection(target);

            if (g_stop) break;
            if (!hasFixed) haveTarget = false;
            state::set_status(haveTarget ? kConnecting : kSearching);
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectGapMs));
        }

        g_running = false;
    }).detach();
}

void stop() { g_stop = true; }

bool running() { return g_running.load(); }

void request_reconnect()
{
    logx::write(L"bang: reconnect");
    g_forceReconnect = true;
    if (!g_running) {
        g_stop = true;
        Sleep(200);
        start();
    }
}

} // namespace ble
