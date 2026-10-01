"""Capture a window by title substring via PrintWindow (Windows, any DPI/monitor).
Usage: winshot.py TITLE OUT.png"""
import ctypes, sys
from ctypes import wintypes
from PIL import Image
u, g = ctypes.windll.user32, ctypes.windll.gdi32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    u.SetProcessDPIAware()
found = []
@ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(h, _):
    n = u.GetWindowTextLengthW(h)
    if n and u.IsWindowVisible(h):
        b = ctypes.create_unicode_buffer(n + 1); u.GetWindowTextW(h, b, n + 1)
        if sys.argv[1] in b.value: found.append(h)
    return True
u.EnumWindows(cb, 0)
h = found[0]
r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
w, hh = r.right - r.left, r.bottom - r.top
hdc = u.GetWindowDC(h); mdc = g.CreateCompatibleDC(hdc)
bmp = g.CreateCompatibleBitmap(hdc, w, hh); g.SelectObject(mdc, bmp)
u.PrintWindow(h, mdc, 2)
class BIH(ctypes.Structure):
    _fields_ = [('biSize', ctypes.c_uint32), ('biWidth', ctypes.c_int32), ('biHeight', ctypes.c_int32),
                ('biPlanes', ctypes.c_uint16), ('biBitCount', ctypes.c_uint16), ('biCompression', ctypes.c_uint32),
                ('a', ctypes.c_uint32), ('b', ctypes.c_int32), ('c', ctypes.c_int32), ('d', ctypes.c_uint32), ('e', ctypes.c_uint32)]
bi = BIH(ctypes.sizeof(BIH), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
buf = ctypes.create_string_buffer(w * hh * 4)
g.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
Image.frombuffer('RGBA', (w, hh), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(sys.argv[2])
print(w, hh)
