// ============================================================================
//  Config.cpp
// ============================================================================
#include "Config.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
config::Values g_cfg;

} // namespace

namespace config {

Values get()
{
    AcquireSRWLockShared(&g_lock);
    Values v = g_cfg;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

std::wstring dir() { return get().logDir; }

void set_log_dir(const std::wstring &v)
{
    if (v.empty()) return;
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.logDir = v;
    ReleaseSRWLockExclusive(&g_lock);
}

void set_device_name(const std::wstring &v)
{
    if (v.empty()) return;
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.deviceName = v;
    ReleaseSRWLockExclusive(&g_lock);
}

void set_mac_opt(const std::wstring &v)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.macOpt = v;
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring expand_env(const std::wstring &in)
{
    wchar_t buf[1024];
    DWORD n = ExpandEnvironmentStringsW(in.c_str(), buf, 1024);
    if (n > 0 && n <= 1024) return std::wstring(buf);
    return in;
}

void ensure_log_dir()
{
    if (!dir().empty()) return;
    set_log_dir(expand_env(L"%LOCALAPPDATA%\\DormTempPlugin"));
}

bool parse_mac(const std::wstring &text, unsigned long long *out)
{
    if (!out) return false;

    std::wstring hex;
    for (wchar_t c : text) {
        if (iswxdigit(c)) hex.push_back(c);
    }
    if (hex.size() < 6 || hex.size() > 12 || (hex.size() % 2) != 0) return false;

    wchar_t *end = nullptr;
    unsigned long long v = wcstoull(hex.c_str(), &end, 16);
    if (!end || *end) return false;

    *out = v;
    return true;
}

std::wstring format_mac(unsigned long long addr)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%012llX", addr);
    std::wstring s(buf);
    for (int i = 10; i > 0; i -= 2) s.insert(s.begin() + i, L':');
    return s;
}

unsigned long long load_remembered_mac()
{
    const std::wstring d = dir();
    if (d.empty()) return 0;

    std::filesystem::path p = std::filesystem::path(d) / L"device_mac.txt";
    FILE *f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8") != 0 || !f) return 0;

    wchar_t buf[64] = {0};
    fgetws(buf, 64, f);
    fclose(f);

    unsigned long long v = 0;
    return parse_mac(buf, &v) ? v : 0;
}

void save_remembered_mac(unsigned long long addr)
{
    if (!get().macOpt.empty()) return;   // 用户固定了 MAC，不再记忆

    const std::wstring d = dir();
    if (d.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(d, ec);

    std::filesystem::path p = std::filesystem::path(d) / L"device_mac.txt";
    FILE *f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%s", format_mac(addr).c_str());
    fclose(f);
}

} // namespace config
