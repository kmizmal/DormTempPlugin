// ============================================================================
//  Plugin.cpp —— Rainmeter Plugin API v3 导出层
//
//  · Update/GetString 只读共享快照，不做任何采集，蓝牙往返永远不占主线程
//  · 采集线程由 ble::start() 拉起，Initialize / Reload 重复调用也只起一个
//  · 曲线类 measure 只拿到一个 PNG 路径，画图在 chart 自己的线程里跑
// ============================================================================
#include "BleSession.h"
#include "Chart.h"
#include "Config.h"
#include "Log.h"
#include "RainmeterApi.h"
#include "SharedState.h"
#include "Stats.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

struct Measure {
    wchar_t     type[32];
    wchar_t     text[512];
    wchar_t     key[40];
    int         window;
    int         chartId;
    bool        owner;          // Type=Chart 的那个，负责注册/注销渲染
    chart::Spec spec;
    void       *rm;
};

// 汇总类 Type：数值给 Update，文本给 GetString，两者共用一张表
enum class Field {
    None = 0,
    TempAvg, TempMax, TempMin, TempMaxAt, TempMinAt,
    HumAvg, HumMax, HumMin,
    Samples, Summary,
};

// 曲线类 Type：都指向同一个 key 的渲染结果，各取一项
enum class ChartField {
    None = 0,
    Path, Hi, Mid, Lo,
    Min, Max, MinAt, MaxAt,
    Span, Series,
};

Field field_of(const wchar_t *t)
{
    if (_wcsicmp(t, L"TempAvg") == 0) return Field::TempAvg;
    if (_wcsicmp(t, L"TempMax") == 0) return Field::TempMax;
    if (_wcsicmp(t, L"TempMin") == 0) return Field::TempMin;
    if (_wcsicmp(t, L"TempMaxAt") == 0) return Field::TempMaxAt;
    if (_wcsicmp(t, L"TempMinAt") == 0) return Field::TempMinAt;
    if (_wcsicmp(t, L"HumAvg") == 0) return Field::HumAvg;
    if (_wcsicmp(t, L"HumMax") == 0) return Field::HumMax;
    if (_wcsicmp(t, L"HumMin") == 0) return Field::HumMin;
    if (_wcsicmp(t, L"Samples") == 0) return Field::Samples;
    if (_wcsicmp(t, L"Summary") == 0) return Field::Summary;
    return Field::None;
}

ChartField chart_field_of(const wchar_t *t)
{
    if (_wcsicmp(t, L"Chart") == 0) return ChartField::Path;
    if (_wcsicmp(t, L"ChartHi") == 0) return ChartField::Hi;
    if (_wcsicmp(t, L"ChartMid") == 0) return ChartField::Mid;
    if (_wcsicmp(t, L"ChartLo") == 0) return ChartField::Lo;
    if (_wcsicmp(t, L"ChartMin") == 0) return ChartField::Min;
    if (_wcsicmp(t, L"ChartMax") == 0) return ChartField::Max;
    if (_wcsicmp(t, L"ChartMinAt") == 0) return ChartField::MinAt;
    if (_wcsicmp(t, L"ChartMaxAt") == 0) return ChartField::MaxAt;
    if (_wcsicmp(t, L"ChartSpan") == 0) return ChartField::Span;
    if (_wcsicmp(t, L"ChartSeries") == 0) return ChartField::Series;
    return ChartField::None;
}

