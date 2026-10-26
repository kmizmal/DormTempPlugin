// DormTemp Rainmeter 插件（Rainmeter Plugin API v3，纯 C，无额外运行时依赖）
// 数据源: DormTempService.exe 通过共享内存推送 BLE 采集结果（由本插件按需拉起）
// 构建: zig cc -target x86_64-windows-gnu -shared -O2 -o DormTemp.dll shim.c -lkernel32 -luser32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

// ---------------- 共享内存布局（与 DormTempService/Program.cs 一致） ----------------
#define MAP_NAME   L"Local\\DormTempPlugin_IPC"
#define MAP_SIZE   256
#define MAGIC      0x4D544244u
#define OFF_TEMP   8
#define OFF_HUM    16
#define OFF_VOLT   24
#define OFF_LAST   32
#define OFF_STATUS 40
#define OFF_EVER   44
#define OFF_BEAT   48
#define OFF_MAC    56

typedef struct {
    wchar_t type[32];
    wchar_t strbuf[192];
    void *rm;
} Measure;

typedef struct {
    int valid;
    double temp, hum, volt;
    ULONGLONG lastSampleFT;
    int status, ever;
    ULONGLONG heartbeatFT;
    wchar_t mac[32];
} Snapshot;

static Measure **g_list = NULL;
static int g_listCount = 0, g_listCap = 0;

static CRITICAL_SECTION g_lock;
static int g_lockInit = 0;
static HANDLE g_map = NULL;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_logDir[512];
static wchar_t g_devName[128];
static wchar_t g_mac[128];
static int g_noLog = 0;
static ULONGLONG g_lastSpawn = 0;

// ---------------- Rm* 辅助函数（Rainmeter.dll 导出） ----------------
typedef LPCWSTR(__stdcall *RmReadString_t)(void *rm, LPCWSTR option, LPCWSTR defValue, BOOL replaceMeasures);

static RmReadString_t pRmReadString = NULL;

static void resolve_rm_api(void)
{
    if (pRmReadString) return;
    HMODULE rmdll = GetModuleHandleW(L"Rainmeter.dll");
    if (!rmdll) rmdll = GetModuleHandleW(NULL);
    if (rmdll) pRmReadString = (RmReadString_t)GetProcAddress(rmdll, "RmReadString");
}

static LPCWSTR rm_read(void *rm, const wchar_t *key, const wchar_t *def)
{
    resolve_rm_api();
    if (pRmReadString && rm) return pRmReadString(rm, key, def, TRUE);
    return def;
}

// ---------------- 日志 ----------------
static void native_log(const wchar_t *msg)
{
    FILE *f = NULL;
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%sPluginDormTemp.native.log", g_dir);
    _wfopen_s(&f, path, L"a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

static void logf(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 512, fmt, ap);
    va_end(ap);
    native_log(buf);
}

static void get_own_dir(void)
{
    HMODULE hm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&g_map, &hm);
    DWORD n = GetModuleFileNameW(hm, g_dir, MAX_PATH);
    while (n > 0 && g_dir[n - 1] != L'\\') n--;
    g_dir[n] = 0;
}

// ---------------- 共享内存读取 ----------------
static void read_snapshot(Snapshot *s)
{
    s->valid = 0;
    if (!g_map) return;
    const unsigned char *m = (const unsigned char *)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, MAP_SIZE);
    if (!m) return;
    uint32_t magic, ver;
    memcpy(&magic, m + 0, 4);
    memcpy(&ver, m + 4, 4);
    if (magic == MAGIC && ver == 1) {
        memcpy(&s->temp, m + OFF_TEMP, 8);
        memcpy(&s->hum, m + OFF_HUM, 8);
        memcpy(&s->volt, m + OFF_VOLT, 8);
        memcpy(&s->lastSampleFT, m + OFF_LAST, 8);
        memcpy(&s->status, m + OFF_STATUS, 4);
        memcpy(&s->ever, m + OFF_EVER, 4);
        memcpy(&s->heartbeatFT, m + OFF_BEAT, 8);
        memcpy(s->mac, m + OFF_MAC, 64);
        s->mac[31] = 0;
        s->valid = 1;
    }
    UnmapViewOfFile(m);
}

