// 测试宿主：模拟 Rainmeter 加载插件并驱动 measure
#include <windows.h>
#include <stdio.h>

typedef void  (__cdecl *InitFn)(void **data, void *rm);
typedef void  (__cdecl *ReloadFn)(void *data, void *rm, double *maxValue);
typedef double(__cdecl *UpdateFn)(void *data);
typedef LPCWSTR(__cdecl *GetStrFn)(void *data);
typedef void  (__cdecl *FinalFn)(void *data);
typedef void  (__cdecl *BangFn)(LPCWSTR args, void *data);

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { printf("usage: test_host <dll>\n"); return 2; }
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    printf("loaded %p\n", (void *)h);

    InitFn    init   = (InitFn)GetProcAddress(h, "Initialize");
    ReloadFn  reload = (ReloadFn)GetProcAddress(h, "Reload");
    UpdateFn  update = (UpdateFn)GetProcAddress(h, "Update");
    GetStrFn  getstr = (GetStrFn)GetProcAddress(h, "GetString");
    FinalFn   fin    = (FinalFn)GetProcAddress(h, "Finalize");
    BangFn    bang   = (BangFn)GetProcAddress(h, "ExecuteBang");

    printf("Initialize=%p Reload=%p Update=%p GetString=%p Finalize=%p ExecuteBang=%p\n",
           (void *)init, (void *)reload, (void *)update, (void *)getstr, (void *)fin, (void *)bang);
    if (!init || !update || !getstr) return 3;

    void *data = NULL;
    init(&data, NULL);
    printf("measure data=%p\n", data);
    if (reload) { double mx = 0; reload(data, NULL, &mx); }

    for (int i = 0; i < 40; i++) {
        Sleep(500);
        double v = update(data);
        LPCWSTR s = getstr(data);
        printf("t=%2ds  update=%.2f  str=%ls\n", (i + 1) / 2, v, s ? s : L"(null)");
        fflush(stdout);
    }

    if (bang) bang(L"Reconnect", data);
    for (int i = 0; i < 10; i++) {
        Sleep(500);
        printf("  post-bang str=%ls\n", getstr(data));
        fflush(stdout);
    }

    if (fin) fin(data);
    FreeLibrary(h);
    printf("done\n");
    return 0;
}
