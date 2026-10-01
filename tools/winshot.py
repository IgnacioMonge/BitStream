"""Capture a window by title substring (Windows). Usage: winshot.py TITLE OUT.png"""
import ctypes, sys
from ctypes import wintypes
from PIL import ImageGrab
u = ctypes.windll.user32
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
r = wintypes.RECT(); u.GetWindowRect(found[0], ctypes.byref(r))
vx, vy = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
im = ImageGrab.grab(all_screens=True)
im.crop((r.left - vx, r.top - vy, r.right - vx, r.bottom - vy)).save(sys.argv[2])
print(r.left, r.top, r.right, r.bottom)
