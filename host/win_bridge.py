"""Windows-специфика для host/monitor.py: яркость, громкость, бездействие, медиа-клавиши.
Требует: pip install pywin32 comtypes pycaw
Медиа-клавиши ("низкоуровневый hook" клавиатуры) работают без прав администратора."""
import ctypes
import subprocess

IDLE_INPUT_SECS = 120

VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF
VK_MEDIA_NEXT = 0xB0
VK_MEDIA_PREV = 0xB1
VK_MEDIA_STOP = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3

MEDIA_TOGGLE = 2
MEDIA_NEXT = 3
MEDIA_PREV = 4


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_ulong)]


def screen_idle():
    try:
        lii = LASTINPUTINFO()
        lii.cbSize = ctypes.sizeof(LASTINPUTINFO)
        if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii)):
            return False
        secs = (ctypes.windll.kernel32.GetTickCount() - lii.dwTime) / 1000.0
        return secs > IDLE_INPUT_SECS
    except Exception:  # noqa: BLE001
        return False


def read_brightness_pct():
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness).CurrentBrightness"],
            capture_output=True, text=True, timeout=3).stdout.strip()
        v = int(out)
        return max(0, min(100, v))
    except Exception:  # noqa: BLE001
        return None


def read_kbd_pct():
    return None


_VOLUME = None


def _volume_endpoint():
    global _VOLUME
    if _VOLUME is None:
        try:
            from comtypes import CLSCTX_ALL
            from ctypes import cast, POINTER
            from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
            dev = AudioUtilities.GetSpeakers()
            if dev is None:
                return None
            v = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
            _VOLUME = cast(v, POINTER(IAudioEndpointVolume))
        except Exception:  # noqa: BLE001
            _VOLUME = False
    return None if _VOLUME is False else _VOLUME


def volume_state():
    vol = _volume_endpoint()
    if vol is None:
        return None
    try:
        return round(vol.GetMasterVolumeLevelScalar() * 100), bool(vol.GetMute())
    except Exception:  # noqa: BLE001
        return None


def _on_vk(vk, events):
    if vk == VK_MEDIA_PLAY_PAUSE:
        events.put(("media", MEDIA_TOGGLE, None))
        print("WIN MEDIA KEY: play/pause -> toggle", flush=True)
    elif vk == VK_MEDIA_NEXT:
        events.put(("media", MEDIA_NEXT, None))
        print("WIN MEDIA KEY: next", flush=True)
    elif vk == VK_MEDIA_PREV:
        events.put(("media", MEDIA_PREV, None))
        print("WIN MEDIA KEY: prev", flush=True)
    elif vk in (VK_VOLUME_UP, VK_VOLUME_DOWN, VK_VOLUME_MUTE):
        v = volume_state()
        if v is not None:
            events.put(("vol", v[0], int(v[1])))


def watch_media(events):
    try:
        import win32con
        import win32gui
        import win32api
        import pythoncom
    except ImportError:
        print("WIN: нет pywin32 — медиа-клавиши неактивны", flush=True)
        return

    _HOOKCTX = {}

    class KBDLLHOOKSTRUCT(ctypes.Structure):
        _fields_ = [("vkCode", ctypes.c_ulong), ("scanCode", ctypes.c_ulong),
                    ("flags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                    ("dwExtraInfo", ctypes.c_void_p)]

    def hook_proc(nCode, wParam, lParam):
        if nCode >= 0 and wParam in (win32con.WM_KEYDOWN, win32con.WM_SYSKEYDOWN):
            ks = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
            _on_vk(ks.vkCode, events)
        return win32api.CallNextHookEx(0, nCode, wParam, lParam)

    _HOOKCTX["proc"] = hook_proc
    hook = win32gui.SetWindowsHookEx(win32con.WH_KEYBOARD_LL, hook_proc, None, 0)
    if not hook:
        print("WIN: не удалось установить hook клавиатуры", flush=True)
        return
    print("WIN: hook клавиатуры активен", flush=True)
    pythoncom.PumpMessages()