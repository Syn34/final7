"""Attach to a running PlayStation emulator and locate the PS1's 2 MB of main RAM.

Windows only (uses ctypes + kernel32). Nothing is ever written to the emulator:
the process is opened with read/query rights only.

Supported (pointer/signature methods adapted from the MIT-licensed emu-help
project by Jujstme, https://github.com/Jujstme/emu-help):

    DuckStation        exported "RAM" symbol, signature fallback
    ePSXe              signature
    RetroArch          SwanStation, Beetle/Mednafen PSX (HW), PCSX ReARMed cores
    Mednafen           signature
    pSX / psxfin       signature
    PCSX-Redux         signature (64-bit builds)
    Xebra              signature

Anything else (BizHawk, other forks, future versions) falls back to a full
memory scan that looks for a loaded Forbidden Memories save (your 40-card deck
plus its mirror copy), so load your save in-game before attaching.
"""

from __future__ import annotations

import ctypes
import re
import struct
import sys
from ctypes import wintypes

PS1_RAM_SIZE = 0x200000

IS_WINDOWS = sys.platform == "win32"

# --------------------------------------------------------------------------
# kernel32 plumbing
# --------------------------------------------------------------------------
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
MEM_MAPPED = 0x40000
MEM_IMAGE = 0x1000000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}
STILL_ACTIVE = 259

if IS_WINDOWS:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", ctypes.c_wchar * 260),
        ]

    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("th32ModuleID", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("GlblcntUsage", wintypes.DWORD),
            ("ProccntUsage", wintypes.DWORD),
            ("modBaseAddr", ctypes.c_void_p),
            ("modBaseSize", wintypes.DWORD),
            ("hModule", wintypes.HMODULE),
            ("szModule", ctypes.c_wchar * 256),
            ("szExePath", ctypes.c_wchar * 260),
        ]

    if ctypes.sizeof(ctypes.c_void_p) == 8:
        class MEMORY_BASIC_INFORMATION(ctypes.Structure):
            _fields_ = [
                ("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wintypes.DWORD),
                ("PartitionId", wintypes.WORD),
                ("RegionSize", ctypes.c_ulonglong),
                ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD),
                ("Type", wintypes.DWORD),
            ]
    else:
        class MEMORY_BASIC_INFORMATION(ctypes.Structure):
            _fields_ = [
                ("BaseAddress", ctypes.c_ulong),
                ("AllocationBase", ctypes.c_ulong),
                ("AllocationProtect", wintypes.DWORD),
                ("RegionSize", ctypes.c_ulong),
                ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD),
                ("Type", wintypes.DWORD),
            ]

    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                           ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel32.ReadProcessMemory.restype = wintypes.BOOL
    kernel32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                        ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
    kernel32.VirtualQueryEx.restype = ctypes.c_size_t
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    kernel32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    kernel32.IsWow64Process.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]

    INVALID_HANDLE = wintypes.HANDLE(-1).value


class AttachError(Exception):
    pass


# --------------------------------------------------------------------------
# Process discovery
# --------------------------------------------------------------------------
# lower-case substring of the exe name -> emulator key
EMULATOR_PROCESS_HINTS = [
    ("duckstation", "duckstation"),
    ("epsxe", "epsxe"),
    ("retroarch", "retroarch"),
    ("mednafen", "mednafen"),
    ("psxfin", "psx"),
    ("pcsx-redux", "pcsxredux"),
    ("xebra", "xebra"),
    ("emuhawk", "generic"),       # BizHawk
    ("pcsx", "generic"),
    ("nopsx", "generic"),
]


def list_processes():
    """[(pid, exe_name)] for every running process."""
    if not IS_WINDOWS:
        return []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE:
        return []
    out = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            out.append((entry.th32ProcessID, entry.szExeFile))
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return out


def classify(exe_name: str):
    low = exe_name.lower()
    for hint, key in EMULATOR_PROCESS_HINTS:
        if hint in low:
            return key
    return None


def find_emulators():
    """[(pid, exe_name, emulator_key)] for processes that look like PS1 emulators."""
    found = []
    for pid, name in list_processes():
        key = classify(name)
        if key:
            found.append((pid, name, key))
    return found


