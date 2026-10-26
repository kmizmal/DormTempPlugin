// ============================================================================
//  Chart.cpp
//  · GDI+ 全部靠 GetProcAddress 取（gdiplus.dll 系统必有），不加链接依赖
//  · 图片本身透明：卡片底色、字号都归皮肤管，插件只负责把数据画成曲线
//  · 每次重画都写新文件名（chart_x_7.png）：皮肤那边靠 DynamicVariables 重读，
//    但「宿主正在读、这里正在写」撞上可能读到半个 PNG，换个名字就没这问题
// ============================================================================
#include "Chart.h"

#include "Config.h"
#include "Stats.h"
#include "Store.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const unsigned long long kRefreshMs = 40000;   // 无变化也重画一次（窗口在滑动）
const int kMaxBuckets = 512;
const int kArgb32 = 0x26200A;                  // PixelFormat32bppARGB

struct GpPoint {
    int X;
    int Y;
};

struct Guid {
    unsigned long  d1;
    unsigned short d2;
    unsigned short d3;
    unsigned char  d4[8];
};

const Guid kPngEncoder = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

// ---------------------------------------------------------------- GDI+ 绑定
struct Gdi {
    HMODULE lib = nullptr;
    int  (*startup)(ULONG_PTR *, const void *, void *) = nullptr;
    int  (*create_bitmap)(int, int, int, int, unsigned char *, void **) = nullptr;
    int  (*get_graphics)(void *, void **) = nullptr;
    int  (*set_smoothing)(void *, int) = nullptr;
    int  (*set_pixel_offset)(void *, int) = nullptr;
    int  (*clear)(void *, unsigned long) = nullptr;
    int  (*create_fill)(unsigned long, void **) = nullptr;
    int  (*create_pen)(unsigned long, float, int, void **) = nullptr;
    int  (*draw_line)(void *, void *, int, int, int, int) = nullptr;
    int  (*draw_lines)(void *, void *, const GpPoint *, int) = nullptr;
    int  (*fill_polygon)(void *, void *, const GpPoint *, int, int) = nullptr;
    int  (*fill_ellipse)(void *, void *, int, int, int, int) = nullptr;
    int  (*save_file)(void *, const wchar_t *, const Guid *, void *) = nullptr;
    int  (*delete_pen)(void *) = nullptr;
    int  (*delete_brush)(void *) = nullptr;
    int  (*delete_graphics)(void *) = nullptr;
    int  (*dispose_image)(void *) = nullptr;
};

Gdi g_gdi;
ULONG_PTR g_token = 0;
bool g_gdiOk = false;
std::once_flag g_gdiOnce;

