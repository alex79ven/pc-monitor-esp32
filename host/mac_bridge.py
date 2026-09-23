"""macOS-специфика для host/monitor.py: яркость, громкость, бездействие, медиа-клавиши.
Требует: pip install pyobjc-framework-Quartz pyobjc-framework-Cocoa
Медиа-клавиши требуют прав: Системные настройки -> Конфиденциальность -> Специальные возможности."""
import ctypes
import json
import re
import subprocess
import threading
import time

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
NX_KEYTYPE_BRIGHTNESS_UP = 2
NX_KEYTYPE_BRIGHTNESS_DOWN = 3
NX_KEYTYPE_MUTE = 7
NX_KEYTYPE_PLAY = 16
NX_KEYTYPE_NEXT = 17
NX_KEYTYPE_PREVIOUS = 18
NX_KEYTYPE_ILLUMINATION_UP = 21
NX_KEYTYPE_ILLUMINATION_DOWN = 22

MEDIA_TOGGLE = 2
MEDIA_NEXT = 3
MEDIA_PREV = 4

KBD_BACKLIGHT_MAX = 4095

_DS_FW = None
_DS_MISSING = object()
_BRIGHTNESS_CACHE = {"value": None, "ts": -1e9}
_KBD_CACHE = {"value": None, "ts": -1e9}
_SAFARI_CACHE = {"item": None, "ts": -1e9}
_SAVER_CACHE = {"active": False, "ts": -1e9}


def _ds():
    global _DS_FW
    if _DS_FW is None:
        try:
            fw = ctypes.CDLL("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices")
            if not all(hasattr(fw, name) for name in (
                    "DisplayServicesGetActiveDisplayID", "DisplayServicesGetBrightness")):
                raise AttributeError("DisplayServices brightness API is unavailable")
            fw.DisplayServicesGetActiveDisplayID.restype = ctypes.c_uint32
            fw.DisplayServicesGetActiveDisplayID.argtypes = []
            fw.DisplayServicesGetBrightness.restype = ctypes.c_int
            fw.DisplayServicesGetBrightness.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)]
            _DS_FW = fw
        except Exception:  # noqa: BLE001
            _DS_FW = _DS_MISSING
    return None if _DS_FW is _DS_MISSING else _DS_FW


def _read_ios_brightness():
    try:
        out = subprocess.run(["/usr/sbin/ioreg", "-r", "-c", "AppleBacklightDisplay"],
                             capture_output=True, text=True, timeout=2).stdout
        m = re.search(r'"brightness"\s*=\s*\{([^}]*)\}', out)
        if not m:
            return None
        values = {k: int(v) for k, v in re.findall(
            r'"(min|max|value)"\s*=\s*(\d+)', m.group(1))}
        lo, hi, value = values.get("min"), values.get("max"), values.get("value")
        if lo is None or hi is None or value is None or hi <= lo:
            return None
        return max(0, min(100, round((value - lo) * 100 / (hi - lo))))
    except Exception:  # noqa: BLE001
        return None


def read_brightness_pct(force=False):
    now = time.monotonic()
    if not force and now - _BRIGHTNESS_CACHE["ts"] < 0.4:
        return _BRIGHTNESS_CACHE["value"]

    value = _read_ios_brightness()
    if value is None:
        fw = _ds()
        if fw is not None:
            try:
                did = fw.DisplayServicesGetActiveDisplayID()
                brightness = ctypes.c_float(0.0)
                if fw.DisplayServicesGetBrightness(did, ctypes.byref(brightness)) == 0:
                    value = max(0, min(100, round(brightness.value * 100)))
            except Exception:  # noqa: BLE001
                pass

    _BRIGHTNESS_CACHE["value"] = value
    _BRIGHTNESS_CACHE["ts"] = now
    return value