# --------------------------------------------------------------------------
# A read-only handle to one process
# --------------------------------------------------------------------------
class Process:
    def __init__(self, pid: int, name: str):
        if not IS_WINDOWS:
            raise AttachError("Attaching to an emulator only works on Windows.")
        self.pid = pid
        self.name = name
        self.handle = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.handle:
            self.handle = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not self.handle:
            raise AttachError(
                f"Could not open {name} (pid {pid}). If the emulator runs as administrator, "
                "run this tracker as administrator too.")
        self.is64 = self._is_64bit()
        self.ptr_size = 8 if self.is64 else 4

    def _is_64bit(self) -> bool:
        if ctypes.sizeof(ctypes.c_void_p) == 4:
            return False
        wow = wintypes.BOOL()
        if not kernel32.IsWow64Process(self.handle, ctypes.byref(wow)):
            return True
        return not bool(wow.value)

    def alive(self) -> bool:
        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            return False
        return code.value == STILL_ACTIVE

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None

    # --- reads ------------------------------------------------------------
    def read(self, addr: int, size: int):
        """bytes, or None if any part of the range is unreadable."""
        if addr <= 0 or size <= 0:
            return None
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(self.handle, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
        if not ok or got.value != size:
            return None
        return buf.raw

    def read_ptr(self, addr: int):
        data = self.read(addr, self.ptr_size)
        if data is None:
            return None
        return struct.unpack("<Q" if self.is64 else "<I", data)[0]

    def read_u32(self, addr):
        d = self.read(addr, 4)
        return None if d is None else struct.unpack("<I", d)[0]

    def read_i32(self, addr):
        d = self.read(addr, 4)
        return None if d is None else struct.unpack("<i", d)[0]

    def read_u8(self, addr):
        d = self.read(addr, 1)
        return None if d is None else d[0]

    # --- layout -----------------------------------------------------------
    def modules(self):
        """[(name, base, size)]"""
        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, self.pid)
        if snap == INVALID_HANDLE:
            return []
        out = []
        try:
            entry = MODULEENTRY32W()
            entry.dwSize = ctypes.sizeof(MODULEENTRY32W)
            ok = kernel32.Module32FirstW(snap, ctypes.byref(entry))
            while ok:
                out.append((entry.szModule, entry.modBaseAddr or 0, entry.modBaseSize))
                ok = kernel32.Module32NextW(snap, ctypes.byref(entry))
        finally:
            kernel32.CloseHandle(snap)
        return out

    def main_module(self):
        mods = self.modules()
        for m in mods:
            if m[0].lower() == self.name.lower():
                return m
        return mods[0] if mods else None

    def regions(self, start=0, end=None):
        """Yield (base, size, state, protect, type) for every region in [start, end)."""
        mbi = MEMORY_BASIC_INFORMATION()
        addr = start
        limit = end if end is not None else (0x7FFFFFFFFFFF if self.is64 else 0xFFFFFFFF)
        while addr < limit:
            if not kernel32.VirtualQueryEx(self.handle, ctypes.c_void_p(addr), ctypes.byref(mbi),
                                           ctypes.sizeof(mbi)):
                break
            base, size = mbi.BaseAddress, mbi.RegionSize
            if size == 0:
                break
            yield base, size, mbi.State, mbi.Protect, mbi.Type
            addr = base + size

    @staticmethod
    def _readable(state, protect):
        return state == MEM_COMMIT and (protect & 0xFF) in READABLE and not (protect & PAGE_GUARD)

    # --- signature scanning ----------------------------------------------
    @staticmethod
    def compile_sig(sig: str):
        parts = []
        for tok in sig.split():
            if tok.startswith("?"):
                parts.append(b".")
            else:
                parts.append(re.escape(bytes([int(tok, 16)])))
        return re.compile(b"".join(parts), re.DOTALL)

    def sig_scan(self, base: int, size: int, sig: str):
        """Address of the first match of `sig` inside [base, base+size), or None."""
        pattern = self.compile_sig(sig)
        for rbase, rsize, state, protect, _ in self.regions(base, base + size):
            if not self._readable(state, protect):
                continue
            lo, hi = max(rbase, base), min(rbase + rsize, base + size)
            chunk = 0x1000000
            pos = lo
            while pos < hi:
                n = min(chunk + 64, hi - pos)
                data = self.read(pos, n)
                if data:
                    m = pattern.search(data)
                    if m:
                        return pos + m.start()
                pos += chunk
        return None

    # --- PE exports -------------------------------------------------------
    def find_export(self, module_base: int, symbol: str):
        try:
            hdr = self.read(module_base, 0x1000)
            if not hdr or hdr[:2] != b"MZ":
                return None
            e_lfanew = struct.unpack_from("<I", hdr, 0x3C)[0]
            magic = struct.unpack_from("<H", hdr, e_lfanew + 24)[0]
            dd_off = e_lfanew + 24 + (112 if magic == 0x20B else 96)
            exp_rva, exp_size = struct.unpack_from("<II", hdr, dd_off)
            if not exp_rva:
                return None
            exp = self.read(module_base + exp_rva, 40)
            n_funcs, n_names, a_funcs, a_names, a_ords = struct.unpack_from("<IIIII", exp, 20)
            names = self.read(module_base + a_names, 4 * n_names)
            ords = self.read(module_base + a_ords, 2 * n_names)
            funcs = self.read(module_base + a_funcs, 4 * n_funcs)
            target = symbol.encode()
            for i in range(n_names):
                name_rva = struct.unpack_from("<I", names, 4 * i)[0]
                raw = self.read(module_base + name_rva, len(target) + 1)
                if raw == target + b"\x00":
                    ordinal = struct.unpack_from("<H", ords, 2 * i)[0]
                    return module_base + struct.unpack_from("<I", funcs, 4 * ordinal)[0]
        except (struct.error, TypeError):
            return None
        return None


