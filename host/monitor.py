#!/usr/bin/env python3
"""Отправляет показатели системы ПК на ESP32 (OLED монитор)
по USB-serial и/или BLE (Nordic UART).
Также ловит изменения яркости/подсветки/громкости и шлёт OSD-события (BRIGHT/KBD/VOL).
Поддерживает Linux, macOS (mac_bridge) и Windows (win_bridge)."""
import argparse
import asyncio
import glob
import json
import os
import queue
import re
import select
import signal
import struct
import subprocess
import sys
import threading
import time

import psutil
import serial

IS_LINUX = sys.platform == "linux"
IS_DARWIN = sys.platform == "darwin"
IS_WINDOWS = sys.platform == "win32"

if sys.platform not in ("linux", "darwin", "win32"):
    sys.exit("Скрипт рассчитан на Linux, macOS и Windows")

if IS_DARWIN:
    import mac_bridge as bridge  # noqa: E402
elif IS_WINDOWS:
    import win_bridge as bridge  # noqa: E402
else:
    bridge = None

from bleak import BleakClient, BleakScanner  # noqa: E402

INTERVAL = 1.0
BLE_NAME = "OLED-MONITOR"
BLE_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
BLE_CHAR_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
BLE_CHAR_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

OSD_TAGS = {"bright": "BRIGHT", "kbd": "KBD", "vol": "VOL", "media": "MEDIA"}

MEDIA_PLAY = 1
MEDIA_TOGGLE = 2
MEDIA_NEXT = 3
MEDIA_PREV = 4

IDLE_STATE = [None]

if IS_LINUX:
    MEDIA_KEY_ACTIONS = {163: MEDIA_NEXT, 164: MEDIA_TOGGLE, 165: MEDIA_PREV, 207: MEDIA_PLAY}
    _INPUT_EVENT = struct.Struct("@llHHI")


def sample_line():
    cpu = int(psutil.cpu_percent())
    ram = int(psutil.virtual_memory().percent)
    disk = int(psutil.disk_usage("/").percent)
    temp = int(pick_temp())
    return f"CPU {cpu} RAM {ram} DISK {disk} TEMP {temp}"


def pick_temp():
    for name in ("coretemp", "k10temp", "cpu_thermal", "acpitz"):
        if name in psutil.sensors_temperatures():
            zones = psutil.sensors_temperatures()[name]
            if zones:
                return max(z.current or 0 for z in zones)
    return 0


SAVER_CLASS = "org.omarchy.screensaver"


def session_locked():
    """Omarchy: True если активен локскрин, False если нет, None если не определить."""
    try:
        out = subprocess.run(["omarchy-shell", "lock", "status"],
                             capture_output=True, text=True, timeout=2).stdout
        i = out.find("{")
        if i < 0:
            return None
        st = json.loads(out[i:])
        if "locked" in st:
            return bool(st["locked"])
        if "sessionLocked" in st:
            return bool(st["sessionLocked"])
    except Exception:  # noqa: BLE001
        pass
    return None


def idle_reason():
    """Linux: 'saver' заставка, 'lock' блокировка, 'off' экран погас, None активность.
    macOS/Windows: 'off' если бездействие, иначе None."""
    if bridge is not None:
        return "off" if bridge.screen_idle() else None

    saver = False
    try:
        out = subprocess.run(["hyprctl", "clients", "-j"],
                             capture_output=True, text=True, timeout=2).stdout
        cs = json.loads(out)
        for c in cs:
            if c.get("class") == SAVER_CLASS or c.get("initialClass") == SAVER_CLASS:
                saver = True
    except Exception:  # noqa: BLE001
        pass
    if not saver:
        try:
            saver = bool(subprocess.run(["pgrep", "-f", SAVER_CLASS],
                                        capture_output=True, text=True, timeout=2).stdout)
        except Exception:  # noqa: BLE001
            pass

    locked = session_locked()
    off = False
    try:
        out = subprocess.run(["hyprctl", "monitors", "-j"],
                             capture_output=True, text=True, timeout=2).stdout
        ms = json.loads(out)
        off = any(not m.get("dpmsStatus", True) for m in ms)
    except Exception:  # noqa: BLE001
        pass

    if off:
        return "off"
    if locked:
        return "lock"
    if saver:
        return "saver"
    return None


