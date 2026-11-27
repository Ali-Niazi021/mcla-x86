"""inject.py - send keyboard and mouse input to the running mcla.exe window on a timeline (T7 overlay tests).

    python inject.py <script.txt> [--log out.log]

Waits for a visible window titled "mcla_x86*" of a process named mcla.exe, then runs the script on the process' own clock
(seconds since the process was created: the same clock as run_game's flow and schedule, so steps line up with them).

Script lines (# comments):
    <t> key <name> [hold_ms]       tap a key (down, hold, up); hold default 80 ms
    <t> down <name>                key down only        <t> up <name>   key up only
    <t> move <x> <y>               mouse to client pixel (x, y) of the game window (screenshot coordinates of the 3200x2000 capture)
    <t> click <x> <y> [button]     move, then click (left default; "right")
    <t> mdown <x> <y>              move, then left button down        <t> mup   left button up
    <t> rel <dx> <dy> [steps] [ms] relative mouse motion (SendInput MOUSEEVENTF_MOVE), `steps` pieces over `ms` milliseconds
    <t> wheel <delta>              mouse wheel, 120 = one notch up
    <t> mark <text>                a line in the log
Key names: A-Z, 0-9, F1-F12, RETURN, BACK, TAB, SPACE, ESC, UP, DOWN, LEFT, RIGHT, SHIFT, CTRL, ALT.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_KEYUP, KEYEVENTF_EXTENDEDKEY = 0x2, 0x1
MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x1, 0x2, 0x4
MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_WHEEL = 0x8, 0x10, 0x800
MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_VIRTUALDESK = 0x8000, 0x4000


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


VK = {"RETURN": 0x0D, "BACK": 0x08, "TAB": 0x09, "SPACE": 0x20, "ESC": 0x1B, "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25,
      "RIGHT": 0x27, "SHIFT": 0x10, "CTRL": 0x11, "ALT": 0x12}
for i in range(1, 13):
    VK["F%d" % i] = 0x6F + i
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789":
    VK[c] = ord(c)
EXTENDED = {0x26, 0x28, 0x25, 0x27}


def send(inp):
    n = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if n != 1:
        raise OSError("SendInput failed: %d" % ctypes.get_last_error())


def key_event(vk, up):
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = (KEYEVENTF_KEYUP if up else 0) | (KEYEVENTF_EXTENDEDKEY if vk in EXTENDED else 0)
    inp = INPUT(type=INPUT_KEYBOARD)
    inp.ki = KEYBDINPUT(vk, scan, flags, 0, 0)
    send(inp)


def mouse_event(flags, dx=0, dy=0, data=0):
    inp = INPUT(type=INPUT_MOUSE)
    inp.mi = MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
    send(inp)


def find_window():
    found = []
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if buf.value.startswith("mcla_x86"):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found.append((hwnd, pid.value))
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return found[0] if found else None


def process_age_s(pid):
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    kernel32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    kernel32.CloseHandle(h)
    created = (c.dwHighDateTime << 32) | c.dwLowDateTime
    now = wt.FILETIME()
    kernel32.GetSystemTimeAsFileTime(ctypes.byref(now))
    return (((now.dwHighDateTime << 32) | now.dwLowDateTime) - created) / 1e7


def client_to_screen(hwnd, x, y):
    # The capture is the client area at physical pixels; the process is per-monitor DPI aware (the game's manifest), this one is
    # made so below, so ClientToScreen works in physical pixels too.
    pt = wt.POINT(int(x), int(y))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def move_abs(hwnd, x, y):
    sx, sy = client_to_screen(hwnd, x, y)
    w = user32.GetSystemMetrics(78)  # SM_CXVIRTUALSCREEN
    h = user32.GetSystemMetrics(79)
    ox = user32.GetSystemMetrics(76)
    oy = user32.GetSystemMetrics(77)
    ax = int((sx - ox) * 65535 / max(1, w - 1))
    ay = int((sy - oy) * 65535 / max(1, h - 1))
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, ax, ay)


def bring_front(hwnd):
    """The game window must be the foreground window before anything is sent (input goes to whatever is in front)."""
    if user32.GetForegroundWindow() == hwnd:
        return True
    for _ in range(4):
        # An Alt tap lets this process take the foreground (the usual foreground-lock workaround), then ask for it.
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0)
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        user32.SetForegroundWindow(hwnd)
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.15)
        if user32.GetForegroundWindow() == hwnd:
            return True
    return False


def main():
    path = sys.argv[1]
    log_path = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else None
    logf = open(log_path, "w", encoding="utf-8") if log_path else None

    def log(msg, pid=None):
        line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        if logf:
            logf.write(line + "\n")
            logf.flush()

    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        user32.SetProcessDPIAware()
    steps = []
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        steps.append((float(parts[0]), parts[1].lower(), parts[2:]))
    steps.sort(key=lambda s: s[0])
    log("waiting for the game window")
    win = None
    while win is None:
        win = find_window()
        time.sleep(0.2)
    hwnd, pid = win
    log("window %#x pid %d" % (hwnd, pid))
    for t, cmd, args in steps:
        while True:
            age = process_age_s(pid)
            if age is None:
                log("the process is gone")
                return 1
            if age >= t:
                break
            time.sleep(min(0.05, t - age))
        if cmd != "mark" and not bring_front(hwnd):
            fg = user32.GetForegroundWindow()
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(fg, buf, 256)
            log("t=%.2f %s SKIPPED: the game window is not in front (foreground %#x '%s')" % (t, cmd, fg or 0, buf.value))
            continue
        if cmd == "key":
            vk = VK[args[0].upper()]
            hold = float(args[1]) / 1000 if len(args) > 1 else 0.08
            key_event(vk, False)
            time.sleep(hold)
            key_event(vk, True)
        elif cmd == "down":
            key_event(VK[args[0].upper()], False)
        elif cmd == "up":
            key_event(VK[args[0].upper()], True)
        elif cmd == "move":
            move_abs(hwnd, float(args[0]), float(args[1]))
        elif cmd == "click":
            move_abs(hwnd, float(args[0]), float(args[1]))
            time.sleep(0.15)
            right = len(args) > 2 and args[2].lower() == "right"
            mouse_event(MOUSEEVENTF_RIGHTDOWN if right else MOUSEEVENTF_LEFTDOWN)
            time.sleep(0.1)
            mouse_event(MOUSEEVENTF_RIGHTUP if right else MOUSEEVENTF_LEFTUP)
        elif cmd == "mdown":
            move_abs(hwnd, float(args[0]), float(args[1]))
            time.sleep(0.1)
            mouse_event(MOUSEEVENTF_LEFTDOWN)
        elif cmd == "mup":
            mouse_event(MOUSEEVENTF_LEFTUP)
        elif cmd == "rel":
            dx, dy = int(args[0]), int(args[1])
            n = int(args[2]) if len(args) > 2 else 20
            ms = float(args[3]) if len(args) > 3 else 1000.0
            for i in range(n):
                mouse_event(MOUSEEVENTF_MOVE, dx // n, dy // n)
                time.sleep(ms / 1000.0 / n)
        elif cmd == "wheel":
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, int(args[0]))
        elif cmd == "mark":
            pass
        else:
            log("unknown command %s" % cmd)
        log("t=%.2f %s %s (process age %.2f s)" % (t, cmd, " ".join(args), process_age_s(pid) or -1))
    log("script done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
