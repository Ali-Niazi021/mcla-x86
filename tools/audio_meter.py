"""audio_meter.py - log the peak level of mcla.exe's audio session (and of the default render endpoint) with wall-clock times.

    python -I audio_meter.py out.csv [seconds] [interval_ms]

Pure ctypes (WASAPI IAudioMeterInformation); no packages. Used to see whether the game answers a pad button with a sound (a horn).
The columns are: wall clock (HH:MM:SS.mmm, the log's clock), the endpoint peak 0..1, the peak of the session(s) of mcla.exe 0..1
(-1 = the game has no audio session), the number of sessions of mcla.exe.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time
import datetime

ole32 = ctypes.WinDLL("ole32")
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, wt.DWORD]


class GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]


def guid(s):
    import uuid
    u = uuid.UUID(s)
    g = GUID()
    g.a, g.b, g.c = u.time_low, u.time_mid, u.time_hi_version
    for i, byte in enumerate(u.bytes[8:]):
        g.d[i] = byte
    return g


CLSID_MMDeviceEnumerator = guid("BCDE0395-E52F-467C-8E3D-C4579291692E")
IID_IMMDeviceEnumerator = guid("A95664D2-9614-4F35-A746-DE8DB63617E6")
IID_IAudioMeterInformation = guid("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IID_IAudioSessionManager2 = guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")
IID_IAudioSessionControl2 = guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")


def vcall(obj, index, restype, *argtypes):
    """Call method `index` of a COM object pointer (a c_void_p value) with the given ctypes args (pairs: type, value)."""
    vtbl = ctypes.cast(ctypes.c_void_p(obj), ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    fn = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *[t for t, _ in argtypes])(vtbl[index])
    return fn(obj, *[v for _, v in argtypes])


def release(obj):
    if obj:
        vcall(obj, 2, ctypes.c_ulong)


def check(hr, what):
    if hr != 0:
        raise OSError("%s failed: 0x%08X" % (what, hr & 0xFFFFFFFF))


def process_name(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(520)
        n = wt.DWORD(520)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return buf.value.rsplit("\\", 1)[-1].lower()
        return ""
    finally:
        kernel32.CloseHandle(h)


def main():
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
    interval = float(sys.argv[3]) / 1000.0 if len(sys.argv) > 3 else 0.05
    ole32.CoInitializeEx(None, 0)
    enum = ctypes.c_void_p()
    check(ole32.CoCreateInstance(ctypes.byref(CLSID_MMDeviceEnumerator), None, 1, ctypes.byref(IID_IMMDeviceEnumerator), ctypes.byref(enum)),
          "CoCreateInstance(MMDeviceEnumerator)")
    dev = ctypes.c_void_p()
    check(vcall(enum.value, 4, ctypes.c_long, (ctypes.c_int, 0), (ctypes.c_int, 1), (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(dev))),
          "GetDefaultAudioEndpoint")  # eRender, eMultimedia
    meter = ctypes.c_void_p()
    check(vcall(dev.value, 3, ctypes.c_long, (ctypes.POINTER(GUID), ctypes.byref(IID_IAudioMeterInformation)), (wt.DWORD, 1),
                (ctypes.c_void_p, None), (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(meter))), "Activate(meter)")
    mgr = ctypes.c_void_p()
    check(vcall(dev.value, 3, ctypes.c_long, (ctypes.POINTER(GUID), ctypes.byref(IID_IAudioSessionManager2)), (wt.DWORD, 1),
                (ctypes.c_void_p, None), (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(mgr))), "Activate(session manager)")
    pid_names = {}
    t_end = time.time() + seconds
    with open(out, "w") as f:
        f.write("time,endpoint,game,sessions\n")
        while time.time() < t_end:
            ep = ctypes.c_float(0.0)
            vcall(meter.value, 3, ctypes.c_long, (ctypes.POINTER(ctypes.c_float), ctypes.byref(ep)))
            game, nsess = -1.0, 0
            senum = ctypes.c_void_p()
            if vcall(mgr.value, 5, ctypes.c_long, (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(senum))) == 0:
                count = ctypes.c_int(0)
                vcall(senum.value, 3, ctypes.c_long, (ctypes.POINTER(ctypes.c_int), ctypes.byref(count)))
                for i in range(count.value):
                    ctl = ctypes.c_void_p()
                    if vcall(senum.value, 4, ctypes.c_long, (ctypes.c_int, i), (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(ctl))) != 0:
                        continue
                    ctl2 = ctypes.c_void_p()
                    if vcall(ctl.value, 0, ctypes.c_long, (ctypes.POINTER(GUID), ctypes.byref(IID_IAudioSessionControl2)),
                             (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(ctl2))) == 0:
                        pid = wt.DWORD(0)
                        vcall(ctl2.value, 14, ctypes.c_long, (ctypes.POINTER(wt.DWORD), ctypes.byref(pid)))
                        if pid.value not in pid_names:
                            pid_names[pid.value] = process_name(pid.value)
                        if pid_names[pid.value] == "mcla.exe":
                            m = ctypes.c_void_p()
                            if vcall(ctl.value, 0, ctypes.c_long, (ctypes.POINTER(GUID), ctypes.byref(IID_IAudioMeterInformation)),
                                     (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(m))) == 0:
                                pk = ctypes.c_float(0.0)
                                vcall(m.value, 3, ctypes.c_long, (ctypes.POINTER(ctypes.c_float), ctypes.byref(pk)))
                                game = max(game, pk.value)
                                nsess += 1
                                release(m.value)
                        release(ctl2.value)
                    release(ctl.value)
                release(senum.value)
            f.write("%s,%.4f,%.4f,%d\n" % (datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3], ep.value, game, nsess))
            time.sleep(interval)
    print("done", out)


if __name__ == "__main__":
    main()
