"""macOS-специфика для host/monitor.py: яркость, громкость, бездействие, медиа-клавиши.
Требует: pip install pyobjc-framework-Quartz pyobjc-framework-Cocoa
Медиа-клавиши требуют прав: Системные настройки -> Конфиденциальность -> Специальные возможности."""
import ctypes
import re
import subprocess
import threading

try:
    import Quartz
    HAS_QUARTZ = True
except ImportError:
    HAS_QUARTZ = False

try:
    from AppKit import NSEvent
    HAS_APPKIT = True
except ImportError:
    HAS_APPKIT = False

IDLE_INPUT_SECS = 120

NX_KEYTYPE_SOUND_UP = 0
NX_KEYTYPE_SOUND_DOWN = 1
NX_KEYTYPE_MUTE = 7
NX_KEYTYPE_PLAY = 16
NX_KEYTYPE_NEXT = 17
NX_KEYTYPE_PREVIOUS = 18
NX_KEYTYPE_BRIGHTNESS_UP = 21
NX_KEYTYPE_BRIGHTNESS_DOWN = 22

MEDIA_TOGGLE = 2
MEDIA_NEXT = 3
MEDIA_PREV = 4

_DS_FW = None
_DS_MISSING = object()


def _ds():
    global _DS_FW
    if _DS_FW is None:
        try:
            fw = ctypes.CDLL("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices")
            fw.DisplayServicesGetActiveDisplayID.restype = ctypes.c_uint32
            fw.DisplayServicesGetActiveDisplayID.argtypes = []
            fw.DisplayServicesGetBrightness.restype = ctypes.c_int
            fw.DisplayServicesGetBrightness.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)]
            _DS_FW = fw
        except Exception:  # noqa: BLE001
            _DS_FW = _DS_MISSING
    return None if _DS_FW is _DS_MISSING else _DS_FW


def read_brightness_pct():
    fw = _ds()
    if fw is None:
        return None
    try:
        did = fw.DisplayServicesGetActiveDisplayID()
        b = ctypes.c_float(0.0)
        if fw.DisplayServicesGetBrightness(did, ctypes.byref(b)) == 0:
            return max(0, min(100, round(b.value * 100)))
    except Exception:  # noqa: BLE001
        pass
    return None


def read_kbd_pct():
    try:
        out = subprocess.run(["ioreg", "-rc", "AppleHIDKeyboardEventDriver", "-l"],
                             capture_output=True, text=True, timeout=2).stdout
        for m in re.finditer(r'"Keyboard(Brightness|Backlight)"\s*=\s*(\d+)', out):
            v = int(m.group(2))
            return max(0, min(100, round(v * 100 / 255)))
    except Exception:  # noqa: BLE001
        pass
    return None


def volume_state():
    try:
        out = subprocess.run(["osascript", "-e", "get volume settings"],
                             capture_output=True, text=True, timeout=3).stdout
        m = re.search(r"output volume:(\d+)", out)
        if not m:
            return None
        pct = int(m.group(1))
        muted = "output muted:true" in out
        return pct, muted
    except Exception:  # noqa: BLE001
        return None


def screen_idle():
    if not HAS_QUARTZ:
        return False
    try:
        d = Quartz.CGSessionCopyCurrentDictionary()
        if d and d.get("CGSSessionScreenIsLocked") == "1":
            return True
    except Exception:  # noqa: BLE001
        pass
    try:
        if Quartz.CGDisplayIsAsleep():
            return True
    except Exception:  # noqa: BLE001
        pass
    try:
        src = Quartz.CGEventSourceCreate(Quartz.kCGEventSourceStateCombinedSessionState)
        if src:
            secs = Quartz.CGEventSourceSecondsSinceLastEventType(
                Quartz.kCGEventSourceStateCombinedSessionState, Quartz.kCGAnyInputEventType)
            if secs > IDLE_INPUT_SECS:
                return True
    except Exception:  # noqa: BLE001
        pass
    return False


def _map_sys_key(code):
    if code == NX_KEYTYPE_PLAY:
        return MEDIA_TOGGLE
    if code == NX_KEYTYPE_NEXT:
        return MEDIA_NEXT
    if code == NX_KEYTYPE_PREVIOUS:
        return MEDIA_PREV
    return None


def _tap_run(events):
    def cb(proxy, etype, event, refcon):
        if etype in (Quartz.kCGEventTapDisabledByTimeout, Quartz.kCGEventTapDisabledByUserInput):
            Quartz.CGEventTapEnable(tap, True)
            return None
        try:
            ns = NSEvent.eventWithCGEvent_(event)
            if ns is None or ns.type() != 14 or ns.subtype() != 8:
                return None
            keycode = (ns.data1() >> 16) & 0xFFFF
            if ((ns.data2() >> 16) & 0xFFFF) != 0xA:
                return None
        except Exception:  # noqa: BLE001
            return None
        action = _map_sys_key(keycode)
        if action is not None:
            events.put(("media", action, None))
            print(f"MAC MEDIA KEY: code={keycode} -> {action}", flush=True)
            return None
        if keycode in (NX_KEYTYPE_BRIGHTNESS_UP, NX_KEYTYPE_BRIGHTNESS_DOWN):
            b = read_brightness_pct()
            if b is not None:
                events.put(("bright", b, None))
            return None
        if keycode in (NX_KEYTYPE_SOUND_UP, NX_KEYTYPE_SOUND_DOWN, NX_KEYTYPE_MUTE):
            v = volume_state()
            if v is not None:
                events.put(("vol", v[0], int(v[1])))
        return None

    tap = Quartz.CGEventTapCreate(
        Quartz.kCGHIDEventTap, Quartz.kCGHeadInsertEventTap,
        Quartz.kCGEventTapOptionListenOnly,
        Quartz.kCGEventMaskForAllEvents, cb, None)
    if tap is None:
        print("MAC: медиа-клавиши неактивны — выдайте права ввода с клавиатуры "
              "(Конфиденциальность -> Специальные возможности)", flush=True)
        return
    src = Quartz.CFMachPortCreateRunLoopSource(Quartz.kCFAllocatorDefault, tap, 0)
    loop = Quartz.CFRunLoopGetCurrent()
    Quartz.CFRunLoopAddSource(loop, src, Quartz.kCFRunLoopDefaultMode)
    Quartz.CGEventTapEnable(tap, True)
    Quartz.CFRunLoopRun()


def watch_media(events):
    if not HAS_QUARTZ or not HAS_APPKIT:
        print("MAC: нет pyobjc (Quartz/AppKit) — медиа-клавиши неактивны", flush=True)
        return
    threading.Thread(target=_tap_run, args=(events,), daemon=True).start()