def find_port():
    if IS_WINDOWS:
        try:
            from serial.tools import list_ports
            for p in list_ports.comports():
                if p.device and "Bluetooth" not in (p.description or ""):
                    return p.device
        except Exception:  # noqa: BLE001
            pass
        return None
    if IS_DARWIN:
        pats = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*")
    else:
        pats = ("/dev/ttyACM*", "/dev/ttyUSB*")
    for pat in pats:
        ports = glob.glob(pat)
        if ports:
            return ports[0]
    return None


def read_sysfs_pct(base):
    with open(f"{base}/brightness") as f:
        b = int(f.read())
    with open(f"{base}/max_brightness") as f:
        m = int(f.read())
    return round(b * 100 / m) if m else 0


def media_status():
    """Linux: PlaybackStatus 1 играет, 0 пауза, None нет активного MPRIS-плеера."""
    try:
        out = subprocess.run(["busctl", "--user", "list"],
                             capture_output=True, text=True, timeout=1).stdout
    except Exception:  # noqa: BLE001
        return None
    names = [l.split()[0] for l in out.splitlines() if "org.mpris." in l]
    statuses = []
    for name in names:
        try:
            r = subprocess.run(
                ["busctl", "--user", "get-property", name,
                 "org.mpris.MediaPlayer2.Player", "PlaybackStatus"],
                capture_output=True, text=True, timeout=1).stdout
            statuses.append(r)
        except Exception:  # noqa: BLE001
            continue
    if any("Playing" in s for s in statuses):
        return 1
    if any("Paused" in s for s in statuses):
        return 0
    return None


def watch_media_keys_linux(events: "queue.Queue"):
    """Linux: ловит нажатия медиа-клавиш и публикует (media, status)."""
    fds = []
    for pat in glob.glob("/dev/input/event*"):
        try:
            fd = os.open(pat, os.O_RDONLY | os.O_NONBLOCK)
            fds.append(fd)
        except OSError:
            continue
    if not fds:
        print("MEDIA: нет доступа к /dev/input, медиа-клавиши неактивны", flush=True)
        return
    while True:
        try:
            r, _, _ = select.select(fds, [], [], 0.3)
        except (OSError, ValueError):
            break
        for fd in r:
            try:
                while True:
                    data = os.read(fd, _INPUT_EVENT.size)
                    if len(data) != _INPUT_EVENT.size:
                        break
                    _, _, etype, code, value = _INPUT_EVENT.unpack(data)
                    if etype != 1 or value != 1:
                        continue
                    if code in MEDIA_KEY_ACTIONS:
                        action = MEDIA_KEY_ACTIONS[code]
                        if action == MEDIA_TOGGLE:
                            time.sleep(0.25)
                            st = media_status()
                            action = st if st is not None else MEDIA_TOGGLE
                        events.put(("media", action, None))
                        print(f"MEDIA KEY: code={code} -> {action}", flush=True)
                        continue
                    if IDLE_STATE[0] == "lock":
                        events.put(("key", None, None))
                        print(f"LOCK KEY: code={code}", flush=True)
            except OSError:
                continue


