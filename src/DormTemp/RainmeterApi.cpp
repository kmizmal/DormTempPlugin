// ============================================================================
//  RainmeterApi.cpp
// ============================================================================
#include "RainmeterApi.h"

namespace {

typedef LPCWSTR(__stdcall *RmReadString_t)(void *rm, LPCWSTR option, LPCWSTR defValue,
                                           BOOL replaceMeasures);

RmReadString_t g_read_string = nullptr;

} // namespace

namespace rmapi {

void resolve()
{
    if (g_read_string) return;
    HMODULE dll = GetModuleHandleW(L"Rainmeter.dll");
    if (dll) g_read_string = (RmReadString_t)GetProcAddress(dll, "RmReadString");
}

LPCWSTR read(void *rm, const wchar_t *key, const wchar_t *def)
{
    resolve();
    if (g_read_string && rm) {
        LPCWSTR v = g_read_string(rm, key, def, TRUE);
        if (v) return v;
    }
    return def;
}

} // namespace rmapi
