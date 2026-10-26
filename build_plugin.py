#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""构建 DormTemp Rainmeter 插件（x64 单 DLL）。

流程：
    zig c++   ->  DormTemp.dll（C++/WinRT 静态链接，无 .NET / Python 依赖）
    读 PE     ->  校验 machine / 导出表 / 导入表
    可选      ->  跑测试、装进本机两个插件目录

为什么用 zig 当编译器：
    本机没有 MSVC（无 cl.exe）也没有 Windows SDK；zig 自带的 clang + mingw 头
    足够编 C++/WinRT 投影头（配合 RoShim.cpp 补 combase 导入库的缺口）。

注意：
    · 产物必须 x64：当前 Rainmeter 是 64 位，32 位宿主会静默加载失败
    · 导出名就是 Rainmeter 的调用入口，改名等于改 ABI
    · 插件文件名必须与皮肤里的 Plugin= 一致（DormTemp）
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src" / "DormTemp"
WINRT_GEN = ROOT / "winrt_gen"
DIST_PLUGIN = ROOT / "dist" / "plugin"
PLUGIN_NAME = "DormTemp"

# Rainmeter 加载插件的搜索顺序：安装目录 Plugins 优先，其次用户插件目录。
# 两处都存在同名 DLL 时，实际加载哪个取决于顺序 —— 只改用户目录会「改了没生效」。
INSTALL_PLUGINS = Path.home() / "AppData" / "Local" / "Programs" / "Rainmeter" / "Plugins"
USER_PLUGINS = Path.home() / "AppData" / "Roaming" / "Rainmeter" / "Plugins"

EXPORTS = ["Initialize", "Reload", "Update", "Finalize", "GetString", "ExecuteBang"]

# 这些运行库一旦出现在导入表里，就说明产物不是静态链接，分发会缺 DLL
FORBIDDEN_IMPORTS = (
    "msvcp", "vcruntime", "msvcr", "libstdc++", "libgcc", "libwinpthread",
)

PLUGIN_SOURCES = ["Plugin.cpp", "BleSession.cpp", "SharedState.cpp",
                  "DormProtocol.cpp", "Config.cpp", "Store.cpp", "Stats.cpp",
                  "Chart.cpp", "Log.cpp", "RainmeterApi.cpp", "RoShim.cpp"]

TEST_SOURCES = ["DormProtocol.cpp", "SharedState.cpp", "Config.cpp", "Store.cpp",
                "Stats.cpp", "Chart.cpp"]

# 探测工具和插件共用同一套采集/解析代码，只是少了 Rainmeter 导出层
PROBE_SOURCES = ["BleSession.cpp", "SharedState.cpp", "DormProtocol.cpp",
                 "Config.cpp", "Store.cpp", "Stats.cpp", "Chart.cpp", "Log.cpp", "RoShim.cpp"]


