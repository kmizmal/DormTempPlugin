"""抓取 Rainmeter 皮肤窗口的离屏画面（PrintWindow），不受前台游戏遮挡影响。

用法：python grab_skin.py <输出png> <宽> <高> [标题关键字]
按尺寸匹配窗口，命中第一个就截图；给了标题关键字则再按标题过滤（例如 ChartDay）。
找不到时打印候选清单便于排查。
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.PrintWindow.restype = wt.BOOL


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
        ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


def windows():
    out = []

    def cb(hwnd, _):
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        if w > 60 and h > 60:
            c = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, c, 256)
            t = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, t, 512)
            out.append((hwnd, w, h, r.left, r.top, c.value, t.value))
        return True

    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb)
    user32.EnumWindows(proc, 0)
    return out


def grab(hwnd, w, h, path):
    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)

    ok = user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h          # 负值 = 自上而下
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    # 直接写 32 位 BMP（负高度 = 自上而下），不依赖 Pillow
    pixels = buf.raw
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + pixels)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return ok, None


def main():
    out = sys.argv[1]
    want_w = int(sys.argv[2])
    want_h = int(sys.argv[3])
    needle = sys.argv[4].lower() if len(sys.argv) > 4 else None

    cands = windows()
    exact = [c for c in cands
             if abs(c[1] - want_w) <= 2 and abs(c[2] - want_h) <= 2
             and (not needle or needle in c[6].lower())]
    if not exact:
        print("没找到尺寸匹配的窗口，候选：")
        for c in cands:
            if 150 <= c[1] <= 400 and 60 <= c[2] <= 500:
                print("  ", c[1], c[2], c[3], c[4], c[5], c[6][-50:])
        return 1

    hwnd, w, h, *_ = exact[0]
    ok, img = grab(hwnd, w, h, out)
    print(f"PrintWindow={ok} size={w}x{h} saved={out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
