"""Read CPU temperature from Apple SMC on Intel Macs.

The SMC interface is private, but the read-only calls used here are available
through IOKit on Intel Macs and do not require root. The module is optional:
callers should treat a missing/unavailable sensor as no reading.
"""
import ctypes
import struct
import threading
from typing import Optional

_KERN_SUCCESS = 0
_SMC_USER_CLIENT_METHOD = 2
_SMC_CMD_READ_BYTES = 5
_SMC_CMD_READ_KEYINFO = 9


class _SMCVersion(ctypes.Structure):
    _fields_ = [
        ("major", ctypes.c_uint8),
        ("minor", ctypes.c_uint8),
        ("build", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8),
        ("release", ctypes.c_uint16),
    ]


class _SMCPLimitData(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint16),
        ("length", ctypes.c_uint16),
        ("cpu_plimit", ctypes.c_uint32),
        ("gpu_plimit", ctypes.c_uint32),
        ("mem_plimit", ctypes.c_uint32),
    ]


class _SMCKeyInfo(ctypes.Structure):
    _fields_ = [
        ("data_size", ctypes.c_uint32),
        ("data_type", ctypes.c_uint32),
        ("data_attributes", ctypes.c_uint8),
    ]


class _SMCKeyData(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_uint32),
        ("version", _SMCVersion),
        ("p_limit_data", _SMCPLimitData),
        ("key_info", _SMCKeyInfo),
        ("result", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
        ("data8", ctypes.c_uint8),
        ("data32", ctypes.c_uint32),
        ("bytes", ctypes.c_uint8 * 32),
    ]


class _SMCReader:
    def __init__(self):
        self._lock = threading.Lock()
        self._iokit = None
        self._connection = None

    @staticmethod
    def _fourcc(value):
        return int.from_bytes(value.encode("ascii"), "big")

    @staticmethod
    def _type_name(value):
        return struct.pack(">I", value).decode("ascii", errors="replace")

    def _load_iokit(self):
        if self._iokit is not None:
            return
        iokit = ctypes.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
        system = ctypes.CDLL(None)

        iokit.IOMasterPort.argtypes = [
            ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)
        ]
        iokit.IOMasterPort.restype = ctypes.c_uint32
        iokit.IOServiceMatching.argtypes = [ctypes.c_char_p]
        iokit.IOServiceMatching.restype = ctypes.c_void_p
        iokit.IOServiceGetMatchingService.argtypes = [
            ctypes.c_uint32, ctypes.c_void_p
        ]
        iokit.IOServiceGetMatchingService.restype = ctypes.c_uint32
        iokit.IOServiceOpen.argtypes = [
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        iokit.IOServiceOpen.restype = ctypes.c_uint32
        iokit.IOConnectCallStructMethod.argtypes = [
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
        ]
        iokit.IOConnectCallStructMethod.restype = ctypes.c_uint32
        iokit.IOServiceClose.argtypes = [ctypes.c_uint32]
        iokit.IOServiceClose.restype = ctypes.c_uint32
        iokit.IOObjectRelease.argtypes = [ctypes.c_uint32]
        iokit.IOObjectRelease.restype = ctypes.c_uint32

        # mach_task_self_ is exported as a global mach_port_t by libSystem.
        try:
            task = ctypes.c_uint32.in_dll(system, "mach_task_self_").value
        except (AttributeError, ValueError):
            system.mach_task_self.restype = ctypes.c_uint32
            task = system.mach_task_self()

        self._iokit = iokit
        self._task = task

    def _open(self):
        if self._connection is not None:
            return True
        try:
            self._load_iokit()
            master_port = ctypes.c_uint32()
            result = self._iokit.IOMasterPort(0, ctypes.byref(master_port))
            if result != _KERN_SUCCESS:
                return False
            matching = self._iokit.IOServiceMatching(b"AppleSMC")
            if not matching:
                return False
            service = self._iokit.IOServiceGetMatchingService(
                master_port.value, matching)
            if not service:
                return False
            connection = ctypes.c_uint32()
            result = self._iokit.IOServiceOpen(
                service, self._task, 0, ctypes.byref(connection))
            self._iokit.IOObjectRelease(service)
            if result != _KERN_SUCCESS:
                return False
            self._connection = connection
            return True
        except (AttributeError, OSError, TypeError, ValueError):
            self._connection = None
            return False

    def _close(self):
        if self._connection is None or self._iokit is None:
            return
        try:
            self._iokit.IOServiceClose(self._connection)
        except (AttributeError, OSError):
            pass
        self._connection = None

    def _call(self, request):
        response = _SMCKeyData()
        response_size = ctypes.c_size_t(ctypes.sizeof(response))
        result = self._iokit.IOConnectCallStructMethod(
            self._connection, _SMC_USER_CLIENT_METHOD,
            ctypes.byref(request), ctypes.sizeof(request),
            ctypes.byref(response), ctypes.byref(response_size))
        return response if result == _KERN_SUCCESS else None

    def read_key(self, key):
        with self._lock:
            if not self._open():
                return None
            try:
                key_code = self._fourcc(key)
                request = _SMCKeyData()
                request.key = key_code
                request.data8 = _SMC_CMD_READ_KEYINFO
                info = self._call(request)
                if info is None or not info.key_info.data_size:
                    return None

                request = _SMCKeyData()
                request.key = key_code
                request.key_info.data_size = info.key_info.data_size
                request.data8 = _SMC_CMD_READ_BYTES
                value = self._call(request)
                if value is None:
                    return None
                size = min(int(info.key_info.data_size), len(value.bytes))
                raw = bytes(value.bytes[:size])
                return self._decode(raw, self._type_name(info.key_info.data_type))
            except (AttributeError, OSError, TypeError, ValueError, struct.error):
                self._close()
                return None

    @staticmethod
    def _decode(raw, type_name):
        if len(raw) < 1:
            return None
        try:
            if type_name == "sp78" and len(raw) >= 2:
                return int.from_bytes(raw[:2], "big", signed=True) / 256.0
            if type_name == "sp87" and len(raw) >= 2:
                return int.from_bytes(raw[:2], "big", signed=True) / 128.0
            if type_name == "sp96" and len(raw) >= 2:
                return int.from_bytes(raw[:2], "big", signed=True) / 64.0
            if type_name == "flt " and len(raw) >= 4:
                value = struct.unpack(">f", raw[:4])[0]
                if not -20.0 < value < 150.0:
                    value = struct.unpack("<f", raw[:4])[0]
                return value
            if type_name == "ui8":
                return float(raw[0])
            if type_name == "ui16" and len(raw) >= 2:
                return float(int.from_bytes(raw[:2], "big"))
            if type_name == "si16" and len(raw) >= 2:
                return float(int.from_bytes(raw[:2], "big", signed=True))
        except (ValueError, struct.error):
            return None
        return None


_READER = _SMCReader()
_CPU_KEYS = ("TC0P", "TC0D", "TC0H", "TCXC", "TCXc")


def cpu_temperature() -> Optional[float]:
    """Return a representative CPU temperature in Celsius, if available."""
    for key in _CPU_KEYS:
        value = _READER.read_key(key)
        if value is not None and 0.0 < value < 150.0:
            return value
    return None