# --------------------------------------------------------------------------
# Locating PS1 RAM inside each emulator
# --------------------------------------------------------------------------
def _rel32(proc: Process, addr: int):
    """RIP-relative target: addr + 4 + int32 at addr."""
    off = proc.read_i32(addr)
    return None if off is None else addr + 4 + off


class RamLocator:
    """Finds PS1 RAM and re-validates it on every poll.

    `resolve()` returns the current host address of PS1 RAM, or None.
    Some emulators reallocate RAM when a game is reset, so pointer-based
    methods re-read their pointer each time.
    """

    def __init__(self, proc: Process, kind: str, log=print):
        self.proc = proc
        self.kind = kind
        self.log = log
        self.pointer_addr = None       # address of a pointer to RAM, when the method has one
        self.static_base = None        # fixed RAM address, when the method has one
        self.method = ""

    # each finder sets pointer_addr or static_base, returns True on success
    def _duckstation(self):
        p = self.proc
        mod = p.main_module()
        if not mod:
            return False
        sym = p.find_export(mod[1], "RAM")
        if sym:
            self.pointer_addr = sym
            self.method = "DuckStation (exported RAM symbol)"
            return True
        hit = p.sig_scan(mod[1], mod[2], "48 89 0D ?? ?? ?? ?? B8")
        if hit:
            self.pointer_addr = _rel32(p, hit + 3)
            self.method = "DuckStation (signature)"
            return self.pointer_addr is not None
        return False

    def _epsxe(self):
        p = self.proc
        mod = p.main_module()
        hit = mod and p.sig_scan(mod[1], mod[2], "C1 E1 10 8D 89")
        if hit:
            self.static_base = p.read_u32(hit + 5)
            self.method = "ePSXe (signature)"
            return bool(self.static_base)
        return False

    def _retroarch(self):
        p = self.proc
        cores = {
            "mednafen_psx_hw_libretro.dll": ("48 8B 05 ?? ?? ?? ?? 41 81 E4 FF FF 1F 00", "A1 ?? ?? ?? ?? 81 E3 FF FF 1F 00", 1),
            "mednafen_psx_libretro.dll": ("48 8B 05 ?? ?? ?? ?? 41 81 E4 FF FF 1F 00", "A1 ?? ?? ?? ?? 81 E3 FF FF 1F 00", 1),
            "swanstation_libretro.dll": ("48 89 0D ?? ?? ?? ?? 89 35 ?? ?? ?? ?? 89 3D", "A1 ?? ?? ?? ?? 23 CB 8B", 1),
            "pcsx_rearmed_libretro.dll": ("48 8B 35 ?? ?? ?? ?? 81 E2", "FF FF 1F 00 89 ?? ?? ?? A1", 9),
        }
        for name, base, size in p.modules():
            info = cores.get(name.lower())
            if not info:
                continue
            sig64, sig32, off32 = info
            if p.is64:
                hit = p.sig_scan(base, size, sig64)
                if hit:
                    self.pointer_addr = _rel32(p, hit + 3)
            else:
                hit = p.sig_scan(base, size, sig32)
                if hit:
                    self.pointer_addr = p.read_ptr(hit + off32)
            if self.pointer_addr:
                self.method = f"RetroArch ({name})"
                return True
        return False

    def _mednafen(self):
        p = self.proc
        mod = p.main_module()
        sig = "89 01 0F B6 82" if p.is64 else "89 01 0F B6 82 ?? ?? ?? ?? C3"
        hit = mod and p.sig_scan(mod[1], mod[2], sig)
        if hit:
            self.static_base = p.read_u32(hit + 5)
            self.method = "Mednafen (signature)"
            return bool(self.static_base)
        return False

    def _psx(self):
        p = self.proc
        mod = p.main_module()
        if not mod:
            return False
        for off, sig in ((2, "8B 15 ?? ?? ?? ?? 8D 34 1A"), (1, "A1 ?? ?? ?? ?? 8D 34 18"),
                         (1, "A1 ?? ?? ?? ?? 8B 7C 24 14"), (1, "A1 ?? ?? ?? ?? 8B 6C 24")):
            hit = p.sig_scan(mod[1], mod[2], sig)
            if hit:
                self.pointer_addr = p.read_ptr(hit + off)
                self.method = "pSX (signature)"
                return bool(self.pointer_addr)
        return False

    def _pcsxredux(self):
        p = self.proc
        mod = p.main_module()
        if not mod or not p.is64:
            return False
        hit = p.sig_scan(mod[1], mod[2], "48 B9 ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 00 00 00 00")
        hit2 = p.sig_scan(mod[1], mod[2], "89 D1 C1 E9 10 48 8B")
        if hit and hit2:
            obj = p.read_ptr(hit + 2)
            off = p.read_u8(hit2 + 8)
            if obj and off is not None:
                self.pointer_addr = obj + off
                self.method = "PCSX-Redux (signature)"
                return True
        return False

    def _xebra(self):
        p = self.proc
        mod = p.main_module()
        hit = mod and p.sig_scan(mod[1], mod[2], "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 89 C8 C1 F8 10")
        if hit:
            tgt = _rel32(p, hit + 1)
            inner = tgt and p.read_ptr(tgt + 0x16A)
            if inner:
                self.pointer_addr = inner
                self.method = "Xebra (signature)"
                return True
        return False

    def locate(self, validate, allow_scan=True, progress=None):
        """Try the emulator-specific method, then the generic scan.

        `validate(proc, base)` must return True when `base` looks like FM's RAM
        with a save loaded (used by the generic scan and as a sanity check).
        """
        finder = {
            "duckstation": self._duckstation, "epsxe": self._epsxe, "retroarch": self._retroarch,
            "mednafen": self._mednafen, "psx": self._psx, "pcsxredux": self._pcsxredux,
            "xebra": self._xebra,
        }.get(self.kind)
        if finder:
            try:
                if finder():
                    base = self.resolve()
                    if base and self.proc.read(base, PS1_RAM_SIZE) is not None:
                        return True
                    if self.pointer_addr and not base:
                        # emulator found but no game booted yet; RAM appears later
                        return True
            except Exception as exc:  # signature layouts change between versions
                self.log(f"Emulator-specific lookup failed: {exc}")
            self.pointer_addr = self.static_base = None
        if not allow_scan:
            return False
        base = generic_scan(self.proc, validate, progress)
        if base:
            self.static_base = base
            self.method = "memory scan (found your loaded save)"
            return True
        return False

    def resolve(self):
        if self.static_base:
            return self.static_base
        if self.pointer_addr:
            v = self.proc.read_ptr(self.pointer_addr)
            return v or None
        return None