def watch_system_linux(events: "queue.Queue"):
    """Linux: публикует (name, pct) при изменении яркости/подсветки/громкости/бездействия."""
    paths = {
        "bright": next(iter(glob.glob("/sys/class/backlight/*")), None),
        "kbd": next((p for p in glob.glob("/sys/class/leds/*") if p.endswith("kbd_backlight")), None),
    }
    prev = {}
    prev_reason = "?"
    last_reason_t = 0
    while True:
        for name, base in paths.items():
            if not base:
                continue
            try:
                pct = read_sysfs_pct(base)
            except Exception:  # noqa: BLE001
                continue
            if prev.get(name) != pct:
                prev[name] = pct
                events.put((name, pct, None))
        t = time.time()
        if t - last_reason_t >= 1.0:
            last_reason_t = t
            reason = idle_reason()
            if reason != prev_reason:
                prev_reason = reason
                events.put(("idle", reason, None))
                print(f"IDLE STATE: {reason}", flush=True)
        try:
            out = subprocess.run(
                ["pactl", "get-sink-volume", "@DEFAULT_SINK@"],
                capture_output=True, text=True, timeout=2).stdout
            m = re.search(r"(\d+)%", out)
            mout = subprocess.run(
                ["pactl", "get-sink-mute", "@DEFAULT_SINK@"],
                capture_output=True, text=True, timeout=2).stdout
            muted = "yes" in mout.lower()
            if m and (prev.get("vol") != int(m.group(1)) or prev.get("mute") != muted):
                prev["vol"] = int(m.group(1))
                prev["mute"] = muted
                events.put(("vol", prev["vol"], int(muted)))
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.3)


def watch_system_bridge(events: "queue.Queue"):
    """macOS/Windows: публикует (name, pct) при изменении яркости/подсветки/громкости/бездействия."""
    prev = {}
    prev_idle = None
    while True:
        b = bridge.read_brightness_pct()
        if b is not None and prev.get("bright") != b:
            prev["bright"] = b
            events.put(("bright", b, None))
        k = bridge.read_kbd_pct()
        if k is not None and prev.get("kbd") != k:
            prev["kbd"] = k
            events.put(("kbd", k, None))
        v = bridge.volume_state()
        if v is not None and (prev.get("vol") != v[0] or prev.get("mute") != v[1]):
            prev["vol"], prev["mute"] = v[0], v[1]
            events.put(("vol", v[0], v[1]))
        it = bridge.screen_idle()
        if prev_idle != it:
            prev_idle = it
            events.put(("idle", "off" if it else None, None))
            print(f"IDLE STATE: {'off' if it else 'None'}", flush=True)
        time.sleep(0.3)


def watch_system(events: "queue.Queue"):
    if bridge is not None:
        watch_system_bridge(events)
    else:
        watch_system_linux(events)


def watch_media_keys(events: "queue.Queue"):
    if bridge is not None:
        bridge.watch_media(events)
    else:
        watch_media_keys_linux(events)


class BleSender(threading.Thread):
    def __init__(self, name=BLE_NAME):
        super().__init__(daemon=True)
        self.name = name
        self.connected = False
        self._rx = None
        self._loop = None
        self._ready = threading.Event()
        self._err = None
        self._dbg = False
        self._stop = False

    def run(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._main())

    async def _find(self):
        devices = await BleakScanner.discover(timeout=3)
        return next((d for d in devices if self.name in (d.name or "")), None)

    async def _main(self):
        while not self._stop:
            client = None
            try:
                dev = await self._find()
                if dev is None:
                    raise ConnectionError("устройство не найдено в радиусе")
                print(f"BLE: подключаюсь к {dev.name}", flush=True)
                client = BleakClient(dev.address)
                await client.connect(timeout=10)
                await client.start_notify(BLE_CHAR_TX, lambda *a, **k: None)
                self._rx = client
                self.connected = True
                self._ready.set()
                self._dbg = False
                while not self._stop and client.is_connected and self._rx is client:
                    await asyncio.sleep(0.5)
            except asyncio.CancelledError:
                break
            except Exception as e:  # noqa: BLE001
                self._err = e
                print(f"BLE: сбой: {e!r}", flush=True)
            finally:
                if client is not None:
                    try:
                        await client.disconnect()
                    except Exception:  # noqa: BLE001
                        pass
                self._rx = None
                self.connected = False
                if not self._stop:
                    await asyncio.sleep(2)

    def stop(self):
        self._stop = True
        if self._loop is not None:
            try:
                asyncio.run_coroutine_threadsafe(self._force_disconnect(), self._loop).result(timeout=3)
            except Exception:  # noqa: BLE001
                pass

    async def _force_disconnect(self):
        if self._rx is not None:
            try:
                await self._rx.disconnect()
            except Exception:  # noqa: BLE001
                pass

    def send(self, data):
        if not self.connected or self._loop is None:
            return False
        fut = asyncio.run_coroutine_threadsafe(
            self._rx.write_gatt_char(BLE_CHAR_RX, (data + "\n").encode()), self._loop)
        try:
            fut.result(timeout=2)
            return True
        except Exception as e:  # noqa: BLE001
            print(f"BLE: write failed: {e!r}", flush=True)
            self.connected = False
            self._rx = None
            return False