static void ensure_service(void)
{
    EnterCriticalSection(&g_lock);
    if (!g_map) g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, MAP_NAME);

    Snapshot s; read_snapshot(&s);
    int alive = 0;
    if (s.valid && s.heartbeatFT) {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULONGLONG nowFT = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        if (nowFT > s.heartbeatFT && (nowFT - s.heartbeatFT) < 150000000ULL) alive = 1; // 15s 心跳
    }
    ULONGLONG now = GetTickCount64();
    int canSpawn = (now - g_lastSpawn >= 15000);
    if (!alive && canSpawn) g_lastSpawn = now;
    LeaveCriticalSection(&g_lock);

    if (alive) return;
    if (!canSpawn) return;

    wchar_t svc[MAX_PATH * 2];
    _snwprintf(svc, MAX_PATH * 2, L"%sDormTempService.exe", g_dir);
    if (GetFileAttributesW(svc) == INVALID_FILE_ATTRIBUTES) {
        native_log(L"service exe not found");
        return;
    }
    wchar_t cmd[2048];
    _snwprintf(cmd, 2048, L"\"%s\" --logpath \"%s\" --name \"%s\"%s%s%s",
               svc, g_logDir, g_devName[0] ? g_devName : L"MJWSD06MMC",
               g_mac[0] ? L" --mac \"" : L"", g_mac, g_mac[0] ? L"\"" : L"");
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        native_log(L"service spawned");
    } else {
        logf(L"CreateProcess failed err=%lu", GetLastError());
    }
}

// ---------------- 数值换算 ----------------
static double seconds_since(ULONGLONG ft)
{
    if (ft == 0) return -1;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG nowFT = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (double)(nowFT - ft) / 10000000.0;
}

static const wchar_t *status_text(const Snapshot *s)
{
    if (!s->valid) return L"搜索中";
    double beatAge = seconds_since(s->heartbeatFT);
    if (beatAge < 0 || beatAge > 15) return s->ever ? L"离线" : L"搜索中";
    if (s->status == 2) {
        double age = seconds_since(s->lastSampleFT);
        if (age >= 0 && age < 120) return L"在线";
        return L"数据超时";
    }
    if (s->status == 1) return s->ever ? L"重连中" : L"连接中";
    return s->ever ? L"离线" : L"搜索中";
}

// ---------------- 导出（Plugin API v3） ----------------
__declspec(dllexport) void __cdecl Initialize(void **data, void *rm)
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        get_own_dir();
        g_lockInit = 1;
    }
    Measure *m = (Measure *)calloc(1, sizeof(Measure));
    if (!m) { *data = NULL; return; }
    m->rm = rm;
    wcsncpy_s(m->type, 32, rm_read(rm, L"Type", L"Temperature"), _TRUNCATE);
    *data = m;

    EnterCriticalSection(&g_lock);
    if (g_listCount == g_listCap) {
        g_listCap = g_listCap ? g_listCap * 2 : 8;
        g_list = (Measure **)realloc(g_list, g_listCap * sizeof(Measure *));
    }
    if (g_list) g_list[g_listCount++] = m;
    LeaveCriticalSection(&g_lock);

    // 采集配置：只采纳非空值（避免后续 measure 的空值覆盖）
    {
        LPCWSTR v = rm_read(rm, L"LogPath", L"");
        if (v && v[0]) wcsncpy_s(g_logDir, 512, v, _TRUNCATE);
        v = rm_read(rm, L"DeviceName", L"");
        if (v && v[0]) wcsncpy_s(g_devName, 128, v, _TRUNCATE);
        v = rm_read(rm, L"MacAddress", L"");
        if (v && v[0]) wcsncpy_s(g_mac, 128, v, _TRUNCATE);
    }

    logf(L"init type=%s logdir=%s", m->type, g_logDir);
    ensure_service();
}

__declspec(dllexport) void __cdecl Reload(void *data, void *rm, double *maxValue)
{
    Measure *m = (Measure *)data;
    if (m && rm) {
        m->rm = rm;
        wcsncpy_s(m->type, 32, rm_read(rm, L"Type", L"Temperature"), _TRUNCATE);
        LPCWSTR v = rm_read(rm, L"LogPath", L"");
        if (v && v[0]) wcsncpy_s(g_logDir, 512, v, _TRUNCATE);
        v = rm_read(rm, L"DeviceName", L"");
        if (v && v[0]) wcsncpy_s(g_devName, 128, v, _TRUNCATE);
        v = rm_read(rm, L"MacAddress", L"");
        if (v && v[0]) wcsncpy_s(g_mac, 128, v, _TRUNCATE);
    }
    if (maxValue) *maxValue = 100.0;
    if (g_lockInit) ensure_service();
}

