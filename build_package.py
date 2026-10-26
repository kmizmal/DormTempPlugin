#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 DormTempMonitor 皮肤与 DormTemp 插件打包成可直接双击安装的 .rmskin。

做的事：
  1. 把 skins/ 下的 UTF-8 源复制到 dist/，转成 **UTF-16 LE + BOM + CRLF**
     （Rainmeter 只认这个编码；LF 会让新插入的段落粘到上一行整段不解析）；
  2. 生成 RMSKIN.ini 元数据；
  3. 把 DLL 放进包内 Plugins/64bit/ —— 安装器会按宿主位数自动装到插件目录；
  4. 压缩并追加 16 字节 PackageFooter（少了它安装器直接拒绝这个包）；
  5. 自检：footer 魔数、Load 指向的皮肤在包里、插件确实是 x64；
  6. 可选 --install：直接投影到本机（皮肤 + 两个插件目录）。

用法：
    python build_plugin.py             # 先构建插件
    python build_package.py            # 打包
    python build_package.py --install  # 打包并安装到本机 Rainmeter
"""

from __future__ import annotations

import argparse
import shutil
import struct
import zipfile
from pathlib import Path

CONFIG = "DormTempMonitor"
PLUGIN_NAME = "DormTemp"
UTF16_SUFFIXES = {".ini", ".inc"}

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "skins" / CONFIG


def skin_version() -> str:
    """从皮肤 [Metadata] 里取 Version=，避免打包脚本和皮肤版本号各写一份。"""
    for line in (SRC / f"{CONFIG}.ini").read_text(encoding="utf-8-sig").splitlines():
        if line.strip().lower().startswith("version="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"皮肤里找不到 Version=：{SRC / (CONFIG + '.ini')}")


VERSION = skin_version()
PACKAGE_NAME = f"DormTempMonitor_{VERSION}"
DIST = ROOT / "dist"
PLUGIN_DLL = DIST / "plugin" / f"{PLUGIN_NAME}.dll"
SKINS_DIR = Path.home() / "Documents" / "Rainmeter" / "Skins" / CONFIG
INSTALL_PLUGINS = Path.home() / "AppData" / "Local" / "Programs" / "Rainmeter" / "Plugins"
USER_PLUGINS = Path.home() / "AppData" / "Roaming" / "Rainmeter" / "Plugins"

RMSKIN_INI = """[rmskin]
Name=DormTemp Monitor
Author=WorkBuddy
Version={version}
LoadType=Skin
Load={config}\\{config}.ini
MinimumRainmeter=4.5
MinimumWindows=10
""".format(config=CONFIG, version=VERSION)


def stage() -> Path:
    target = DIST / CONFIG
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(SRC, target, ignore=shutil.ignore_patterns("*.tmp", "__pycache__"))

    for path in target.rglob("*"):
        if path.is_file() and path.suffix.lower() in UTF16_SUFFIXES:
            text = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
            # newline=None 时 open() 会把 \n 转成 \r\n，得到 Rainmeter 要求的 CRLF
            path.write_text(text, encoding="utf-16")
    return target


def build_package(config_dir: Path) -> Path:
    if not PLUGIN_DLL.exists():
        raise SystemExit(f"缺少插件 DLL：{PLUGIN_DLL}\n先运行 python build_plugin.py 生成。")

    DIST.mkdir(parents=True, exist_ok=True)
    package = DIST / (PACKAGE_NAME + ".rmskin")
    if package.exists():
        package.unlink()

    meta = DIST / "RMSKIN.ini"
    meta.write_text(RMSKIN_INI, encoding="utf-8")

    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(meta, "RMSKIN.ini")
        # 安装器按宿主位数选目录（IsWin32Build() ? "32bit" : "64bit"），
        # 直接放 Plugins/ 根目录不会被识别。
        z.write(PLUGIN_DLL, f"Plugins/64bit/{PLUGIN_NAME}.dll")
        for path in sorted(config_dir.rglob("*")):
            if path.is_file():
                z.write(path, f"Skins/{CONFIG}/{path.relative_to(config_dir).as_posix()}")
    meta.unlink()

    # .rmskin 不是普通 zip：末尾要追加 16 字节 PackageFooter，否则安装器按
    # strcmp(key, "RMSKIN") 判定失败。
    #   struct PackageFooter { __int64 size; BYTE flags; char key[7]; };
    this_size = package.stat().st_size
    with package.open("ab") as f:
        f.write(struct.pack("<qB7s", this_size, 0, b"RMSKIN\0"))
    return package


def verify_package(package: Path) -> None:
    raw = package.read_bytes()
    size, flags, key = struct.unpack("<qB7s", raw[-16:])
    if key.rstrip(b"\x00") != b"RMSKIN":
        raise SystemExit(f"footer 魔数错误：{key!r}")
    if size != len(raw) - 16:
        raise SystemExit(f"footer.size={size} 与 zip 实际大小 {len(raw) - 16} 不符")
    if flags != 0:
        raise SystemExit(f"footer.flags 应为 0，实际 {flags}")

    with zipfile.ZipFile(package) as z:
        names = z.namelist()
        if "RMSKIN.ini" not in names:
            raise SystemExit("包内缺少 RMSKIN.ini")

        meta = z.read("RMSKIN.ini").decode("utf-8")
        load = [ln for ln in meta.splitlines() if ln.lower().startswith("load=")]
        if not load:
            raise SystemExit("RMSKIN.ini 缺少 Load= 键")
        rel = load[0].split("=", 1)[1].strip().replace("\\", "/")
        if f"Skins/{rel}" not in names:
            raise SystemExit(f"Load 指向的皮肤不在包里：Skins/{rel}")

        plugins = [n for n in names if n.startswith("Plugins/64bit/") and n.endswith(".dll")]
        if not plugins:
            raise SystemExit("缺少 Plugins/64bit/*.dll")

        dll = z.read(plugins[0])
        pe = struct.unpack_from("<I", dll, 0x3C)[0]
        machine = struct.unpack_from("<H", dll, pe + 4)[0]
        if machine != 0x8664:
            raise SystemExit(f"包内插件不是 x64（machine=0x{machine:04X}）")

        # 皮肤必须真的是 UTF-16 LE + CRLF
        skin = z.read(f"Skins/{CONFIG}/{CONFIG}.ini")
        if not skin.startswith(b"\xff\xfe"):
            raise SystemExit("皮肤不是 UTF-16 LE + BOM")
        if b"\r\x00\n\x00" not in skin:
            raise SystemExit("皮肤没有 CRLF 换行")

    print(f"包校验通过：footer OK，{len(names)} 个条目，插件 {plugins[0]}（x64），皮肤 UTF-16 LE + CRLF")


def install(config_dir: Path) -> None:
    """覆盖式安装：不删目录（Rainmeter 可能正占用），逐文件覆盖后清掉残留。"""
    SKINS_DIR.mkdir(parents=True, exist_ok=True)

    for src in config_dir.rglob("*"):
        if not src.is_file():
            continue
        dst = SKINS_DIR / src.relative_to(config_dir)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    for dst in sorted(SKINS_DIR.rglob("*"), reverse=True):
        if dst.is_file() and not (config_dir / dst.relative_to(SKINS_DIR)).exists():
            try:
                dst.unlink()
            except OSError:
                pass
    print(f"已安装皮肤：{SKINS_DIR}")

    for folder in (INSTALL_PLUGINS, USER_PLUGINS):
        if not folder.exists():
            continue
        target = folder / f"{PLUGIN_NAME}.dll"
        try:
            shutil.copy2(PLUGIN_DLL, target)
            print(f"已安装插件：{target}")
        except PermissionError:
            print(f"！插件被占用，未覆盖：{target}")
            print("  退出 Rainmeter 后再运行一次 --install，或手动替换。")


def main() -> int:
    parser = argparse.ArgumentParser(description="打包 DormTempMonitor 皮肤与插件")
    parser.add_argument("--install", action="store_true", help="同时安装到本机 Rainmeter")
    args = parser.parse_args()

    config_dir = stage()
    package = build_package(config_dir)
    verify_package(package)
    print(f"已生成：{package}  ({package.stat().st_size / 1024:.0f} KB)")

    if args.install:
        install(config_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
