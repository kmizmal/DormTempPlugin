using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using PluginDormTempImpl;

namespace DormTempService;

/// <summary>
/// 边车服务：BLE 采集 + 把快照写入共享内存供 Rainmeter 插件读取。
/// 用法: DormTempService.exe [--logpath DIR] [--name DEVNAME] [--mac AA:BB:..] [--nolog]
/// </summary>
internal static class Program
{
    // 共享内存布局（与 shim/shim.c 保持一致）
    internal const int MapSize = 256;
    internal const uint Magic = 0x4D544244;   // "DBTM"
    internal const uint Version = 1;

    private static Mutex _single;

    private static int Main(string[] args)
    {
        bool created;
        _single = new Mutex(true, "Local\\DormTempService_SingleInstance", out created);
        if (!created)
        {
            return 0; // 已有实例
        }

        string logPath = null, name = "MJWSD06MMC", mac = "";
        bool nolog = false;
        for (int i = 0; i < args.Length - 1; i++)
        {
            switch (args[i])
            {
                case "--logpath": logPath = args[i + 1]; break;
                case "--name": name = args[i + 1]; break;
                case "--mac": mac = args[i + 1]; break;
            }
        }
        if (Array.IndexOf(args, "--nolog") >= 0) nolog = true;

        var collector = BleCollector.GetOrCreate();
        collector.ApplyConfig(mac, name, logPath, nolog);

        // 注册表/事件：常驻
        using var mmf = MemoryMappedFile.CreateOrOpen("Local\\DormTempPlugin_IPC", MapSize);
        using var acc = mmf.CreateViewAccessor();

        WriteHeader(acc);
        BleCollector.Log("service started");

        var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (s, e) => { e.Cancel = true; cts.Cancel(); };

        while (!cts.IsCancellationRequested)
        {
            var (temp, hum, volt, last, macText, phase, ever) = collector.Snapshot();
            Write(acc, temp, hum, volt, last, phase, ever, macText);
            try { Task.Delay(2000, cts.Token).Wait(cts.Token); }
            catch (AggregateException) { break; }
        }
        return 0;
    }

    private static unsafe void WriteHeader(MemoryMappedViewAccessor acc)
    {
        byte* p = null;
        acc.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        try
        {
            WriteU32(p, 0, Magic);
            WriteU32(p, 4, Version);
        }
        finally { acc.SafeMemoryMappedViewHandle.ReleasePointer(); }
    }

    private static unsafe void Write(MemoryMappedViewAccessor acc,
        double temp, double hum, double volt, DateTime last, int phase, bool ever, string macText)
    {
        byte* p = null;
        acc.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        try
        {
            WriteU32(p, 0, Magic);
            WriteU32(p, 4, Version);
            WriteF64(p, 8, double.IsNaN(temp) ? -999 : temp);
            WriteF64(p, 16, double.IsNaN(hum) ? -999 : hum);
            WriteF64(p, 24, double.IsNaN(volt) ? -999 : volt);
            WriteI64(p, 32, last == DateTime.MinValue ? 0 : last.ToFileTime());
            WriteU32(p, 40, (uint)phase);
            WriteU32(p, 44, ever ? 1u : 0u);
            WriteI64(p, 48, DateTime.UtcNow.ToFileTime());   // 心跳
            // mac: UTF-16, offset 56, 最多 31 字符 + NUL
            if (!string.IsNullOrEmpty(macText))
            {
                int n = Math.Min(31, macText.Length);
                for (int i = 0; i < n; i++)
                {
                    p[56 + i * 2] = (byte)macText[i];
                    p[57 + i * 2] = 0;
                }
                p[56 + n * 2] = 0; p[57 + n * 2] = 0;
            }
        }
        finally { acc.SafeMemoryMappedViewHandle.ReleasePointer(); }
    }

    private static unsafe void WriteU32(byte* p, int off, uint v)
    {
        p[off] = (byte)v; p[off + 1] = (byte)(v >> 8);
        p[off + 2] = (byte)(v >> 16); p[off + 3] = (byte)(v >> 24);
    }
    private static unsafe void WriteF64(byte* p, int off, double v)
        => WriteI64(p, off, BitConverter.DoubleToInt64Bits(v));
    private static unsafe void WriteI64(byte* p, int off, long v)
    {
        for (int i = 0; i < 8; i++) p[off + i] = (byte)(v >> (i * 8));
    }
}