__declspec(dllexport) double __cdecl Update(void *data)
{
    Measure *m = (Measure *)data;
    if (!m || !g_lockInit) return 0;
    Snapshot s;
    EnterCriticalSection(&g_lock);
    read_snapshot(&s);
    LeaveCriticalSection(&g_lock);

    // 心跳失效时低频重拉服务（每 10 秒最多一次）
    {
        static ULONGLONG lastCheck = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastCheck >= 10000) {
            lastCheck = now;
            double beatAge = s.valid ? seconds_since(s.heartbeatFT) : 999;
            if (beatAge < 0 || beatAge > 15) ensure_service();
        }
    }

    if (_wcsicmp(m->type, L"Temperature") == 0) return (s.valid && s.temp > -900) ? s.temp : 0;
    if (_wcsicmp(m->type, L"Humidity") == 0) return (s.valid && s.hum > -900) ? s.hum : 0;
    if (_wcsicmp(m->type, L"Voltage") == 0) return (s.valid && s.volt > -900) ? s.volt : 0;
    if (_wcsicmp(m->type, L"SampleAge") == 0) return seconds_since(s.valid ? s.lastSampleFT : 0);
    if (_wcsicmp(m->type, L"Online") == 0) return wcscmp(status_text(&s), L"在线") == 0 ? 1 : 0;
    return 0;
}

__declspec(dllexport) LPCWSTR __cdecl GetString(void *data)
{
    Measure *m = (Measure *)data;
    if (!m) return L"";
    wchar_t *b = m->strbuf;

    Snapshot s;
    ZeroMemory(&s, sizeof(s));
    if (g_lockInit) {
        EnterCriticalSection(&g_lock);
        read_snapshot(&s);
        LeaveCriticalSection(&g_lock);
    }

    if (_wcsicmp(m->type, L"Temperature") == 0) {
        if (s.valid && s.temp > -900) _snwprintf(b, 192, L"%.1f", s.temp);
        else wcscpy_s(b, 192, L"--");
    } else if (_wcsicmp(m->type, L"Humidity") == 0) {
        if (s.valid && s.hum > -900) _snwprintf(b, 192, L"%.0f", s.hum);
        else wcscpy_s(b, 192, L"--");
    } else if (_wcsicmp(m->type, L"Voltage") == 0) {
        if (s.valid && s.volt > -900) _snwprintf(b, 192, L"%.2f", s.volt);
        else wcscpy_s(b, 192, L"--");
    } else if (_wcsicmp(m->type, L"LastUpdate") == 0) {
        if (s.valid && s.lastSampleFT) {
            FILETIME ft; memcpy(&ft, &s.lastSampleFT, 8);
            FILETIME local; FileTimeToLocalFileTime(&ft, &local);
            SYSTEMTIME st; FileTimeToSystemTime(&local, &st);
            _snwprintf(b, 192, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        } else {
            wcscpy_s(b, 192, L"--:--");
        }
    } else if (_wcsicmp(m->type, L"Status") == 0) {
        wcscpy_s(b, 192, status_text(&s));
    } else if (_wcsicmp(m->type, L"Battery") == 0) {
        if (!s.valid || s.volt <= -900) wcscpy_s(b, 192, L"--");
        else if (s.volt >= 3.0) wcscpy_s(b, 192, L"电量充足");
        else if (s.volt >= 2.7) wcscpy_s(b, 192, L"电量偏低");
        else wcscpy_s(b, 192, L"需更换电池");
    } else if (_wcsicmp(m->type, L"Comfort") == 0) {
        if (!s.valid || s.temp <= -900 || s.hum <= -900) wcscpy_s(b, 192, L"--");
        else if (s.temp < 18) wcscpy_s(b, 192, L"偏冷");
        else if (s.temp > 28) wcscpy_s(b, 192, L"偏热");
        else if (s.hum < 40) wcscpy_s(b, 192, L"干燥");
        else if (s.hum > 70) wcscpy_s(b, 192, L"潮湿");
        else wcscpy_s(b, 192, L"舒适");
    } else if (_wcsicmp(m->type, L"Mac") == 0) {
        wcscpy_s(b, 192, (s.valid && s.mac[0]) ? s.mac : L"");
    } else {
        b[0] = 0;
    }
    return b;
}

__declspec(dllexport) void __cdecl Finalize(void *data)
{
    Measure *m = (Measure *)data;
    if (!m) return;
    if (g_lockInit) {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_listCount; i++) {
            if (g_list[i] == m) { g_list[i] = g_list[--g_listCount]; break; }
        }
        LeaveCriticalSection(&g_lock);
    }
    free(m);
}

__declspec(dllexport) void __cdecl ExecuteBang(LPCWSTR args, void *data)
{
    if (args && _wcsicmp(args, L"Reconnect") == 0) {
        native_log(L"bang: restart service");
        STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        wchar_t cmd[] = L"taskkill /F /IM DormTempService.exe";
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        EnterCriticalSection(&g_lock);
        if (g_map) { CloseHandle(g_map); g_map = NULL; }
        g_lastSpawn = 0;
        LeaveCriticalSection(&g_lock);
        ensure_service();
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        get_own_dir();
    }
    return TRUE;
}