def _read_ios_kbd():
    commands = (
        ["/usr/sbin/ioreg", "-r", "-n", "AppleLMUController"],
        ["/usr/sbin/ioreg", "-rc", "AppleHIDKeyboardEventDriver", "-l"],
    )
    for command in commands:
        try:
            out = subprocess.run(command, capture_output=True, text=True, timeout=2).stdout
            m = re.search(r'"KeyboardBacklightBrightness"\s*=\s*(\d+)', out)
            if m:
                return max(0, min(100, round(int(m.group(1)) * 100 / KBD_BACKLIGHT_MAX)))
            for m in re.finditer(r'"Keyboard(Brightness|Backlight)"\s*=\s*(\d+)', out):
                return max(0, min(100, round(int(m.group(2)) * 100 / 255)))
        except Exception:  # noqa: BLE001
            pass
    return None


def read_kbd_pct(force=False):
    now = time.monotonic()
    if not force and now - _KBD_CACHE["ts"] < 0.4:
        return _KBD_CACHE["value"]
    value = _read_ios_kbd()
    _KBD_CACHE["value"] = value
    _KBD_CACHE["ts"] = now
    return value


_MEDIA_REMOTE_JXA = r'''
const bundle = $.NSBundle.bundleWithPath("/System/Library/PrivateFrameworks/MediaRemote.framework");
if (!bundle || !bundle.load) "";
const requestClass = $.NSClassFromString("MRNowPlayingRequest");
if (!requestClass) "";
const playerPath = requestClass.localNowPlayingPlayerPath;
const client = playerPath ? playerPath.client : null;
const item = requestClass.localNowPlayingItem;
const info = item ? item.nowPlayingInfo : null;
function asText(value) {
    try { return value == null ? "" : String(value.js); }
    catch (error) { return ""; }
}
function infoValue(key) {
    try { return info ? asText(info.valueForKey(key)) : ""; }
    catch (error) { return ""; }
}
const title = infoValue("kMRMediaRemoteNowPlayingInfoTitle");
const artist = infoValue("kMRMediaRemoteNowPlayingInfoArtist");
const app = client ? asText(client.displayName) : "";
let rate = 1;
try {
    const rateValue = info ? info.valueForKey("kMRMediaRemoteNowPlayingInfoPlaybackRate") : null;
    if (rateValue) rate = Number(rateValue.doubleValue);
} catch (error) {}
const payload = {app: app, title: title, artist: artist};
title && isFinite(rate) && rate > 0 ? JSON.stringify(payload) : "";
'''


def _media_item(app, title, artist="", icon=None):
    title = (title or "").strip()
    artist = (artist or "").strip()
    text = f"{artist} - {title}" if artist and title else (title or artist)
    if not text:
        return None
    if icon is None:
        low = (app or "").lower()
        icon = "1" if any(k in low for k in (
            "youtube", "safari", "chrome", "chromium", "firefox", "brave", "vivaldi", "edge", "arc")) else "0"
    data = text.encode("utf-8")[:140]
    return icon, data.decode("utf-8", "ignore")


