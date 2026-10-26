// ============================================================================
//  Log.cpp —— 写入 <数据目录>\plugin_debug.log
//  目录取自 config::dir()；为空时退到 %TEMP%，保证任何情况下都能留下线索
// ============================================================================
#include "Log.h"

#include "Config.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

bool g_console = false;

std::wstring log_path()
{
    std::wstring dir = config::dir();
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH] = {0};
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) dir = tmp;
    }
    if (dir.empty()) dir = L".";
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return dir + L"\\plugin_debug.log";
}

} // namespace

namespace logx {

void set_console(bool on) { g_console = on; }

void write(const wchar_t *msg)
{
    if (!msg) return;

    std::wstring path = log_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02u:%02u:%02u.%03u  %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }

    if (g_console) wprintf(L"%s\n", msg);
}

void writef(const wchar_t *fmt, ...)
{
    wchar_t buf[768];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 768, fmt, ap);
    va_end(ap);
    write(buf);
}

} // namespace logx
