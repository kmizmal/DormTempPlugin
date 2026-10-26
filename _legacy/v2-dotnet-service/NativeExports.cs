using System;
using System.Runtime.InteropServices;

namespace PluginDormTempImpl;

/// <summary>原生导出（由 shim 转发调用）。签名对应 Rainmeter 插件 API。</summary>
internal static class NativeExports
{
    [UnmanagedCallersOnly(EntryPoint = "Initialize")]
    public static unsafe uint Initialize(nint file, nint section, uint id)
    {
        try { MeasureHost.OnInitialize(file, section, id); return id; }
        catch (Exception ex) { BleCollector.Log("Initialize: " + ex); return id; }
    }

    [UnmanagedCallersOnly(EntryPoint = "Finalize")]
    public static unsafe void Finalize(nint file, nint section, uint id)
    {
        try { MeasureHost.OnFinalize(file, section, id); }
        catch (Exception ex) { BleCollector.Log("Finalize: " + ex); }
    }

    [UnmanagedCallersOnly(EntryPoint = "Update")]
    public static unsafe double Update(uint id)
    {
        try { return MeasureHost.OnUpdate(id); }
        catch (Exception ex) { BleCollector.Log("Update: " + ex); return 0; }
    }

    [UnmanagedCallersOnly(EntryPoint = "GetString")]
    public static unsafe nint GetString(uint id)
    {
        try { return MeasureHost.OnGetString(id); }
        catch (Exception ex) { BleCollector.Log("GetString: " + ex); return 0; }
    }

    [UnmanagedCallersOnly(EntryPoint = "ExecuteBang")]
    public static unsafe void ExecuteBang(nint args, uint id)
    {
        try { MeasureHost.OnExecuteBang(args, id); }
        catch (Exception ex) { BleCollector.Log("ExecuteBang: " + ex); }
    }
}
