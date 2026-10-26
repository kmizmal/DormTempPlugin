// 独立测试宿主：模拟 Rainmeter 调用插件导出
#include <stdint.h>
#include <windows.h>
#include <stdio.h>

typedef uint32_t (*init_fn)(const wchar_t*, const wchar_t*, uint32_t);
typedef double (*upd_fn)(uint32_t);
typedef const wchar_t* (*getstr_fn)(uint32_t);

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dll = argc > 1 ? argv[1] : L"C:\\Users\\zmal\\AppData\\Local\\Programs\\Rainmeter\\Plugins\\DormTemp.dll";
    const wchar_t* ini = argc > 2 ? argv[2] : L"C:\\Users\\zmal\\Documents\\Rainmeter\\Skins\\DormTempMonitor\\DormTempMonitor.ini";

    HMODULE h = LoadLibraryW(dll);
    if (!h) { wprintf(L"LoadLibrary failed %u\n", GetLastError()); fflush(stdout); return 1; }
    wprintf(L"loaded\n"); fflush(stdout);

    init_fn init = (init_fn)GetProcAddress(h, "Initialize");
    upd_fn upd = (upd_fn)GetProcAddress(h, "Update");
    getstr_fn gs = (getstr_fn)GetProcAddress(h, "GetString");
    if (!init || !upd || !gs) { wprintf(L"exports missing\n"); fflush(stdout); return 1; }

    uint32_t id = init(ini, L"MeasureTemp", 1);
    wprintf(L"Initialize -> %u\n", id); fflush(stdout);

    for (int i = 0; i < 5; i++) {
        Sleep(2000);
        double v = upd(1);
        const wchar_t* s = gs(1);
        wprintf(L"t+%ds Update=%.2f GetString=%s\n", (i+1)*2, v, s ? s : L"(null)");
        fflush(stdout);
    }
    wprintf(L"done\n");
    return 0;
}