def _safari_now_playing():
    now = time.monotonic()
    if now - _SAFARI_CACHE["ts"] < 2.0:
        return _SAFARI_CACHE["item"]
    _SAFARI_CACHE["ts"] = now
    _SAFARI_CACHE["item"] = None
    try:
        running = subprocess.run(["/usr/bin/pgrep", "-x", "Safari"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 timeout=1)
        if running.returncode != 0:
            return None
        script = '''
tell application "Safari"
    if (count of windows) is 0 then return ""
    set currentTab to current tab of front window
    return (URL of currentTab) & (ASCII character 9) & (name of currentTab)
end tell
'''
        out = subprocess.run(["/usr/bin/osascript", "-e", script],
                             capture_output=True, text=True, timeout=3).stdout.strip()
        if "\t" not in out:
            return None
        url, title = out.split("\t", 1)
        if any(marker in url for marker in (
                "youtube.com/watch", "youtu.be/", "youtube.com/shorts/", "youtube.com/live/")):
            title = re.sub(r"^\(\d+\)\s*", "", title).strip()
            title = re.sub(r"\s*\(\d+\)\s*$", "", title).strip()
            title = re.sub(r"\s+-\s+YouTube(?:\s.*)?$", "", title).strip()
            _SAFARI_CACHE["item"] = _media_item("Safari", title, icon="1")
    except Exception:  # noqa: BLE001
        pass
    return _SAFARI_CACHE["item"]


def now_playing():
    try:
        out = subprocess.run(["/usr/bin/osascript", "-l", "JavaScript", "-e", _MEDIA_REMOTE_JXA],
                             capture_output=True, text=True, timeout=3).stdout.strip()
        if out:
            data = json.loads(out)
            item = _media_item(data.get("app"), data.get("title"), data.get("artist"))
            if item is not None:
                return item
    except Exception:  # noqa: BLE001
        pass
    return _safari_now_playing()


def volume_state():
    try:
        out = subprocess.run(["/usr/bin/osascript", "-e", "get volume settings"],
                             capture_output=True, text=True, timeout=3).stdout
        m = re.search(r"output volume:(\d+)", out)
        if not m:
            return None
        pct = int(m.group(1))
        muted = "output muted:true" in out
        return pct, muted
    except Exception:  # noqa: BLE001
        return None


def _screensaver_window_visible():
    if not HAS_QUARTZ:
        return False
    try:
        windows = Quartz.CGWindowListCopyWindowInfo(
            Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID) or []
        for window in windows:
            owner = str(window.get("kCGWindowOwnerName", ""))
            try:
                layer = int(window.get("kCGWindowLayer", -1))
            except (TypeError, ValueError):
                layer = -1
            if owner in ("ScreenSaverEngine", "Screen Saver") or layer >= 2147483000:
                return True
    except Exception:  # noqa: BLE001
        pass
    return False


def screensaver_active():
    now = time.monotonic()
    if now - _SAVER_CACHE["ts"] < 0.5:
        return _SAVER_CACHE["active"]
    try:
        result = subprocess.run(
            ["/usr/bin/pgrep", "-x", "ScreenSaverEngine"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1)
        active = result.returncode == 0
    except Exception:  # noqa: BLE001
        active = False
    if not active:
        active = _screensaver_window_visible()
    _SAVER_CACHE["active"] = active
    _SAVER_CACHE["ts"] = now
    return active


def screen_state():
    if screensaver_active():
        return "saver"
    if not HAS_QUARTZ:
        return None
    try:
        d = Quartz.CGSessionCopyCurrentDictionary()
        if d and d.get("CGSSessionScreenIsLocked") == "1":
            return "lock"
    except Exception:  # noqa: BLE001
        pass
    try:
        if Quartz.CGDisplayIsAsleep():
            return "off"
    except Exception:  # noqa: BLE001
        pass
    try:
        src = Quartz.CGEventSourceCreate(Quartz.kCGEventSourceStateCombinedSessionState)
        if src:
            secs = Quartz.CGEventSourceSecondsSinceLastEventType(
                Quartz.kCGEventSourceStateCombinedSessionState, Quartz.kCGAnyInputEventType)
            if secs > IDLE_INPUT_SECS:
                return "off"
    except Exception:  # noqa: BLE001
        pass
    return None


def screen_idle():
    return screen_state() is not None


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
            brightness = read_brightness_pct(force=True)
            if brightness is not None:
                events.put(("bright", brightness, None))
                print(f"MAC DISPLAY BRIGHTNESS: {brightness}", flush=True)
            return None
        if keycode in (NX_KEYTYPE_ILLUMINATION_UP, NX_KEYTYPE_ILLUMINATION_DOWN):
            brightness = read_kbd_pct(force=True)
            if brightness is not None:
                events.put(("kbd", brightness, None))
                print(f"MAC KEYBOARD BRIGHTNESS: {brightness}", flush=True)
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