def send_line(ser, ble, line):
    if ser is not None:
        try:
            ser.write((line + "\n").encode())
        except (serial.SerialException, OSError):
            print("USB-serial отключён, жду переподключения...", flush=True)
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            return None
    if ble is not None and ble.connected:
        if ble.send(line) and not ble._dbg:
            print("BLE: подключено, шлю данные", flush=True)
            ble._dbg = True
    return ser


def on_signal(ble):
    print("\nОстановка...", flush=True)
    if ble:
        ble.stop()
    sys.exit(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, авто-поиск если не задан")
    ap.add_argument("--rate", type=float, default=INTERVAL, help="период обновления, сек")
    ap.add_argument("--bt", metavar="NAME", nargs="?", const=BLE_NAME, default=None,
                    help="включить отправку по BLE (имя устройства, по умолчанию OLED-MONITOR)")
    ap.add_argument("--no-serial", action="store_true", help="не использовать USB-serial")
    args = ap.parse_args()

    ble = BleSender(args.bt or BLE_NAME) if args.bt else None
    if ble:
        ble.start()
    signal.signal(signal.SIGTERM, lambda *a: on_signal(ble))
    signal.signal(signal.SIGINT, lambda *a: on_signal(ble))

    events = queue.Queue()
    threading.Thread(target=watch_system, args=(events,), daemon=True).start()
    threading.Thread(target=watch_media_keys, args=(events,), daemon=True).start()

    try:
        ser = None
        idle = None
        while True:
            if not args.no_serial and ser is None:
                port = args.port or find_port()
                if port:
                    try:
                        ser = serial.Serial(port, 115200, timeout=1)
                        time.sleep(0.2)
                        print(f"Отправка на {port} каждые {args.rate}s, Ctrl+C = выход", flush=True)
                    except (serial.SerialException, OSError):
                        ser = None
                if ser is None and not ble:
                    print("Не найден порт, жду подключение...", flush=True)
                    time.sleep(2)
                    continue

            drawn = False
            while True:
                try:
                    name, val, mute = events.get_nowait()
                except queue.Empty:
                    break
                if name == "idle":
                    idle = val
                    IDLE_STATE[0] = val
                    continue
                if name == "key":
                    if idle == "lock":
                        print("OSD: KEY", flush=True)
                        ser = send_line(ser, ble, "KEY")
                        drawn = True
                    continue
                line = f"{OSD_TAGS[name]} {val}" if name != "vol" else f"VOL {val} MUTE {mute}"
                print(f"OSD: {line}", flush=True)
                ser = send_line(ser, ble, line)
                drawn = True

            if idle == "lock" or idle == "saver":
                if not drawn:
                    ser = send_line(ser, ble, "STARS")
            elif idle == "off":
                ser = send_line(ser, ble, time.strftime("TIME %H:%M:%S"))
            elif not drawn:
                ser = send_line(ser, ble, sample_line())

            time.sleep(args.rate)
    except KeyboardInterrupt:
        print("\nОстановка...", flush=True)
        if ble:
            ble.stop()


if __name__ == "__main__":
    main()