void load_gdi()
{
    HMODULE h = LoadLibraryW(L"gdiplus.dll");
    if (!h) return;

    auto get = [h](const char *name) { return reinterpret_cast<void *>(GetProcAddress(h, name)); };

    g_gdi.lib              = h;
    g_gdi.startup          = reinterpret_cast<decltype(g_gdi.startup)>(get("GdiplusStartup"));
    g_gdi.create_bitmap    = reinterpret_cast<decltype(g_gdi.create_bitmap)>(get("GdipCreateBitmapFromScan0"));
    g_gdi.get_graphics     = reinterpret_cast<decltype(g_gdi.get_graphics)>(get("GdipGetImageGraphicsContext"));
    g_gdi.set_smoothing    = reinterpret_cast<decltype(g_gdi.set_smoothing)>(get("GdipSetSmoothingMode"));
    g_gdi.set_pixel_offset = reinterpret_cast<decltype(g_gdi.set_pixel_offset)>(get("GdipSetPixelOffsetMode"));
    g_gdi.clear            = reinterpret_cast<decltype(g_gdi.clear)>(get("GdipGraphicsClear"));
    g_gdi.create_fill      = reinterpret_cast<decltype(g_gdi.create_fill)>(get("GdipCreateSolidFill"));
    g_gdi.create_pen       = reinterpret_cast<decltype(g_gdi.create_pen)>(get("GdipCreatePen1"));
    g_gdi.draw_line        = reinterpret_cast<decltype(g_gdi.draw_line)>(get("GdipDrawLineI"));
    g_gdi.draw_lines       = reinterpret_cast<decltype(g_gdi.draw_lines)>(get("GdipDrawLinesI"));
    g_gdi.fill_polygon     = reinterpret_cast<decltype(g_gdi.fill_polygon)>(get("GdipFillPolygonI"));
    g_gdi.fill_ellipse     = reinterpret_cast<decltype(g_gdi.fill_ellipse)>(get("GdipFillEllipseI"));
    g_gdi.save_file        = reinterpret_cast<decltype(g_gdi.save_file)>(get("GdipSaveImageToFile"));
    g_gdi.delete_pen       = reinterpret_cast<decltype(g_gdi.delete_pen)>(get("GdipDeletePen"));
    g_gdi.delete_brush     = reinterpret_cast<decltype(g_gdi.delete_brush)>(get("GdipDeleteBrush"));
    g_gdi.delete_graphics  = reinterpret_cast<decltype(g_gdi.delete_graphics)>(get("GdipDeleteGraphics"));
    g_gdi.dispose_image    = reinterpret_cast<decltype(g_gdi.dispose_image)>(get("GdipDisposeImage"));

    if (!g_gdi.startup || !g_gdi.create_bitmap || !g_gdi.get_graphics || !g_gdi.create_pen ||
        !g_gdi.draw_lines || !g_gdi.fill_polygon || !g_gdi.save_file || !g_gdi.dispose_image) {
        FreeLibrary(h);
        g_gdi = Gdi();
        return;
    }

    // GdiplusStartupInput（x64：4 + 4 pad + 8 + 4 + 4 = 24 字节，与原生一致）
    struct {
        UINT32 version;
        void  *callback;
        BOOL   no_bg_thread;
        BOOL   no_codecs;
    } input = {1, nullptr, FALSE, FALSE};

    g_gdiOk = g_gdi.startup(&g_token, &input, nullptr) == 0;
}

// ---------------------------------------------------------------- 注册表
struct Entry {
    chart::Spec        spec;
    chart::View        view;
    std::wstring       stem;
    int                rev = 0;
    unsigned long long renderedAt = 0;
    long long          renderedData = 0;
    bool               dirty = true;
};

std::mutex g_mtx;
std::map<int, Entry> g_entries;
std::map<std::wstring, int> g_keys;
int g_next = 1;

HANDLE g_wake = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};

bool same_spec(const chart::Spec &a, const chart::Spec &b)
{
    return a.window == b.window && a.hours == b.hours && a.buckets == b.buckets &&
           a.width == b.width && a.height == b.height && a.series == b.series &&
           a.dual == b.dual && a.rgb == b.rgb && a.rgb2 == b.rgb2;
}

std::wstring sanitize(const wchar_t *key)
{
    std::wstring out;
    for (const wchar_t *p = (key && *key) ? key : L"chart"; *p && out.size() < 24; ++p) {
        const wchar_t c = *p;
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
        out.push_back(ok ? c : L'_');
    }
    return out.empty() ? std::wstring(L"chart") : out;
}

std::wstring chart_dir()
{
    std::wstring dir = config::dir();
    if (dir.empty()) return std::wstring();
    return dir + L"\\chart";
}

// 只留最新两张：Rainmeter 可能还持有上一张的句柄，别在同一拍里删它
void cleanup_files(const std::wstring &dir, const std::wstring &stem, int keepRev, int keepPrev)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return;

    const std::wstring prefix = stem + L"_";
    for (const auto &entry : it) {
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) != 0) continue;

        std::wstring num = name.substr(prefix.size());
        if (num.size() > 4) num.resize(num.size() - 4);   // 去掉 .png
        int rev = -1;
        try { rev = std::stoi(num); } catch (...) { continue; }
        if (rev == keepRev || rev == keepPrev) continue;
        std::filesystem::remove(entry.path(), ec);
    }
}

