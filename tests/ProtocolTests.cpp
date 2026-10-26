// ============================================================================
//  ProtocolTests.cpp —— 协议 / 状态 / 存储的最小可执行测试集
//
//  只依赖纯逻辑模块（不启蓝牙、不依赖 Rainmeter）：
//      DormProtocol.cpp  SharedState.cpp  Config.cpp  Store.cpp
//  任何一条失败都以非 0 退出码结束，可直接接进构建脚本。
// ============================================================================
#include <windows.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Chart.h"
#include "Config.h"
#include "DormProtocol.h"
#include "SharedState.h"
#include "Stats.h"
#include "Store.h"

namespace {

int g_passed = 0;
std::vector<std::string> g_failures;

std::string narrow(const std::wstring &w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

void check(bool cond, const char *name, const std::string &detail = std::string())
{
    if (cond) {
        ++g_passed;
        return;
    }
    g_failures.push_back(detail.empty() ? std::string(name) : std::string(name) + "（" + detail + "）");
    printf("  FAIL  %s%s\n", name, detail.empty() ? "" : ("  -> " + detail).c_str());
}

void check_equal(const std::wstring &actual, const std::wstring &expected, const char *name)
{
    check(actual == expected, name, actual == expected
                                     ? ""
                                     : "期望「" + narrow(expected) + "」，实际「" + narrow(actual) + "」");
}

void check_near(double actual, double expected, const char *name, double eps = 1e-6)
{
    char detail[128];
    snprintf(detail, sizeof(detail), "期望 %.6f，实际 %.6f", expected, actual);
    check(std::isfinite(actual) && std::fabs(actual - expected) <= eps, name, detail);
}

// ---------------------------------------------------------------- 协议
void test_parse()
{
    printf("[protocol] 通知负载解析\n");

    const uint8_t frame[] = {0x3A, 0x09, 0x31, 0xB9, 0x0B};   // 23.62℃ / 49% / 3.001V
    Reading r;
    check(protocol::parse(frame, sizeof(frame), &r), "5 字节帧应解析成功");
    check(r.valid, "valid 应为 true");
    check_near(r.temp, 23.62, "温度 int16 小端 ÷100");
    check_near(r.hum, 49.0, "湿度取第 3 字节");
    check_near(r.volt, 3.001, "电压 uint16 小端 ÷1000");

    const uint8_t negative[] = {0x0C, 0xFE, 0x32, 0xE8, 0x03};  // -5.00℃ / 50% / 1.000V
    check(protocol::parse(negative, sizeof(negative), &r), "负温度帧应解析成功");
    check_near(r.temp, -5.0, "负温度按有符号 int16 处理");
    check_near(r.volt, 1.0, "电压 0x03E8 = 1000mV");

    const uint8_t zero_hum[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    check(protocol::parse(zero_hum, sizeof(zero_hum), &r), "零值帧应解析成功");
    check_near(r.temp, 0.0, "0℃");
    check_near(r.hum, 0.0, "0%RH");

    const uint8_t max_frame[] = {0xFF, 0x7F, 0x64, 0xFF, 0xFF};  // 327.67℃ / 100% / 65.535V
    check(protocol::parse(max_frame, sizeof(max_frame), &r), "上界帧应解析成功");
    check_near(r.temp, 327.67, "int16 最大值 ÷100");
    check_near(r.hum, 100.0, "湿度上界 100");

    const uint8_t padded[] = {0x3A, 0x09, 0x31, 0xB9, 0x0B, 0xFF, 0xFF};
    check(protocol::parse(padded, sizeof(padded), &r), "多余字节应忽略而不是报错");
    check_near(r.temp, 23.62, "补齐帧温度仍取前 5 字节");

    const uint8_t short_frame[] = {0x3A, 0x09, 0x31, 0xB9};
    r = Reading{};
    r.temp = 123.0;
    check(!protocol::parse(short_frame, sizeof(short_frame), &r), "4 字节帧应判为无效");
    check(!r.valid, "无效帧不应留下 valid");
    check(std::isnan(r.temp), "无效帧应把输出清零");

    check(!protocol::parse(nullptr, 5, &r), "空指针应返回 false");
    check(!protocol::parse(frame, sizeof(frame), nullptr), "输出为空应返回 false");
    check(protocol::kMinPayload == 5, "最小负载长度常量应为 5");
}

void test_name_matching()
{
    printf("[protocol] 广播名匹配\n");
    check(protocol::name_matches(L"MJWSD06MMC", L"MJWSD06MMC"), "完全一致应匹配");
    check(protocol::name_matches(L"mjwsd06mmc", L"MJWSD06MMC"), "应忽略大小写");
    check(!protocol::name_matches(L"MJWSD06MM", L"MJWSD06MMC"), "前缀不应误匹配");
    check(!protocol::name_matches(L"", L"MJWSD06MMC"), "空广播名不匹配");
    check(!protocol::name_matches(L"MJWSD06MMC", L""), "空目标名不匹配");
    check(!protocol::name_matches(nullptr, L"MJWSD06MMC"), "空指针不匹配");
}

// ---------------------------------------------------------------- 文本
void test_number_text()
{
    printf("[state] 数值格式化\n");
    check_equal(number_text(23.62, 1), L"23.6", "温度保留 1 位");
    // 23.65 在 double 里其实略小于 23.65，printf 会舍到 23.6；用 23.66 验证进位
    check_equal(number_text(23.66, 1), L"23.7", "四舍五入（23.66 -> 23.7）");
    check_equal(number_text(49.0, 0), L"49", "湿度取整");
    check_equal(number_text(3.001, 2), L"3.00", "电压保留 2 位");
    check_equal(number_text(-5.0, 1), L"-5.0", "负温度保留符号");
    check_equal(number_text(std::nan(""), 1), L"--", "NaN 回退到 --");
    check_equal(number_text(INFINITY, 1), L"--", "Inf 回退到 --");
    check_equal(number_text(std::nan(""), 1, L"N/A"), L"N/A", "支持自定义回退值");
    check_equal(clock_text(0), L"--:--", "从未采样时时间为 --:--");
}

void test_status_text()
{
    printf("[state] 连接状态文案\n");
    check_equal(status_text(kOnline, true, 0.0), L"在线", "在线且刚采到");
    check_equal(status_text(kOnline, true, 179.9), L"在线", "179.9s 仍算在线");
    check_equal(status_text(kOnline, true, 180.0), L"数据超时", "180s 判为超时");
    check_equal(status_text(kOnline, true, -1.0), L"数据超时", "没有采样时间也算超时");
    check_equal(status_text(kConnecting, false, -1.0), L"连接中", "首次连接");
    check_equal(status_text(kConnecting, true, -1.0), L"重连中", "曾经连上过则显示重连中");
    check_equal(status_text(kSearching, false, -1.0), L"搜索中", "未连过且未扫到");
    check_equal(status_text(kSearching, true, -1.0), L"离线", "连过但现在没目标");
}

void test_battery_text()
{
    printf("[state] 电量档位\n");
    check_equal(battery_text(3.2), L"电量充足", "3.2V 充足");
    check_equal(battery_text(3.0), L"电量充足", "3.0V 边界算充足");
    check_equal(battery_text(2.999), L"电量偏低", "3.0V 以下偏低");
    check_equal(battery_text(2.7), L"电量偏低", "2.7V 边界算偏低");
    check_equal(battery_text(2.699), L"需更换电池", "2.7V 以下需更换");
    check_equal(battery_text(std::nan("")), L"--", "没有电压时占位");
}

void test_comfort_text()
{
    printf("[state] 舒适度判定\n");
    check_equal(comfort_text(17.9, 50), L"偏冷", "低于 18℃ 偏冷");
    check_equal(comfort_text(18.0, 50), L"舒适", "18℃ 边界舒适");
    check_equal(comfort_text(28.0, 50), L"舒适", "28℃ 边界舒适");
    check_equal(comfort_text(28.1, 50), L"偏热", "高于 28℃ 偏热");
    check_equal(comfort_text(24.0, 39.9), L"干燥", "湿度低于 40% 干燥");
    check_equal(comfort_text(24.0, 40.0), L"舒适", "40% 边界舒适");
    check_equal(comfort_text(24.0, 70.0), L"舒适", "70% 边界舒适");
    check_equal(comfort_text(24.0, 70.1), L"潮湿", "湿度高于 70% 潮湿");
    check_equal(comfort_text(std::nan(""), 50), L"--", "缺温度时占位");
    check_equal(comfort_text(24.0, std::nan("")), L"--", "缺湿度时占位");
}

// ---------------------------------------------------------------- 配置
void test_mac()
{
    printf("[config] MAC 解析与格式化\n");
    unsigned long long addr = 0;

    check(config::parse_mac(L"AA:BB:CC:DD:EE:FF", &addr), "带冒号应能解析");
    check(addr == 0xAABBCCDDEEFFULL, "冒号形式数值正确");
    check(config::parse_mac(L"aa-bb-cc-dd-ee-ff", &addr), "小写加短横应能解析");
    check(addr == 0xAABBCCDDEEFFULL, "短横形式数值正确");
    check(config::parse_mac(L"aabbccddeeff", &addr), "纯十六进制应能解析");
    check(addr == 0xAABBCCDDEEFFULL, "纯十六进制数值正确");
    check(config::parse_mac(L"AA:BB:CC", &addr), "6 位缩写应能解析");
    check(addr == 0xAABBCCULL, "缩写数值正确");

    check(!config::parse_mac(L"AABBCCD", &addr), "奇数位应拒绝");
    check(!config::parse_mac(L"ABC", &addr), "位数不足应拒绝");
    check(!config::parse_mac(L"AABBCCDDEEFF00", &addr), "超长应拒绝");
    check(!config::parse_mac(L"", &addr), "空串应拒绝");
    check(!config::parse_mac(L"GG:HH:II:JJ:KK:LL", &addr), "非十六进制应拒绝");
    check(!config::parse_mac(L"AA:BB:CC:DD:EE:FF", nullptr), "输出为空应拒绝");

    check_equal(config::format_mac(0xAABBCCDDEEFFULL), L"AA:BB:CC:DD:EE:FF", "格式化补冒号");
    check_equal(config::format_mac(0x000000000001ULL), L"00:00:00:00:00:01", "左侧补零");
    check_equal(config::format_mac(0), L"00:00:00:00:00:00", "全零");
    check(config::parse_mac(config::format_mac(0x1234567890ABULL), &addr), "格式化结果可回解");
    check(addr == 0x1234567890ABULL, "回解数值一致");
}

void test_config_values()
{
    printf("[config] 选项写入规则\n");
    config::set_log_dir(L"C:\\tmp\\dorm-a");
    check_equal(config::dir(), L"C:\\tmp\\dorm-a", "设置目录生效");

    config::set_log_dir(L"");
    check_equal(config::dir(), L"C:\\tmp\\dorm-a", "空值不覆盖已有目录");

    config::set_device_name(L"MY-SENSOR");
    check_equal(config::get().deviceName, L"MY-SENSOR", "设备名可修改");

    config::set_device_name(L"");
    check_equal(config::get().deviceName, L"MY-SENSOR", "空设备名不覆盖");

    config::set_mac_opt(L"AA:BB:CC:DD:EE:FF");
    check_equal(config::get().macOpt, L"AA:BB:CC:DD:EE:FF", "固定 MAC 可写入");

    config::set_mac_opt(L"");
    check_equal(config::get().macOpt, L"", "MAC 允许清空");
}

// ---------------------------------------------------------------- 快照
void test_state()
{
    printf("[state] 快照读写\n");
    const Snapshot before = state::get();
    check(!std::isfinite(before.temp), "初始快照没有温度");
    check(before.status == kSearching, "初始状态为搜索中");

    Reading r;
    const uint8_t frame[] = {0x3A, 0x09, 0x31, 0xB9, 0x0B};
    protocol::parse(frame, sizeof(frame), &r);
    const unsigned long long stamp = 132000000000000000ULL;
    state::publish(r, stamp);

    const Snapshot after = state::get();
    check_near(after.temp, 23.62, "快照温度已更新");
    check_near(after.hum, 49.0, "快照湿度已更新");
    check(after.lastSampleFT == stamp, "采样时间戳已记录");

    state::publish(Reading{}, 0);
    check_near(state::get().temp, 23.62, "无效读数不应覆盖快照");

    state::set_status(kOnline);
    check(state::get().status == kOnline, "状态可写入");

    state::set_mac(0xAABBCCDDEEFFULL, true);
    const Snapshot mac = state::get();
    check_equal(mac.mac, L"AA:BB:CC:DD:EE:FF", "MAC 以带冒号文本保存");
    check(mac.ever, "连上过标记置位");

    const int base = state::measure_count();
    state::measure_add();
    check(state::measure_count() == base + 1, "measure 计数增加");
    state::measure_remove();
    check(state::measure_count() == base, "measure 计数减少");
    state::measure_remove();
    state::measure_remove();
    check(state::measure_count() == 0, "计数不会变成负数");

    check(seconds_since(0) < 0, "未采样时 age 为负");
}

// ---------------------------------------------------------------- 存储
void test_store()
{
    printf("[store] SQLite 落盘\n");

    const std::wstring dir = L"testdata\\store";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    store::init(L"");
    store::close();

    check(store::init(dir), "初始化数据库成功（winsqlite3 可用）");
    check(store::available(), "初始化后 available");
    check(store::count() == 0, "新库没有记录");

    store::sample(23.62, 49, 3.001);
    check(store::count() == 1, "写入一条采样");

    store::sample(23.70, 50, 3.001);
    check(store::count() == 1, "同一秒内的重复通知被去重");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store::sample(24.01, 48, 2.999);
    check(store::count() == 2, "跨秒的采样正常入库");

    store::Row rows[8] = {};
    const int n = store::recent(8, rows);
    check(n == 2, "recent 返回全部记录");
    if (n == 2) {
        check_near(rows[0].temp, 24.01, "recent 按 id 倒序（最新在前）");
        check_near(rows[0].hum, 48, "最新记录湿度正确");
        check_near(rows[0].volt, 2.999, "最新记录电压正确");
        check_near(rows[1].temp, 23.62, "第二条为更早的采样");
        check(rows[0].ts >= rows[1].ts, "时间戳单调不减");
    }
    check(store::recent(1, rows) == 1, "limit 生效");
    check(store::recent(0, rows) == 0, "limit 为 0 时不返回数据");
    check(store::recent(4, nullptr) == 0, "空输出缓冲不崩溃");

    store::Agg agg;
    check(store::aggregate(0, &agg), "全库聚合返回成功");
    check(agg.n == 2, "全库聚合条数正确");
    check_near(agg.tMin, 23.62, "聚合温度最低值");
    check_near(agg.tMax, 24.01, "聚合温度最高值");
    check_near(agg.tAvg, (23.62 + 24.01) / 2.0, "聚合温度均值", 1e-9);
    check_near(agg.hMin, 48, "聚合湿度最低值");
    check_near(agg.hMax, 49, "聚合湿度最高值");
    check_near(agg.hAvg, 48.5, "聚合湿度均值", 1e-9);
    check(agg.tMaxTs >= agg.tMinTs, "温度最高时刻晚于最低时刻");

    store::Agg future;
    check(store::aggregate(4102444800LL, &future), "未来时间点聚合仍返回成功");  // 2100-01-01
    check(future.n == 0, "空窗口条数为 0");
    check(!store::aggregate(0, nullptr), "空输出指针返回 false");

    store::close();
    check(!store::available(), "close 之后 available 为 false");
    check(store::count() == -1, "关闭后查询返回 -1");

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------- 区间汇总
void test_stats()
{
    printf("[stats] 区间汇总缓存\n");

    check(stats::window_from_text(nullptr) == stats::kToday, "空选项默认为今天");
    check(stats::window_from_text(L"Today") == stats::kToday, "Today -> 今天");
    check(stats::window_from_text(L"today") == stats::kToday, "大小写不敏感");
    check(stats::window_from_text(L"24h") == stats::kDay, "24h -> 近 24 小时");
    check(stats::window_from_text(L"Day") == stats::kDay, "Day -> 近 24 小时");
    check(stats::window_from_text(L"7d") == stats::kWeek, "7d -> 近 7 天");
    check(stats::window_from_text(L"垃圾值") == stats::kToday, "无法识别的窗口回落到今天");

    const long long now = static_cast<long long>(time(nullptr));
    const long long today = stats::window_start(stats::kToday, now);
    check(today > 0 && today <= now, "今天窗口起点不晚于当前时刻");
    check(now - today < 86400 + 3600, "今天窗口起点在最近 24 小时内");
    check(stats::window_start(stats::kDay, now) == now - 86400, "近 24 小时窗口起点");
    check(stats::window_start(stats::kWeek, now) == now - 7 * 86400, "近 7 天窗口起点");
    check(stats::window_start(99, now) == now - 86400, "非法窗口回落到 24 小时");

    // store 此时已被 test_store 关掉，刷新应失败且缓存保持空
    check(!stats::refresh(), "库不可用时刷新失败");
    check(!stats::get(stats::kToday).valid, "无数据时汇总标记为无效");
    check_equal(stats::summary_text(stats::get(stats::kToday)), L"窗口内暂无数据", "空汇总的文案");
    check(stats::get(99).window == stats::kToday, "非法窗口号取今天");

    const std::wstring dir = L"testdata\\stats";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    store::init(L"");
    store::close();
    check(store::init(dir), "统计用临时库初始化");
    store::sample(23.00, 40, 3.000);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store::sample(25.00, 60, 3.000);

    check(stats::refresh(), "有数据时刷新成功");
    const stats::Summary d = stats::get(stats::kToday);
    check(d.valid, "今天窗口有效");
    check(d.n == 2, "今天窗口样本数");
    check_near(d.tMin, 23.0, "今天窗口温度最低");
    check_near(d.tMax, 25.0, "今天窗口温度最高");
    check_near(d.tAvg, 24.0, "今天窗口温度均值", 1e-9);
    check_near(d.hAvg, 50.0, "今天窗口湿度均值", 1e-9);
    check(d.tMaxTs >= d.tMinTs, "极值时刻顺序合理");
    check(stats::get(stats::kWeek).n == 2, "7 天窗口同样包含这两条");

    check(!stats::refresh_if_stale(60), "刚刷新过，60 秒内不重复查库");
    check(stats::refresh_if_stale(0), "节流窗口为 0 时立刻重算");

    const std::wstring text = stats::summary_text(d);
    check(text.find(L"今日") == 0, "摘要以窗口名开头");
    check(text.find(L"最高 25.0") != std::wstring::npos, "摘要包含最高温");
    check(text.find(L"样本 2 条") != std::wstring::npos, "摘要包含样本数");
    check_equal(stats::window_label(stats::kWeek), L"7 天", "窗口中文名");

    store::close();
    std::filesystem::remove_all(dir, ec);
}

} // namespace

void test_series()
{
    printf("[series] 分桶序列\n");

    const std::wstring dir = L"testdata\\series";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(store::init(dir), "序列测试初始化数据库");

    // 4 个整桶、每桶 600 秒，桶号 = (ts - since) / 600
    const long long now = static_cast<long long>(time(nullptr));
    const long long since = now - 2400;
    const long long until = since + 2400;

    store::sample_at(since + 100, 20.0, 40.0, 3.0);
    store::sample_at(since + 200, 20.0, 40.0, 3.0);   // 同桶第二点，验均值
    store::sample_at(since + 700, 22.0, 50.0, 3.0);
    store::sample_at(since + 1900, 24.0, 60.0, 3.0);  // 跳过第 3 桶

    store::Slot slots[8] = {};
    check(store::series(since, until, 4, slots), "分桶查询成功");
    check(slots[0].n == 2, "第 1 桶两条采样");
    check_near(slots[0].t, 20.0, "第 1 桶温度均值");
    check_near(slots[0].h, 40.0, "第 1 桶湿度均值");
    check(slots[1].n == 1, "第 2 桶一条采样");
    check_near(slots[1].t, 22.0, "第 2 桶温度均值");
    check(slots[2].n == 0, "空桶 n = 0（曲线在这里断线）");
    check(slots[3].n == 1, "第 4 桶一条采样");
    check_near(slots[3].t, 24.0, "第 4 桶温度均值");

    store::Slot one[1] = {};
    check(store::series(since, until, 1, one), "单桶查询成功");
    check(one[0].n == 4, "单桶收全部 4 条");
    check_near(one[0].t, (20.0 + 20.0 + 22.0 + 24.0) / 4, "单桶温度均值", 1e-9);

    store::Slot many[16] = {};
    check(store::series(since, until, 16, many), "16 桶查询成功");
    int total = 0;
    for (int i = 0; i < 16; ++i) total += many[i].n;
    check(total == 4, "换成 16 桶后条数总和不变");

    store::Slot empty[4] = {};
    check(store::series(until + 100, until + 900, 4, empty), "空区间仍返回成功");
    check(empty[0].n == 0 && empty[3].n == 0, "空区间所有桶都是 0");

    check(!store::series(since, since, 4, empty), "区间为 0 返回 false");
    check(!store::series(since, until, 0, empty), "桶数为 0 返回 false");
    check(!store::series(since, until, 4, nullptr), "空输出指针返回 false");

    check(store::latest_ts() >= since + 1900, "latest_ts 取到最新时间戳");

    store::close();
    std::filesystem::remove_all(dir, ec);
}

void test_axis()
{
    printf("[axis] 曲线坐标轴\n");

    double lo = 0, hi = 0, mid = 0;

    chart::axis_bounds(24.1, 25.2, chart::kTemp, &lo, &hi, &mid);
    check(lo <= 24.1 && hi >= 25.2, "温度轴包住数据");
    check_near(mid, (lo + hi) / 2, "中值等于上下界中点", 1e-9);
    check(hi - lo >= 2.0 - 1e-9, "温度轴最小跨度 2℃");

    chart::axis_bounds(24.30, 24.34, chart::kTemp, &lo, &hi, &mid);
    check(hi - lo >= 2.0 - 1e-9, "极窄波动也撑到 2℃");

    chart::axis_bounds(58, 64, chart::kHum, &lo, &hi, &mid);
    check(lo <= 58 && hi >= 64, "湿度轴包住数据");
    check(hi - lo >= 10.0 - 1e-9, "湿度轴最小跨度 10%");

    chart::axis_bounds(25, 25, chart::kTemp, &lo, &hi, &mid);
    check(hi > lo && lo <= 25 && hi >= 25, "零跨度不除以零，仍包住数据");

    chart::axis_bounds(std::nan(""), std::nan(""), chart::kTemp, &lo, &hi, &mid);
    check(std::isfinite(lo) && std::isfinite(hi) && hi > lo, "NaN 输入退回默认区间");
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    test_parse();
    test_name_matching();
    test_number_text();
    test_status_text();
    test_battery_text();
    test_comfort_text();
    test_mac();
    test_config_values();
    test_state();
    test_store();
    test_stats();
    test_series();
    test_axis();

    printf("\n通过 %d 项，失败 %d 项\n", g_passed, static_cast<int>(g_failures.size()));
    if (!g_failures.empty()) {
        for (const auto &f : g_failures) printf("  - %s\n", f.c_str());
        return 1;
    }
    return 0;
}
