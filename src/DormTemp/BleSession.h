// ============================================================================
//  BleSession.h —— BLE 采集状态机（扫描 → 连接 → 订阅 → 保活 → 断线重连）
//
//  所有 WinRT 调用都收在这个模块里：插件与探测工具共用同一套逻辑，
//  协议解析、状态判定、落盘都不在这里（分别由 DormProtocol / SharedState / Store 负责）。
// ============================================================================
#pragma once

namespace ble {

void start();          // 幂等：重复调用不会起第二个采集线程
void stop();
bool running();

void request_reconnect();

// 扫描到目标返回 64 位蓝牙地址，超时返回 0
unsigned long long scan(int timeoutMs);

// 连接并保持订阅；maxMs > 0 时到点主动收工（探测工具用，插件传 0 = 一直守着）
// 返回是否真正建立过链路（订阅成功）。地址解析失败、服务不可达、CCCD 写失败都算失败，
// 调用方据此决定「继续重试记忆地址」还是「退回扫描」。
bool run_connection(unsigned long long addr, int maxMs = 0);

} // namespace ble
