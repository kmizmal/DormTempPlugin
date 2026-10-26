using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.InteropServices;

namespace PluginDormTempImpl;

/// <summary>
/// Rainmeter 插件入口：每个 [Measure] 一个 id。
/// 支持的选项（写在皮肤 measure 段）：
///   Type       = Temperature | Humidity | Voltage | Status | LastUpdate | Battery | Comfort | SampleAge | Mac
///   MacAddress = 可选，固定 MAC（如 AA:BB:CC:DD:EE:FF）
///   DeviceName = 可选，广播名，默认 MJWSD06MMC
///   LogPath    = 可选，CSV 输出目录
///   DisableLogging = 0/1
/// </summary>
internal static class MeasureHost
{
    private sealed class Measure
    {
        public string Section;
        public string Type = "Temperature";
        public IntPtr StringBuf;      // GetString 返回的持久缓冲
        public double LastValue;
    }

    private static readonly ConcurrentDictionary<uint, Measure> Measures = new();
    private static readonly object Gate = new();
    private static bool _collectorStarted;

    // ---------- 选项解析 ----------
    internal static void OnInitialize(IntPtr file, IntPtr section, uint id)
    {
        string iniFile = Marshal.PtrToStringUni(file);
        string sect = Marshal.PtrToStringUni(section);
        var m = new Measure { Section = sect };
        Measures[id] = m;

        m.Type = ReadOption(iniFile, sect, "Type", "Temperature");

        // 采集器配置：全局生效（同 ini 读一次即可，重复 Apply 也无害）
        string mac = ReadOption(iniFile, sect, "MacAddress", "");
        string name = ReadOption(iniFile, sect, "DeviceName", "MJWSD06MMC");
        string logPath = ReadOption(iniFile, sect, "LogPath", "");
        bool noLog = ReadOption(iniFile, sect, "DisableLogging", "0").Trim() == "1";

        lock (Gate)
        {
            BleCollector.GetOrCreate().ApplyConfig(mac, name, logPath, noLog);
            _collectorStarted = true;
        }
        BleCollector.Log($"init id={id} section={sect} type={m.Type}");
    }

    internal static void OnFinalize(IntPtr file, IntPtr section, uint id)
    {
        if (Measures.TryRemove(id, out var m) && m.StringBuf != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(m.StringBuf);
            m.StringBuf = IntPtr.Zero;
        }
    }

    internal static double OnUpdate(uint id)
    {
        if (!Measures.TryGetValue(id, out var m)) return 0;
        var c = BleCollector.GetOrCreate();
        var (temp, hum, volt, last, _, phase, ever) = c.Snapshot();
        bool fresh = (DateTime.Now - last).TotalSeconds < 120;

        switch (m.Type)
        {
            case "Temperature": m.LastValue = double.IsNaN(temp) ? 0 : temp; break;
            case "Humidity": m.LastValue = double.IsNaN(hum) ? 0 : hum; break;
            case "Voltage": m.LastValue = double.IsNaN(volt) ? 0 : volt; break;
            case "SampleAge": m.LastValue = double.IsNaN(temp) ? -1 : (DateTime.Now - last).TotalSeconds; break;
            case "Online": m.LastValue = (phase == 2 && fresh) ? 1 : 0; break;
            default: m.LastValue = 0; break;
        }
        return m.LastValue;
    }

    internal static IntPtr OnGetString(uint id)
    {
        if (!Measures.TryGetValue(id, out var m)) return IntPtr.Zero;
        var c = BleCollector.GetOrCreate();
        var (temp, hum, volt, last, mac, phase, ever) = c.Snapshot();
        bool fresh = (DateTime.Now - last).TotalSeconds < 120;

        string s;
        switch (m.Type)
        {
            case "Temperature":
                s = double.IsNaN(temp) ? "--" : temp.ToString("0.0", CultureInfo.InvariantCulture);
                break;
            case "Humidity":
                s = double.IsNaN(hum) ? "--" : hum.ToString("0", CultureInfo.InvariantCulture);
                break;
            case "Voltage":
                s = double.IsNaN(volt) ? "--" : volt.ToString("0.00", CultureInfo.InvariantCulture);
                break;
            case "LastUpdate":
                s = double.IsNaN(temp) ? "--:--" : last.ToString("HH:mm:ss");
                break;
            case "Status":
                if (phase == 2 && fresh) s = "在线";
                else if (phase == 2) s = "数据超时";
                else if (phase == 1) s = ever ? "重连中" : "连接中";
                else if (ever) s = "离线";
                else s = "搜索中";
                break;
            case "Battery":
                if (double.IsNaN(volt)) s = "--";
                else if (volt >= 3.0) s = "电量充足";
                else if (volt >= 2.7) s = "电量偏低";
                else s = "需更换电池";
                break;
            case "Comfort":
                if (double.IsNaN(temp) || double.IsNaN(hum)) s = "--";
                else if (temp < 18) s = "偏冷";
                else if (temp > 28) s = "偏热";
                else if (hum < 40) s = "干燥";
                else if (hum > 70) s = "潮湿";
                else s = "舒适";
                break;
            case "Mac":
                s = mac;
                break;
            default:
                s = "";
                break;
        }
        return StoreString(m, s);
    }

    internal static void OnExecuteBang(IntPtr args, uint id)
    {
        string a = Marshal.PtrToStringUni(args)?.Trim() ?? "";
        var c = BleCollector.GetOrCreate();
        if (a.Equals("Reconnect", StringComparison.OrdinalIgnoreCase)) c.ForceReconnect();
    }

    // ---------- ini 读取（GetPrivateProfileString + 简易 #变量# 解析） ----------
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetPrivateProfileString(string app, string key, string def,
        System.Text.StringBuilder buf, uint size, string file);

    private static string ReadOption(string iniFile, string section, string key, string def)
    {
        if (string.IsNullOrEmpty(iniFile)) return def;
        var sb = new System.Text.StringBuilder(1024);
        uint n = GetPrivateProfileString(section, key, def, sb, (uint)sb.Capacity, iniFile);
        string v = sb.ToString(0, (int)n).Trim();
        if (n == 0 && v.Length == 0) return def;
        return ResolveVariables(v, iniFile);
    }

    /// <summary>解析 #Var#（来自 [Variables] 段）与 #CurrentSection#。</summary>
    private static string ResolveVariables(string input, string iniFile)
    {
        if (input.IndexOf('#') < 0) return input;
        var sb = new System.Text.StringBuilder(1024);
        return System.Text.RegularExpressions.Regex.Replace(input, "#([^#]+)#", mm =>
        {
            string varName = mm.Groups[1].Value;
            GetPrivateProfileString("Variables", varName, "", sb, (uint)sb.Capacity, iniFile);
            string v = sb.ToString(0, (int)sb.Length).Trim();
            return v.Length > 0 ? v : mm.Value;
        });
    }

    private static IntPtr StoreString(Measure m, string s)
    {
        IntPtr nb = Marshal.StringToHGlobalUni(s);
        IntPtr old = m.StringBuf;
        m.StringBuf = nb;
        if (old != IntPtr.Zero) Marshal.FreeHGlobal(old);
        return nb;
    }
}
