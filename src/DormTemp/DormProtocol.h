// ============================================================================
//  DormProtocol.h —— MJWSD06MMC（小米温湿度计 3 mini）明文 GATT 协议层
//  纯函数、不碰 WinRT：可以被测试直接覆盖
//
//  通知负载（5 字节小端）：
//    [0..1] int16  温度 ×100 ℃
//    [2]    uint8  湿度 %RH
//    [3..4] uint16 电压 mV
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

struct Reading {
    bool   valid = false;
    double temp  = std::numeric_limits<double>::quiet_NaN();
    double hum   = std::numeric_limits<double>::quiet_NaN();
    double volt  = std::numeric_limits<double>::quiet_NaN();
};

namespace protocol {

inline constexpr wchar_t kServiceUuid[]        = L"ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
inline constexpr wchar_t kCharacteristicUuid[] = L"ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";

// 少于 5 字节视为无效帧；多余字节忽略（部分固件会补齐）
inline constexpr std::size_t kMinPayload = 5;

bool parse(const std::uint8_t *data, std::size_t len, Reading *out);

// 广播名匹配（不区分大小写）；want 为空视为不匹配
bool name_matches(const wchar_t *advertised, const wchar_t *wanted);

} // namespace protocol