# 40 little-endian u16 card ids, each 1..722 (0x001..0x2D2)
_DECK_RE = re.compile(rb"(?:[\x01-\xff]\x00|[\x00-\xff]\x01|[\x00-\xd2]\x02){40,}", re.DOTALL)
DECK_OFF = 0x1D0200
MIRROR_OFF = 0x1D3200


def generic_scan(proc: Process, validate, progress=None):
    """Search every readable region for FM's save block; return RAM base or None."""
    regions = [r for r in proc.regions()
               if Process._readable(r[2], r[3]) and r[4] in (MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE)
               and r[1] >= 0x10000]
    total = sum(r[1] for r in regions) or 1
    done = 0
    # Fast path: allocations that are exactly PS1-RAM shaped.
    for base, size, *_ in regions:
        if size in (0x200000, 0x800000) and validate(proc, base):
            return base
    chunk = 0x1000000
    for base, size, *_ in regions:
        pos, end = base, base + size
        while pos < end:
            n = min(chunk + 80, end - pos)
            data = proc.read(pos, n)
            if data:
                for m in _DECK_RE.finditer(data):
                    # The deck starts somewhere inside this run of valid ids.
                    last = min(m.end() - 80, m.start() + 4000)
                    for start in range(m.start(), last + 1, 2):
                        hit = pos + start
                        for off in (DECK_OFF, MIRROR_OFF):
                            cand = hit - off
                            if cand > 0 and cand % 4 == 0 and validate(proc, cand):
                                return cand
            pos += chunk
        done += size
        if progress:
            progress(done / total)
    return None
