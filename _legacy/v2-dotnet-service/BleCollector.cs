using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace PluginDormTempImpl;

/// <summary>
/// BLE 采集单例：扫描 MJWSD06MMC → 连接 → 订阅通知/定时读取 → 解析温湿度电压 → 写 CSV。
/// 与 dorm-temp-monitor(Python) 行为对齐：掉线自动找回、跨天切文件、记忆 MAC。
/// </summary>
internal sealed class BleCollector
{
    // LYWSD06MMC（小米温湿度计 3 mini）明文服务/特征
    private static readonly Guid ServiceUuid = new("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
    private static readonly Guid CharUuid = new("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");
    private const string DefaultDeviceName = "MJWSD06MMC";

    private static BleCollector _instance;
    private static readonly object InstLock = new();

    internal static BleCollector GetOrCreate()
    {
        lock (InstLock)
        {
            if (_instance == null)
            {
                _instance = new BleCollector();
                _instance.Start();
            }
            return _instance;
        }
    }

    // ---- 可配置项 ----
    private string _deviceName = DefaultDeviceName;
    private string _logDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DormTempPlugin");
    private bool _disableLogging;
    private ulong? _fixedMac;   // 显式配置的 MAC（十进制 ulong 或 AA:BB:CC 格式）
    private volatile bool _configured;

    // ---- 运行状态（lock _state 访问） ----
    private readonly object _state = new();
    private double _temp = double.NaN;
    private double _hum = double.NaN;
    private double _volt = double.NaN;
    private DateTime _lastSample = DateTime.MinValue;
    private string _macText = "";
    private int _phase;          // 0=搜索中 1=连接中 2=在线
    private bool _everConnected;

    // ---- BLE 对象 ----
    private Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher _watcher;
    private Windows.Devices.Bluetooth.BluetoothLEDevice _device;
    private Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic _characteristic;
    private ulong _targetAddr;
    private bool _hasTargetAddr;
    private readonly CancellationTokenSource _cts = new();
    private readonly ConcurrentQueue<string> _log = new();
    private static readonly string LogFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "DormTempPlugin", "plugin_debug.log");

    private BleCollector() { }

    /// <summary>应用来自皮肤 ini 的配置（首个 measure Initialize 时调用一次；刷新时覆盖）。</summary>
    internal void ApplyConfig(string macOption, string deviceName, string logPath, bool disableLogging)
    {
        lock (_state)
        {
            if (!string.IsNullOrWhiteSpace(deviceName)) _deviceName = deviceName.Trim();
            if (!string.IsNullOrWhiteSpace(logPath)) _logDir = Environment.ExpandEnvironmentVariables(logPath.Trim());
            _disableLogging = disableLogging;
            _fixedMac = ParseMac(macOption);
        }
    }