long long since_of(const chart::Spec &spec, long long now)
{
    if (spec.window >= 0 && spec.window < stats::kWindowCount)
        return stats::window_start(spec.window, now);

    const int hours = std::max(1, std::min(24 * 90, spec.hours));
    return now - static_cast<long long>(hours) * 3600;
}

// ---------------------------------------------------------------- 绘制
void draw_day_grid(void *gfx, void *pen, long long since, long long now, int W, int H)
{
    time_t anchor = static_cast<time_t>(since);
    struct tm *p = localtime(&anchor);
    if (!p) return;

    struct tm cur = *p;
    for (int guard = 0; guard < 400; ++guard) {
        cur.tm_mday += 1;
        cur.tm_hour = 0;
        cur.tm_min = 0;
        cur.tm_sec = 0;
        cur.tm_isdst = -1;

        const time_t m = mktime(&cur);   // mktime 会把 cur 归一化，可继续往后推
        if (m <= 0) return;

        const long long ts = static_cast<long long>(m);
        if (ts >= now) return;
        if (ts <= since) continue;

        const int x = static_cast<int>(std::lround(
            static_cast<double>(ts - since) / static_cast<double>(now - since) * (W - 1)));
        g_gdi.draw_line(gfx, pen, x, 0, x, H - 1);
    }
}

// 把一个序列按各自的 Y 轴折成像素点；空桶（n = 0）跳过，曲线在这里断开
void build_points(const std::vector<store::Slot> &slots, int series,
                  double lo, double hi, int W, int H, std::vector<GpPoint> *out)
{
    out->clear();
    if (!(hi - lo > 1e-6)) return;

    const int nb = static_cast<int>(slots.size());
    out->reserve(static_cast<size_t>(nb));
    for (int i = 0; i < nb; ++i) {
        if (slots[static_cast<size_t>(i)].n <= 0) continue;
        const double v = (series == chart::kHum) ? slots[static_cast<size_t>(i)].h
                                                 : slots[static_cast<size_t>(i)].t;
        const int x = (nb <= 1) ? (W - 1) / 2
                                : static_cast<int>(std::lround(static_cast<double>(i) * (W - 1) / (nb - 1)));
        const int y = static_cast<int>(std::lround((H - 1) - (v - lo) / (hi - lo) * (H - 1)));
        out->push_back({x, std::max(0, std::min(H - 1, y))});
    }
}