def find_zig() -> str:
    env = os.environ.get("ZIG")
    if env and Path(env).exists():
        return env
    candidates = [
        ROOT.parent / "tools" / "zig" / "zig.exe",
        ROOT.parent / "tools" / "zig" / "zig",
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    found = shutil.which("zig") or shutil.which("zig.exe")
    if found:
        return found
    raise SystemExit("找不到 zig：请设置环境变量 ZIG，或放到 ../tools/zig/zig.exe")


def run(cmd: list[str]) -> None:
    print("  $", " ".join(str(c) for c in cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        print(proc.stdout or "")
        print(proc.stderr or "")
        raise SystemExit(f"命令失败（{proc.returncode}）：{Path(cmd[0]).name}")
    if proc.stdout.strip():
        print(proc.stdout.strip())


def read_pe(path: Path) -> tuple[int, list[str], list[str]]:
    """返回 (machine, 导出名, 导入的 DLL 名)。"""
    data = path.read_bytes()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, e_lfanew + 4)[0]
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
    data_dir = e_lfanew + 24 + (112 if magic == 0x20B else 96)
    section = e_lfanew + 24 + opt_size

    def to_offset(rva: int) -> int | None:
        for i in range(nsec):
            off = section + i * 40
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        return None

    def read_name(rva: int) -> str:
        off = to_offset(rva)
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    exports: list[str] = []
    exp_rva = struct.unpack_from("<I", data, data_dir)[0]
    if exp_rva:
        exp = to_offset(exp_rva)
        count = struct.unpack_from("<I", data, exp + 24)[0]
        names_rva = struct.unpack_from("<I", data, exp + 32)[0]
        names = to_offset(names_rva)
        for i in range(count):
            exports.append(read_name(struct.unpack_from("<I", data, names + i * 4)[0]))

    imports: list[str] = []
    imp_rva = struct.unpack_from("<I", data, data_dir + 8)[0]
    if imp_rva:
        off = to_offset(imp_rva)
        while True:
            desc = struct.unpack_from("<IIIII", data, off)
            if not any(desc):
                break
            imports.append(read_name(desc[3]))
            off += 20

    return machine, exports, imports


def build_plugin(zig: str) -> Path:
    print("[1/3] zig c++ 编译插件")
    DIST_PLUGIN.mkdir(parents=True, exist_ok=True)
    out = DIST_PLUGIN / f"{PLUGIN_NAME}.dll"
    if out.exists():
        out.unlink()

    cmd = [
        zig, "c++", "-target", "x86_64-windows-gnu",
        "-std=c++20", "-fms-extensions", "-O2", "-shared",
        "-I", str(WINRT_GEN), "-I", str(SRC),
        "-o", str(out),
    ]
    cmd += [str(SRC / name) for name in PLUGIN_SOURCES]
    cmd += ["-lole32", "-loleaut32"]
    run(cmd)
    if not out.exists():
        raise SystemExit("编译未产出 DLL")
    return out


def verify_plugin(dll: Path) -> None:
    print("[2/3] 校验 PE")
    machine, exports, imports = read_pe(dll)
    print(f"      machine=0x{machine:04X}  导出={exports}")
    print(f"      导入={imports}")

    if machine != 0x8664:
        raise SystemExit(f"不是 x64 产物（machine=0x{machine:04X}）")

    missing = [n for n in EXPORTS if n not in exports]
    if missing:
        raise SystemExit(f"缺少导出：{', '.join(missing)}")

    # mingw 的 ld 会把全局符号一并导出（unw_* 之类），Rainmeter 只按名字查表，
    # 多出来的不影响加载；这里只提示数量，不作为失败条件。
    extra = [n for n in exports if n not in EXPORTS]
    if extra:
        print(f"      另附带 {len(extra)} 个内部符号（mingw 默认行为，不影响加载）")

    bad = [i for i in imports if any(f in i.lower() for f in FORBIDDEN_IMPORTS)]
    if bad:
        raise SystemExit(f"存在未静态链接的运行时依赖：{', '.join(bad)}")

    print(f"      体积 {dll.stat().st_size / 1024:.0f} KB")


def run_tests(zig: str) -> None:
    print("[3/3] 构建并运行测试")
    build = ROOT / "tests" / "build"
    build.mkdir(parents=True, exist_ok=True)
    exe = build / "ProtocolTests.exe"

    cmd = [
        zig, "c++", "-target", "x86_64-windows-gnu", "-std=c++20", "-O2",
        "-I", str(SRC), "-o", str(exe),
    ]
    cmd += [str(SRC / name) for name in TEST_SOURCES]
    cmd.append(str(ROOT / "tests" / "ProtocolTests.cpp"))
    run(cmd)

    print()
    proc = subprocess.run([str(exe)], cwd=str(build))
    if proc.returncode != 0:
        raise SystemExit(f"测试未全绿（退出码 {proc.returncode}）")


def build_probe(zig: str) -> Path:
    print("[probe] 构建 DormProbe.exe")
    DIST_PLUGIN.parent.mkdir(parents=True, exist_ok=True)
    exe = DIST_PLUGIN.parent / "DormProbe.exe"

    cmd = [
        zig, "c++", "-target", "x86_64-windows-gnu", "-std=c++20",
        "-fms-extensions", "-O2", "-municode",
        "-I", str(WINRT_GEN), "-I", str(SRC), "-o", str(exe),
    ]
    cmd += [str(SRC / name) for name in PROBE_SOURCES]
    cmd.append(str(ROOT / "tools" / "DormProbe.cpp"))
    cmd += ["-lole32", "-loleaut32"]
    run(cmd)
    print(f"      {exe}")
    return exe


def install(dll: Path) -> None:
    for folder in (INSTALL_PLUGINS, USER_PLUGINS):
        if not folder.exists():
            continue
        target = folder / f"{PLUGIN_NAME}.dll"
        try:
            shutil.copy2(dll, target)
            print(f"已安装：{target}")
        except PermissionError:
            print(f"！DLL 被占用，未覆盖：{target}")
            print("  退出 Rainmeter 后再运行一次 --install，或手动替换。")


def main() -> int:
    parser = argparse.ArgumentParser(description="构建 DormTemp Rainmeter 插件")
    parser.add_argument("--install", action="store_true", help="构建后复制到本机两个插件目录")
    parser.add_argument("--test", action="store_true", help="构建后运行测试集")
    parser.add_argument("--probe", action="store_true", help="一并构建探测工具 DormProbe.exe")
    args = parser.parse_args()

    zig = find_zig()
    dll = build_plugin(zig)
    verify_plugin(dll)

    if args.test:
        run_tests(zig)

    if args.probe:
        build_probe(zig)

    if args.install:
        install(dll)

    print(f"\n产物：{dll}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
