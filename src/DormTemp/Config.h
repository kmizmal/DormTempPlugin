// ============================================================================
//  Config.h —— 皮肤选项与数据目录（全局单例，读写都在锁内完成）
// ============================================================================
#pragma once

#include <string>

namespace config {

struct Values {
    std::wstring logDir;                    // 数据目录（SQLite / 日志）
    std::wstring deviceName = L"MJWSD06MMC"; // 传感器广播名
    std::wstring macOpt;                    // 可选固定 MAC（空 = 自动扫描 + 记忆）
};

Values get();
std::wstring dir();

// 空值不覆盖已有配置（皮肤里只有主 measure 写了这些选项）
void set_log_dir(const std::wstring &v);
void set_device_name(const std::wstring &v);
void set_mac_opt(const std::wstring &v);

// 默认落盘目录：%LOCALAPPDATA%\DormTempPlugin
void ensure_log_dir();

std::wstring expand_env(const std::wstring &in);

// MAC 文本 <-> 64 位地址（容忍 : - . 与空格分隔）
bool parse_mac(const std::wstring &text, unsigned long long *out);
std::wstring format_mac(unsigned long long addr);

// 上次连上的设备（未固定 MAC 时用来跳过重新扫描）
unsigned long long load_remembered_mac();
void save_remembered_mac(unsigned long long addr);

} // namespace config