int clamp_int(const wchar_t *text, int lo, int hi, int def)
{
    if (!text || !*text) return def;
    const int v = _wtoi(text);
    if (v == 0) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

unsigned clamp_byte(int v) { return static_cast<unsigned>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

chart::Spec read_spec(void *rm)
{
    chart::Spec s;

    // Window= 优先（Today / 24h / 7d），留空则用 Hours=
    const LPCWSTR w = rmapi::read(rm, L"Window", L"");
    s.window = (w && *w) ? stats::window_from_text(w) : -1;

    s.hours   = clamp_int(rmapi::read(rm, L"Hours", L"24"), 1, 24 * 90, 24);
    s.buckets = clamp_int(rmapi::read(rm, L"Buckets", L"96"), 8, 512, 96);
    s.width   = clamp_int(rmapi::read(rm, L"Width", L"180"), 40, 1200, 180);
    s.height  = clamp_int(rmapi::read(rm, L"Height", L"92"), 30, 600, 92);

    const LPCWSTR ser = rmapi::read(rm, L"Series", L"Temp");
    s.series = (ser && (_wcsicmp(ser, L"Hum") == 0 || _wcsicmp(ser, L"Humidity") == 0))
                   ? chart::kHum : chart::kTemp;

    int r = 120, g = 196, b = 255;
    const LPCWSTR col = rmapi::read(rm, L"Color", L"120,196,255");
    if (col && *col) swscanf(col, L"%d,%d,%d", &r, &g, &b);
    s.rgb = (clamp_byte(r) << 16) | (clamp_byte(g) << 8) | clamp_byte(b);
    return s;
}

int chart_of(Measure *m)
{
    if (m->chartId == 0 && m->key[0]) m->chartId = chart::find(m->key);
    return m->chartId;
}

void setup_measure(Measure *m, void *rm)
{
    const LPCWSTR type = rmapi::read(rm, L"Type", L"Temperature");
    wcsncpy(m->type, type ? type : L"Temperature", 31);
    m->type[31] = 0;

    m->window = stats::window_from_text(rmapi::read(rm, L"Window", L"Today"));

    const LPCWSTR key = rmapi::read(rm, L"Chart", L"main");
    wcsncpy(m->key, (key && *key) ? key : L"main", 39);
    m->key[39] = 0;

    const ChartField cf = chart_field_of(m->type);
    if (cf == ChartField::Path) {
        m->spec = read_spec(rm);
        m->owner = true;
        m->chartId = chart::open(m->key, m->spec);
    } else if (cf != ChartField::None) {
        m->spec = read_spec(rm);
        m->owner = false;
        m->chartId = chart::find(m->key);
    }
}

void apply_config(void *rm)
{
    LPCWSTR v = rmapi::read(rm, L"LogPath", L"");
    if (v && *v) config::set_log_dir(config::expand_env(v));

    v = rmapi::read(rm, L"DeviceName", L"");
    if (v && *v) config::set_device_name(v);

    // MacAddress 允许为空（清空 = 回到自动扫描）
    v = rmapi::read(rm, L"MacAddress", L"");
    if (v) config::set_mac_opt(v);
}

bool is_number(double v) { return std::isfinite(v) && v > -900.0; }

// 温湿度小数位不同：温度 1 位、湿度 0 位
std::wstring decimal_text(double v, int series)
{
    return number_text(v, series == chart::kHum ? 0 : 1);
}

} // namespace

extern "C" {

__declspec(dllexport) void __cdecl Initialize(void **data, void *rm)
{
    try {
        Measure *m = static_cast<Measure *>(calloc(1, sizeof(Measure)));
        if (!m) {
            if (data) *data = nullptr;
            return;
        }
        m->rm = rm;
        apply_config(rm);
        setup_measure(m, rm);
        if (data) *data = m;

        state::measure_add();
        state::touch();

        config::ensure_log_dir();

        logx::writef(L"init type=%s", m->type);
        ble::start();
        chart::start();
    } catch (...) {
    }
}

__declspec(dllexport) void __cdecl Reload(void *data, void *rm, double *maxValue)
{
    try {
        Measure *m = static_cast<Measure *>(data);
        if (m && rm) {
            m->rm = rm;
            apply_config(rm);
            setup_measure(m, rm);
        }
        if (maxValue) *maxValue = 100.0;

        state::touch();
        ble::start();
        chart::start();
    } catch (...) {
        if (maxValue) *maxValue = 100.0;
    }
}

__declspec(dllexport) double __cdecl Update(void *data)
{
    Measure *m = static_cast<Measure *>(data);
    if (!m) return 0;

    // 曲线类：只读缓存里的边界值
    const ChartField cf = chart_field_of(m->type);
    if (cf != ChartField::None) {
        const chart::View v = chart::view(chart_of(m));
        if (!v.valid) return 0;

        const bool hum = m->spec.series == chart::kHum;
        switch (cf) {
        case ChartField::Path: return 1;
        case ChartField::Hi:   return v.hi;
        case ChartField::Mid:  return v.mid;
        case ChartField::Lo:   return v.lo;
        case ChartField::Min:  return hum ? v.hMin : v.tMin;
        case ChartField::Max:  return hum ? v.hMax : v.tMax;
        default:               return 0;   // 文本类没有数值意义
        }
    }

    // 汇总类只读缓存，不碰 SQLite（缓存由采集线程刷新）
    const Field f = field_of(m->type);
    if (f != Field::None) {
        const stats::Summary s = stats::get(m->window);
        switch (f) {
        case Field::TempAvg: return s.valid ? s.tAvg : 0;
        case Field::TempMax: return s.valid ? s.tMax : 0;
        case Field::TempMin: return s.valid ? s.tMin : 0;
        case Field::HumAvg:  return s.valid ? s.hAvg : 0;
        case Field::HumMax:  return s.valid ? s.hMax : 0;
        case Field::HumMin:  return s.valid ? s.hMin : 0;
        case Field::Samples: return s.n;
        default:             return 0;     // 文本类没有数值意义
        }
    }

    const Snapshot s = state::get();

    if (_wcsicmp(m->type, L"Temperature") == 0) return is_number(s.temp) ? s.temp : 0;
    if (_wcsicmp(m->type, L"Humidity")    == 0) return is_number(s.hum)  ? s.hum  : 0;
    if (_wcsicmp(m->type, L"Voltage")     == 0) return is_number(s.volt) ? s.volt : 0;
    if (_wcsicmp(m->type, L"SampleAge")   == 0) return seconds_since(s.lastSampleFT);
    if (_wcsicmp(m->type, L"Online")      == 0) {
        return status_text(s.status, s.ever, seconds_since(s.lastSampleFT)) == L"在线" ? 1 : 0;
    }
    return 0;
}

__declspec(dllexport) LPCWSTR __cdecl GetString(void *data)
{
    Measure *m = static_cast<Measure *>(data);
    if (!m) return L"";

    std::wstring out;

    const ChartField cf = chart_field_of(m->type);
    if (cf != ChartField::None) {
        const int id = chart_of(m);
        const chart::View v = chart::view(id);
        const int series = m->spec.series;

        switch (cf) {
        case ChartField::Path:   out = v.valid ? v.path : L""; break;
        case ChartField::Hi:     out = v.valid ? decimal_text(v.hi, series) : L"--"; break;
        case ChartField::Mid:    out = v.valid ? decimal_text(v.mid, series) : L"--"; break;
        case ChartField::Lo:     out = v.valid ? decimal_text(v.lo, series) : L"--"; break;
        case ChartField::Min:    out = v.valid ? decimal_text(series == chart::kHum ? v.hMin : v.tMin, series) : L"--"; break;
        case ChartField::Max:    out = v.valid ? decimal_text(series == chart::kHum ? v.hMax : v.tMax, series) : L"--"; break;
        case ChartField::MinAt:  out = stats::clock_hhmm(series == chart::kHum ? v.hMinTs : v.tMinTs); break;
        case ChartField::MaxAt:  out = stats::clock_hhmm(series == chart::kHum ? v.hMaxTs : v.tMaxTs); break;
        case ChartField::Span:   out = chart::span_text(id); break;
        case ChartField::Series: out = chart::series_text(id); break;
        default: break;
        }

        wcsncpy(m->text, out.c_str(), 511);
        m->text[511] = 0;
        return m->text;
    }

    const Field f = field_of(m->type);
    if (f != Field::None) {
        const stats::Summary st = stats::get(m->window);
        switch (f) {
        case Field::TempAvg: out = number_text(st.valid ? st.tAvg : NAN, 1); break;
        case Field::TempMax: out = number_text(st.valid ? st.tMax : NAN, 1); break;
        case Field::TempMin: out = number_text(st.valid ? st.tMin : NAN, 1); break;
        case Field::HumAvg:  out = number_text(st.valid ? st.hAvg : NAN, 0); break;
        case Field::HumMax:  out = number_text(st.valid ? st.hMax : NAN, 0); break;
        case Field::HumMin:  out = number_text(st.valid ? st.hMin : NAN, 0); break;
        case Field::Samples: out = std::to_wstring(st.n); break;
        case Field::TempMaxAt: out = stats::clock_hhmm(st.valid ? st.tMaxTs : 0); break;
        case Field::TempMinAt: out = stats::clock_hhmm(st.valid ? st.tMinTs : 0); break;
        case Field::Summary: out = stats::summary_text(st); break;
        default: break;
        }

        wcsncpy(m->text, out.c_str(), 511);
        m->text[511] = 0;
        return m->text;
    }

    const Snapshot s = state::get();
    const double age = seconds_since(s.lastSampleFT);

    if (_wcsicmp(m->type, L"Temperature") == 0) {
        out = number_text(s.temp, 1);
    } else if (_wcsicmp(m->type, L"Humidity") == 0) {
        out = number_text(s.hum, 0);
    } else if (_wcsicmp(m->type, L"Voltage") == 0) {
        out = number_text(s.volt, 2);
    } else if (_wcsicmp(m->type, L"LastUpdate") == 0) {
        out = clock_text(s.lastSampleFT);
    } else if (_wcsicmp(m->type, L"Status") == 0) {
        out = status_text(s.status, s.ever, age);
    } else if (_wcsicmp(m->type, L"Battery") == 0) {
        out = battery_text(s.volt);
    } else if (_wcsicmp(m->type, L"Comfort") == 0) {
        out = comfort_text(s.temp, s.hum);
    } else if (_wcsicmp(m->type, L"Mac") == 0) {
        out = s.mac;
    }

    wcsncpy(m->text, out.c_str(), 511);
    m->text[511] = 0;
    return m->text;
}

__declspec(dllexport) void __cdecl ExecuteBang(LPCWSTR args, void *data)
{
    (void)data;
    try {
        if (args && _wcsicmp(args, L"Reconnect") == 0) {
            state::touch();
            ble::request_reconnect();
        }
    } catch (...) {
    }
}

__declspec(dllexport) void __cdecl Finalize(void *data)
{
    try {
        Measure *m = static_cast<Measure *>(data);
        if (!m) return;
        if (m->owner && m->chartId) chart::close(m->chartId);
        state::measure_remove();
        state::touch();
        free(m);
    } catch (...) {
    }
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        ble::stop();
        chart::stop();
        // 尽量让采集线程收尾，别在代码段卸载后再回调进来
        for (int i = 0; i < 60 && ble::running(); ++i) Sleep(50);
        (void)reserved;
    }
    return TRUE;
}
