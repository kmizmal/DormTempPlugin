---
name: dormtemp-ble-rainmeter-plugin
description: 维护 DormTemp（米家温湿度计 3 mini 的 Rainmeter 插件，C++/WinRT 直读 BLE + SQLite 落盘）的工程约定。应在修改该项目的插件逻辑、皮肤、构建/打包脚本，或排查「插件加载失败 / 改了没生效 / 扫不到设备 / 数据不入库」时使用。
agent_created: true
---

# DormTemp 插件工程约定

一句话架构：**一个 x64 原生 DLL，在 Rainmeter 进程里起一个 MTA 采集线程，用 C++/WinRT
订阅 BLE GATT 通知，采样写 SQLite**。没有 .NET、没有 Python、没有外部进程、没有共享内存。

## 目录

```
src/DormTemp/        插件源码（分层，见 README 第二节）
skins/DormTempMonitor/  皮肤 UTF-8 源（打包时转 UTF-16 LE + BOM + CRLF）
  ├─ DormTempMonitor.ini  主面板（实时数据）
  ├─ @Resources/*.inc     共用变量 / 样式 / 卡片与曲线的 meter 定义
  └─ Cards/<名>/<名>.ini  6 张可选皮肤，各自独立 config 才能自由组合激活
tests/ProtocolTests.cpp 纯逻辑测试（不启蓝牙、不依赖 Rainmeter）
tools/DormProbe.cpp     探测工具：scan / read / db / chart（chart 直接出一张 PNG）
tools/grab_skin.py      离屏抓皮肤画面（按窗口尺寸匹配）
winrt_gen/          cppwinrt 生成的投影头（生成物）
build_plugin.py     构建 + PE 校验 + 可选测试/安装
build_package.py    打 .rmskin + 自检 + 可选安装
dist/               产物：plugin/DormTemp.dll、DormProbe.exe、*.rmskin
```

## 常用命令

```bash
python build_plugin.py --test --probe --install    # 改完代码的标准动作
python build_package.py --install                  # 改完皮肤/要出包时
dist/DormProbe.exe scan 8                          # 现场排查扫描
dist/DormProbe.exe read <MAC> 20                    # 现场排查通知
dist/DormProbe.exe chart "" 24                     # 单独渲染曲线，验证外观
```

## 硬性红线

1. **不要手改 `winrt_gen/`。** 它是 `cppwinrt.exe -in %WINDIR%\System32\WinMetadata -out winrt_gen`
   的产物；要重新生成就重跑那条命令，别在生成头里打补丁。
2. **协议偏移以真机通知为准。** 改 `DormProtocol.cpp` 前先用 `DormProbe.exe read` 抓一帧原始字节；
   注释写错一位会误导下一轮。
3. **改完必须 `--test`。** 189 项测试覆盖解析、文案、MAC、快照、落盘、区间聚合、分桶、坐标轴；不过就不算完成。
4. **皮肤只能通过 `build_package.py` 落地。** 手改 `Documents\Rainmeter\Skins` 里的 UTF-16 文件
   会和 `skins/` 源分叉，下次打包就被覆盖；编码/换行也容易坏（UTF-8 乱码、LF 段落不解析）。
5. **DLL 名必须等于皮肤里的 `Plugin=`（`DormTemp`），且必须是 x64。** 位数不匹配是静默失败。
6. **安装要写两个插件目录。** `%LOCALAPPDATA%\Programs\Rainmeter\Plugins` 优先，
   `%APPDATA%\Rainmeter\Plugins` 备用；只写一个就是经典的「改了没生效」。

## 模块边界

| 允许 | 禁止 |
| --- | --- |
| WinRT 调用只在 `BleSession.cpp` | 在 `Plugin.cpp` / `SharedState.cpp` 里直接调 WinRT |
| 纯逻辑放 `DormProtocol` / `SharedState` / `Config`（无 WinRT 依赖，可测） | 把协议解析塞进回调 lambda 里 |
| `Plugin.cpp` 只做导出与选项读取 | 在 `Update` 里做任何阻塞操作（蓝牙往返、SQLite 大查询） |
| 汇总数字由采集线程调 `Stats::refresh` 算，`Update` 只读缓存 | 在 `GetString`/`Update` 里临时查库（会被每帧调用几十次） |
| 曲线在 `Chart.cpp` 自己的线程里画（GDI+ 动态加载），`Update` 只返回路径 | 在 `Update`/`GetString` 里画图或调 `GdiplusStartup` |
| 曲线 PNG 文件名带 rev（`today_3.png`），路径一变 Rainmeter 才会重新载图 | 原地覆盖同名 PNG（Rainmeter 会继续显示缓存的那张） |
| `Store` 只认目录，不读全局配置 | 在 `Store` 里调 `Config`（循环依赖） |

## 故障排查顺序

1. **界面没变** → `Get-Process Rainmeter | %{ $_.Modules }` 看宿主实际加载的文件与哈希。
2. **扫不到设备** → `DormProbe.exe scan`；没有目标名时看是否按了传感器侧键（休眠不广播）。
3. **连上但无数据** → `DormProbe.exe read`；CCCD 写失败会退到 Indicate，两者都失败才报错。
4. **数据不入库** → `DormProbe.exe db` 看行数；再看日志里是否有 `storage: sqlite unavailable`。
5. **日志本身缺失** → 日志路径 = 皮肤 `DataPath`；为空才退 `%TEMP%`。
6. **看不到界面改了什么** → 前台是全屏游戏时截图只能拍到游戏；用
   `python tools/grab_skin.py out.bmp 225 113` 直接取皮肤窗口的离屏画面
   （窗口尺寸 = 皮肤尺寸 + 1，主面板 224×112 → 225×113）。
7. **皮肤没出来** → `!ActivateConfig <config> <file>` 一次只能让一个 config 活一个皮肤；
   同一文件夹里的多个 .ini 会被互相顶掉。可选皮肤必须各自一个子文件夹。

## 环境事实

- 本机没有 MSVC（无 `cl.exe`）与 Windows SDK，编译器用 `../tools/zig/zig.exe`（可用 `ZIG` 覆盖）。
- `winsqlite3.dll` 是系统组件（本机 3.43.2），**不要把 sqlite amalgamation 打进 DLL**。
- `zig` 链 DLL 默认导出全部全局符号（`unw_*` 等），Rainmeter 只按名字查表，不影响加载；
  用 `.def` 能收紧导出表，但会切到 lld-link 驱动，不值得。