bool draw_png(const chart::Spec &spec, const std::vector<store::Slot> &slots,
              double lo, double hi, double lo2, double hi2, const std::wstring &path)
{
    if (!g_gdiOk || slots.empty()) return false;
    if (hi - lo < 1e-6) return false;

    const int W = std::max(40, std::min(1200, spec.width));
    const int H = std::max(30, std::min(600, spec.height));

    const long long now = static_cast<long long>(time(nullptr));
    const long long since = since_of(spec, now);

    void *bmp = nullptr;
    void *gfx = nullptr;
    if (g_gdi.create_bitmap(W, H, 0, kArgb32, nullptr, &bmp) != 0 || !bmp) return false;
    if (g_gdi.get_graphics(bmp, &gfx) != 0 || !gfx) {
        g_gdi.dispose_image(bmp);
        return false;
    }

    g_gdi.set_smoothing(gfx, 4);      // SmoothingModeAntiAlias
    g_gdi.set_pixel_offset(gfx, 4);   // PixelOffsetModeHalf
    g_gdi.clear(gfx, 0x00000000);     // 全透明

    const unsigned accent  = spec.rgb  & 0xFFFFFF;
    const unsigned accent2 = spec.rgb2 & 0xFFFFFF;
    const bool dual = spec.dual && (hi2 - lo2 > 1e-6);

    // 横向刻度线：0 / 25 / 50 / 75 / 100%
    void *penGrid = nullptr;
    if (g_gdi.create_pen(0x14FFFFFF, 1.0f, 2, &penGrid) == 0 && penGrid) {
        for (int i = 0; i <= 4; ++i) {
            const int y = (H - 1) - (H - 1) * i / 4;
            g_gdi.draw_line(gfx, penGrid, 0, y, W - 1, y);
        }
        // 跨天的竖线，多天窗口才画
        if (now - since > 86400) draw_day_grid(gfx, penGrid, since, now, W, H);
    }

    std::vector<GpPoint> pts;
    build_points(slots, spec.series, lo, hi, W, H, &pts);

    std::vector<GpPoint> pts2;
    if (dual) build_points(slots, chart::kHum, lo2, hi2, W, H, &pts2);

    if (pts.empty() && pts2.empty()) {
        if (penGrid) g_gdi.delete_pen(penGrid);
        g_gdi.delete_graphics(gfx);
        g_gdi.dispose_image(bmp);
        return false;
    }

    // 曲线下的渐变没法做（要 PathGradient），用低透明纯色填充，够用且便宜。
    // 双序列不填：两条线的图里面积会把另一条闷住，数据还没铺满窗口时边缘也显得生硬
    if (!dual && pts.size() >= 2) {
        std::vector<GpPoint> poly;
        poly.reserve(pts.size() + 2);
        poly.push_back({pts.front().X, H - 1});
        poly.insert(poly.end(), pts.begin(), pts.end());
        poly.push_back({pts.back().X, H - 1});

        void *brushFill = nullptr;
        if (g_gdi.create_fill(0x1E000000u | accent, &brushFill) == 0 && brushFill) {
            g_gdi.fill_polygon(gfx, brushFill, poly.data(), static_cast<int>(poly.size()), 1);
            g_gdi.delete_brush(brushFill);
        }
    }

    // 湿度先画（当参考线，细一点），温度压在上层
    auto stroke = [&](const std::vector<GpPoint> &p, unsigned color, float w) {
        if (p.size() < 2) return;
        void *pen = nullptr;
        if (g_gdi.create_pen(0xFF000000 | color, w, 2, &pen) == 0 && pen) {
            g_gdi.draw_lines(gfx, pen, p.data(), static_cast<int>(p.size()));
            g_gdi.delete_pen(pen);
        }
    };
    stroke(pts2, accent2, 1.5f);
    stroke(pts, accent, 2.0f);

    // 末端一个小实心点，示意「最新一刻」；双序列时用本线颜色，免得两条线分不清
    auto dot = [&](const GpPoint &p, unsigned color) {
        void *brushDot = nullptr;
        if (g_gdi.create_fill(0xFF000000 | color, &brushDot) == 0 && brushDot) {
            g_gdi.fill_ellipse(gfx, brushDot, p.X - 2, p.Y - 2, 5, 5);
            g_gdi.delete_brush(brushDot);
        }
    };
    if (!pts2.empty()) dot(pts2.back(), accent2);
    if (!pts.empty())  dot(pts.back(), dual ? accent : 0xFFFFFFu);

    if (penGrid) g_gdi.delete_pen(penGrid);
    g_gdi.delete_graphics(gfx);

    const bool ok = g_gdi.save_file(bmp, path.c_str(), &kPngEncoder, nullptr) == 0;
    g_gdi.dispose_image(bmp);
    return ok;
}

