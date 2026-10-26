// 直接测试 hostfxr_initialize_for_runtime_config
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef void *hostfxr_handle;
typedef int32_t (*init_fn)(const wchar_t *, hostfxr_handle *);

static void p(const char *s) { printf("%s\n", s); fflush(stdout); }

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *cfg = argc > 1 ? argv[1]
        : L"C:\\Users\\zmal\\AppData\\Local\\Programs\\Rainmeter\\Plugins\\PluginDormTempImpl.runtimeconfig.json";
    const wchar_t *fxrPath = argc > 2 ? argv[2]
        : L"C:\\Program Files\\dotnet\\host\\fxr\\8.0.31\\hostfxr.dll";

    wchar_t buf[2048];
    swprintf(buf, 2048, L"loading %s", fxrPath); p("loading hostfxr"); fflush(stdout);
    HMODULE h = LoadLibraryW(fxrPath);
    if (!h) { printf("load fail %u\n", GetLastError()); return 1; }
    p("loaded");
    init_fn init = (init_fn)GetProcAddress(h, "hostfxr_initialize_for_runtime_config");
    if (!init) { p("no export"); return 1; }
    hostfxr_handle ctx = NULL;
    int32_t rc = init(cfg, &ctx);
    printf("init rc=0x%08X ctx=%p\n", (unsigned)rc, ctx); fflush(stdout);
    return 0;
}
