# _legacy —— 两代旧实现，仅作参考

这里的代码**不参与构建**（`build_plugin.py` 的源列表里没有它们），保留原因只有一个：
它们记录了插件从「Python + 边车进程」走到「单 DLL」的过程，里面踩过的坑
（hostfxr 崩溃、命名共享内存布局、CSV 落盘）在 `AGENTS.md` 与 Obsidian 笔记里被反复引用。

| 目录 | 是什么 | 为什么放弃 |
| --- | --- | --- |
| `v2-dotnet-service/` | `shim.c`（C 垫片）+ .NET 8 边车 `DormTempService.exe` + 命名共享内存 | 交付 34 MB；`hostfxr` 对所有自定义宿主 AV（`0xC0000005 @ hostfxr+0xb82e`） |
| `v3-singledll/` | 第一版单 `DormTemp.dll`（C++/WinRT），采样写 CSV | 已被 `src/DormTemp/` 取代；CSV 换 SQLite 后逻辑全删 |

当前实现见 `src/DormTemp/`。