bool render_one(const chart::Spec &spec, const std::wstring &dir, const std::wstring &stem,
                int rev, chart::View *out)
{
    if (dir.empty()) return false;
    store::init(config::dir());   // 蓝牙线程可能还在待机，曲线线程自己保证库是开的

    const long long now = static_cast<long long>(time(nullptr));
    const long long since = since_of(spec, now);
    const int nb = std::max(8, std::min(kMaxBuckets, spec.buckets));

    std::vector<store::Slot> slots(static_cast<size_t>(nb));
    if (!store::series(since, now + 1, nb, slots.data())) return false;

    int n = 0;
    double tmn = 1e18, tmx = -1e18;
    double hmn = 1e18, hmx = -1e18;
    int hmnIdx = -1, hmxIdx = -1;
    for (int i = 0; i < nb; ++i) {
        const auto &s = slots[static_cast<size_t>(i)];
        if (s.n <= 0) continue;
        n += s.n;
        tmn = std::min(tmn, s.t);
        tmx = std::max(tmx, s.t);
        if (s.h < hmn) { hmn = s.h; hmnIdx = i; }
        if (s.h > hmx) { hmx = s.h; hmxIdx = i; }
    }
    if (n <= 0) return false;

    // 双序列各算各的轴：温度走左边（°C），湿度走右边（%）
    double lo = 0, mid = 0, hi = 0;
    double lo2 = 0, mid2 = 0, hi2 = 0;
    if (spec.dual) {
        chart::axis_bounds(tmn, tmx, chart::kTemp, &lo, &hi, &mid);
        chart::axis_bounds(hmn, hmx, chart::kHum, &lo2, &hi2, &mid2);
    } else if (spec.series == chart::kHum) {
        chart::axis_bounds(hmn, hmx, chart::kHum, &lo, &hi, &mid);
    } else {
        chart::axis_bounds(tmn, tmx, chart::kTemp, &lo, &hi, &mid);
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    wchar_t name[96];
    swprintf(name, 96, L"%s\\%s_%d.png", dir.c_str(), stem.c_str(), rev % 1000);
    if (!draw_png(spec, slots, lo, hi, lo2, hi2, name)) return false;

    chart::View v;
    v.valid = true;
    v.path = name;
    v.lo = lo;
    v.mid = mid;
    v.hi = hi;
    v.lo2 = lo2;
    v.mid2 = mid2;
    v.hi2 = hi2;
    v.n = n;
    v.hMin = hmn;
    v.hMax = hmx;

    // 湿度极值时刻按桶心估算（±半个桶），不为此再扫一遍库
    const long long width = (now + 1 - since + nb - 1) / nb;
    if (hmnIdx >= 0) v.hMinTs = since + static_cast<long long>(hmnIdx) * width + width / 2;
    if (hmxIdx >= 0) v.hMaxTs = since + static_cast<long long>(hmxIdx) * width + width / 2;

    store::Agg agg;
    if (store::aggregate(since, &agg) && agg.n > 0) {
        v.tMin = agg.tMin; v.tMax = agg.tMax;
        v.hMin = agg.hMin; v.hMax = agg.hMax;
        v.tMinTs = agg.tMinTs; v.tMaxTs = agg.tMaxTs;
    }

    *out = v;
    return true;
}

// ---------------------------------------------------------------- 渲染线程
void loop()
{
    std::call_once(g_gdiOnce, load_gdi);

    long long dataTs = 0;
    unsigned long long dataChecked = 0;

    while (!g_stop.load()) {
        WaitForSingleObject(g_wake, 400);
        if (g_stop.load()) break;

        const unsigned long long tick = GetTickCount64();

        struct Job {
            int          id;
            chart::Spec  spec;
            std::wstring stem;
            int          rev;
        };
        std::vector<Job> jobs;
        std::wstring dir;

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_entries.empty()) continue;

            if (dataChecked == 0 || tick - dataChecked >= 2000) {
                dataTs = store::latest_ts();
                dataChecked = tick;
            }

            for (const auto &kv : g_entries) {
                const Entry &e = kv.second;
                const bool due = e.renderedAt == 0 || (tick >= e.renderedAt && tick - e.renderedAt >= kRefreshMs);
                const bool fresh = dataTs > e.renderedData;
                if (e.dirty || due || fresh)
                    jobs.push_back({kv.first, e.spec, e.stem, e.rev + 1});
            }
            dir = chart_dir();
        }

        for (const auto &job : jobs) {
            if (g_stop.load()) break;

            chart::View v;
            const bool ok = render_one(job.spec, dir, job.stem, job.rev, &v);

            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_entries.find(job.id);
            if (it == g_entries.end()) continue;

            Entry &e = it->second;
            e.dirty = false;
            e.renderedAt = tick;
            e.renderedData = dataTs;
            if (ok) {
                const int prev = e.rev;
                e.rev = job.rev;
                e.view = v;
                cleanup_files(dir, job.stem, e.rev % 1000, prev % 1000);
            }
        }
    }

    g_started = false;
}

} // namespace