    private static ulong? ParseMac(string s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        s = s.Trim().Replace(":", "").Replace("-", "");
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
        return ulong.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var v) ? v : null;
    }

    private void Start()
    {
        Task.Run(() => SafeAsync("MainLoop", MainLoopAsync));
    }

    private async Task MainLoopAsync()
    {
        while (!_cts.IsCancellationRequested)
        {
            try
            {
                if (_device != null &&
                    _device.ConnectionStatus == Windows.Devices.Bluetooth.BluetoothConnectionStatus.Connected)
                {
                    await Task.Delay(2000, _cts.Token).ConfigureAwait(false);
                    continue;
                }

                // 每轮重连前重新评估目标（配置可能晚于启动到达）
                if (_fixedMac.HasValue)
                {
                    if (_targetAddr != _fixedMac.Value) { _targetAddr = _fixedMac.Value; _hasTargetAddr = true; }
                }
                else if (!_hasTargetAddr)
                {
                    ulong remembered = ReadRememberedMac();
                    if (remembered != 0) { _targetAddr = remembered; _hasTargetAddr = true; }
                }

                // 断线：清理并重连
                await TeardownAsync().ConfigureAwait(false);
                SetPhase(_hasTargetAddr ? 1 : 0);

                if (!_hasTargetAddr)
                {
                    await ScanAsync(_cts.Token).ConfigureAwait(false);
                    if (!_hasTargetAddr) { await Task.Delay(3000, _cts.Token).ConfigureAwait(false); continue; }
                }

                SetPhase(1);
                if (!await ConnectAsync(_targetAddr).ConfigureAwait(false))
                {
                    // 直连失败：清空目标重新扫描（设备可能换了 MAC 或需唤醒）
                    _hasTargetAddr = _fixedMac.HasValue; // 固定 MAC 不放弃
                    await Task.Delay(4000, _cts.Token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex) { Log("loop: " + ex.Message); await Task.Delay(5000).ConfigureAwait(false); }
        }
    }

    private async Task ScanAsync(CancellationToken ct)
    {
        ulong found = 0;
        string want = _deviceName;
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);

        _watcher?.Stop();
        _watcher = new Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher
        {
            ScanningMode = Windows.Devices.Bluetooth.Advertisement.BluetoothLEScanningMode.Active
        };
        _watcher.Received += (s, args) =>
        {
            try
            {
                string name = args.Advertisement?.LocalName ?? "";
                if (name.Equals(want, StringComparison.OrdinalIgnoreCase) && found == 0)
                {
                    found = args.BluetoothAddress;
                    tcs.TrySetResult(true);
                }
            }
            catch { }
        };
        _watcher.Start();
        try
        {
            var done = await Task.WhenAny(tcs.Task, Task.Delay(Timeout.Infinite, ct)).ConfigureAwait(false);
        }
        finally
        {
            try { _watcher.Stop(); } catch { }
        }

        if (found != 0)
        {
            _targetAddr = found;
            _hasTargetAddr = true;
            Log("scan found " + MacText(found));
        }
    }

    private async Task<bool> ConnectAsync(ulong addr)
    {
        var dev = await Windows.Devices.Bluetooth.BluetoothLEDevice
            .FromBluetoothAddressAsync(addr).AsTask().ConfigureAwait(false);
        if (dev == null) { Log("connect: device null"); return false; }

        var svcResult = await dev.GetGattServicesForUuidAsync(
            ServiceUuid, Windows.Devices.Bluetooth.BluetoothCacheMode.Uncached).AsTask().ConfigureAwait(false);
        if (svcResult.Status != Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus.Success
            || svcResult.Services.Count == 0)
        { Log("connect: svc " + svcResult.Status); dev.Dispose(); return false; }

        var charResult = await svcResult.Services[0].GetCharacteristicsForUuidAsync(
            CharUuid, Windows.Devices.Bluetooth.BluetoothCacheMode.Uncached).AsTask().ConfigureAwait(false);
        if (charResult.Status != Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus.Success
            || charResult.Characteristics.Count == 0)
        { Log("connect: char " + charResult.Status); dev.Dispose(); return false; }

        var ch = charResult.Characteristics[0];
        ch.ValueChanged += OnValueChanged;

        var cccd = await ch.WriteClientCharacteristicConfigurationDescriptorAsync(
            Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue.Notify)
            .AsTask().ConfigureAwait(false);
        if (cccd != Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus.Success)
        {
            cccd = await ch.WriteClientCharacteristicConfigurationDescriptorAsync(
                Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue.Indicate)
                .AsTask().ConfigureAwait(false);
        }
        if (cccd != Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus.Success)
        { Log("connect: cccd " + cccd); ch.ValueChanged -= OnValueChanged; dev.Dispose(); return false; }

        dev.ConnectionStatusChanged += OnConnectionChanged;
        _characteristic = ch;
        _device = dev;

        lock (_state)
        {
            _macText = MacText(addr);
            _everConnected = true;
            SetPhaseLocked(2);
        }
        SaveRememberedMac(addr);

        // 主动读取一次 + 定时保活读取
        _ = Task.Run(() => SafeAsync("keepalive", KeepaliveLoopAsync));
        Log("connected " + _macText);
        return true;
    }

    private async Task KeepaliveLoopAsync()
    {
        while (!_cts.IsCancellationRequested)
        {
            var ch = _characteristic; var dev = _device;
            if (ch != null && dev != null &&
                dev.ConnectionStatus == Windows.Devices.Bluetooth.BluetoothConnectionStatus.Connected)
            {
                try
                {
                    var r = await ch.ReadValueAsync(
                        Windows.Devices.Bluetooth.BluetoothCacheMode.Uncached)
                        .AsTask(new CancellationTokenSource(8000).Token).ConfigureAwait(false);
                    if (r.Status == Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus.Success)
                        HandleSample(r.Value);
                }
                catch { }
            }
            await Task.Delay(30000, _cts.Token).ConfigureAwait(false);
        }
    }

    private void OnValueChanged(object sender,
        Windows.Devices.Bluetooth.GenericAttributeProfile.GattValueChangedEventArgs args)
    {
        try { HandleSample(args.CharacteristicValue); } catch { }
    }

    private void OnConnectionChanged(Windows.Devices.Bluetooth.BluetoothLEDevice sender, object args)
    {
        if (sender.ConnectionStatus != Windows.Devices.Bluetooth.BluetoothConnectionStatus.Connected)
        {
            Log("disconnected");
            SetPhase(_hasTargetAddr ? 1 : 0);
        }
    }

    private void HandleSample(Windows.Storage.Streams.IBuffer buffer)
    {
        var reader = Windows.Storage.Streams.DataReader.FromBuffer(buffer);
        var bytes = new byte[buffer.Length];
        reader.ReadBytes(bytes);
        if (bytes.Length < 5) return;

        short rawT = (short)(bytes[0] | (bytes[1] << 8));
        double temp = rawT / 100.0;
        double hum = bytes[2];
        double volt = (bytes[3] | (bytes[4] << 8)) / 1000.0;
        var now = DateTime.Now;

        lock (_state)
        {
            _temp = temp; _hum = hum; _volt = volt; _lastSample = now;
        }
        if (!_disableLogging) AppendCsv(now, temp, hum, volt);
    }

    // ---- CSV（与 dorm-temp-monitor 格式一致）----
    private void AppendCsv(DateTime now, double temp, double hum, double volt)
    {
        try
        {
            string dir = _logDir;
            Directory.CreateDirectory(dir);
            string file = Path.Combine(dir, "temp_log_" + now.ToString("yyyy-MM-dd") + ".csv");
            bool newFile = !File.Exists(file);
            var sb = new StringBuilder();
            if (newFile) sb.Append('\uFEFF').AppendLine("时间,温度℃,湿度%,电压V");
            sb.Append(now.ToString("HH:mm:ss")).Append(',')
              .Append(temp.ToString("0.00", CultureInfo.InvariantCulture)).Append(',')
              .Append(hum.ToString("0", CultureInfo.InvariantCulture)).Append(',')
              .Append(volt.ToString("0.00", CultureInfo.InvariantCulture))
              .AppendLine();
            File.AppendAllText(file, sb.ToString(), new UTF8Encoding(false));
        }
        catch (Exception ex) { Log("csv: " + ex.Message); }
    }

    private ulong ReadRememberedMac()
    {
        try
        {
            if (_fixedMac.HasValue) return _fixedMac.Value;
            string p = Path.Combine(_logDir, "device_mac.txt");
            if (!File.Exists(p)) return 0;
            return ParseMac(File.ReadAllText(p).Trim()) ?? 0;
        }
        catch { return 0; }
    }

    private void SaveRememberedMac(ulong addr)
    {
        try
        {
            if (_fixedMac.HasValue) return;
            Directory.CreateDirectory(_logDir);
            File.WriteAllText(Path.Combine(_logDir, "device_mac.txt"), MacText(addr));
        }
        catch { }
    }

    private async Task TeardownAsync()
    {
        var dev = Interlocked.Exchange(ref _device, null);
        var ch = Interlocked.Exchange(ref _characteristic, null);
        if (ch != null) { try { ch.ValueChanged -= OnValueChanged; } catch { } }
        if (dev != null)
        {
            try { dev.ConnectionStatusChanged -= OnConnectionChanged; } catch { }
            try { dev.Dispose(); } catch { }
        }
        await Task.CompletedTask;
    }

    internal void ForceReconnect()
    {
        Log("bang: reconnect");
        _hasTargetAddr = _fixedMac.HasValue;   // 重新扫描（固定 MAC 除外）
        _targetAddr = _fixedMac.GetValueOrDefault();
        _ = Task.Run(async () => { await TeardownAsync().ConfigureAwait(false); });
    }

    // ---- 快照供 measure 读取 ----
    internal (double temp, double hum, double volt, DateTime last, string mac, int phase, bool ever) Snapshot()
    {
        lock (_state)
            return (_temp, _hum, _volt, _lastSample, _macText, _phase, _everConnected);
    }

    private void SetPhase(int p)
    {
        lock (_state) SetPhaseLocked(p);
    }

    private void SetPhaseLocked(int p)
    {
        _phase = p;
        if (p == 1 && !_everConnected && _phase == 1) { /* keep */ }
    }

    private static string MacText(ulong a) =>
        a.ToString("X12").Insert(10, ":").Insert(8, ":").Insert(6, ":").Insert(4, ":").Insert(2, ":");

    internal static void Log(string msg)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(LogFile));
            File.AppendAllText(LogFile,
                DateTime.Now.ToString("HH:mm:ss.fff") + "  " + msg + Environment.NewLine);
        }
        catch { }
    }

    private async Task SafeAsync(string tag, Func<Task> body)
    {
        try { await body().ConfigureAwait(false); }
        catch (Exception ex) { Log(tag + ": " + ex); }
    }
}