namespace chart {

void axis_bounds(double mn, double mx, int series, double *lo, double *hi, double *mid)
{
    const bool hum = (series == kHum);
    const double minSpan = hum ? 10.0 : 2.0;
    const double unit    = hum ? 5.0  : 0.5;

    if (!std::isfinite(mn) || !std::isfinite(mx)) {
        mn = hum ? 40.0 : 20.0;
        mx = mn + minSpan;
    }
    if (mx < mn) std::swap(mn, mx);

    const double center = (mn + mx) / 2;
    const double span = std::max(mx - mn, minSpan);
    const double pad = span * 0.12;

    double l = std::floor((center - span / 2 - pad) / unit) * unit;
    double h = std::ceil((center + span / 2 + pad) / unit) * unit;
    if (h - l < minSpan) {
        const double c = (l + h) / 2;
        l = std::floor((c - minSpan / 2) / unit) * unit;
        h = l + minSpan;
    }

    if (lo) *lo = l;
    if (hi) *hi = h;
    if (mid) *mid = (l + h) / 2;
}

int open(const wchar_t *key, const Spec &spec)
{
    const std::wstring k = sanitize(key);

    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_keys.find(k);
    if (it != g_keys.end()) {
        Entry &e = g_entries[it->second];
        if (!same_spec(e.spec, spec)) {
            e.spec = spec;
            e.dirty = true;
        }
        if (g_wake) SetEvent(g_wake);
        return it->second;
    }

    const int id = g_next++;
    Entry e;
    e.spec = spec;
    e.stem = k;
    g_entries[id] = e;
    g_keys[k] = id;

    if (g_wake) SetEvent(g_wake);
    return id;
}

int find(const wchar_t *key)
{
    const std::wstring k = sanitize(key);
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_keys.find(k);
    return it == g_keys.end() ? 0 : it->second;
}

void close(int id)
{
    std::wstring dir = chart_dir();
    std::wstring stem;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_entries.find(id);
        if (it == g_entries.end()) return;
        stem = it->second.stem;
        g_keys.erase(stem);
        g_entries.erase(it);
    }

    if (!stem.empty() && !dir.empty()) cleanup_files(dir, stem, -1, -1);
}

View view(int id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_entries.find(id);
    if (it == g_entries.end()) return View();
    return it->second.view;
}

std::wstring span_text(int id)
{
    Spec s;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_entries.find(id);
        if (it == g_entries.end()) return std::wstring();
        s = it->second.spec;
    }

    if (s.window == stats::kToday) return L"今日 00:00 起";
    if (s.window == stats::kDay)   return L"24 小时前";
    if (s.window == stats::kWeek)  return L"7 天前";

    if (s.hours % 24 == 0) return std::to_wstring(s.hours / 24) + L" 天前";
    return std::to_wstring(s.hours) + L" 小时前";
}

std::wstring series_text(int id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_entries.find(id);
    if (it == g_entries.end()) return std::wstring();
    if (it->second.spec.dual) return L"温度 / 湿度";
    return it->second.spec.series == kHum ? L"湿度" : L"温度";
}

void start()
{
    if (g_started.exchange(true)) return;

    g_stop = false;
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(loop).detach();
}

void stop()
{
    g_stop = true;
    if (g_wake) SetEvent(g_wake);
}

} // namespace chart
