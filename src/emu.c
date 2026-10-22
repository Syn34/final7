/* FM Tracker - read-only access to PS1 emulators and locating PS1 main RAM.
 *
 * Pointer/signature methods adapted from emu-help by Jujstme (MIT),
 * https://github.com/Jujstme/emu-help. Anything unrecognised falls back to
 * scanning the process for a loaded Forbidden Memories save.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm.h"

#define CHUNK 0x1000000

static const struct { const char *hint; int kind; } HINTS[] = {
    { "duckstation", EMU_DUCKSTATION }, { "epsxe", EMU_EPSXE }, { "retroarch", EMU_RETROARCH },
    { "mednafen", EMU_MEDNAFEN }, { "psxfin", EMU_PSX }, { "pcsx-redux", EMU_PCSXREDUX },
    { "xebra", EMU_XEBRA }, { "emuhawk", EMU_GENERIC }, { "pcsx", EMU_GENERIC }, { "nopsx", EMU_GENERIC },
};

static void lower(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) d[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    d[i] = 0;
}

int emu_find(EmuProc *out, int max)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    int n = 0;
    for (BOOL ok = Process32First(snap, &pe); ok && n < max; ok = Process32Next(snap, &pe)) {
        char low[260];
        lower(low, pe.szExeFile, sizeof low);
        for (size_t i = 0; i < sizeof HINTS / sizeof HINTS[0]; i++) {
            if (strstr(low, HINTS[i].hint)) {
                out[n].pid = pe.th32ProcessID;
                snprintf(out[n].exe, sizeof out[n].exe, "%s", pe.szExeFile);
                out[n].kind = HINTS[i].kind;
                n++;
                break;
            }
        }
    }
    CloseHandle(snap);
    return n;
}

/* ---- raw reads ------------------------------------------------------------ */
static int rd(Attach *a, uint64_t addr, void *buf, size_t n)
{
    SIZE_T got = 0;
    if (!addr) return 0;
    return ReadProcessMemory(a->handle, (LPCVOID)(uintptr_t)addr, buf, n, &got) && got == n;
}

static uint64_t rd_ptr(Attach *a, uint64_t addr)
{
    uint64_t v = 0;
    if (!rd(a, addr, &v, a->is64 ? 8 : 4)) return 0;
    return v;
}

static int32_t rd_i32(Attach *a, uint64_t addr, int *ok)
{
    int32_t v = 0;
    *ok = rd(a, addr, &v, 4);
    return v;
}

static uint64_t rel32(Attach *a, uint64_t addr)
{
    int ok;
    int32_t off = rd_i32(a, addr, &ok);
    return ok ? addr + 4 + (int64_t)off : 0;
}

static int readable(const MEMORY_BASIC_INFORMATION *m)
{
    DWORD p = m->Protect & 0xFF;
    return m->State == MEM_COMMIT && !(m->Protect & PAGE_GUARD) &&
           (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
            p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
}

/* ---- modules -------------------------------------------------------------- */
static int find_module(Attach *a, const char *name, uint64_t *base, uint64_t *size)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, a->pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    int found = 0;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (!name || !_stricmp(me.szModule, name)) {
            *base = (uint64_t)(uintptr_t)me.modBaseAddr;
            *size = me.modBaseSize;
            found = 1;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

static int main_module(Attach *a, uint64_t *base, uint64_t *size)
{
    return find_module(a, a->exe, base, size) || find_module(a, NULL, base, size);
}

/* ---- signature scanning ---------------------------------------------------- */
typedef struct { uint8_t b[64]; uint8_t m[64]; int n; } Sig;

static void sig_parse(const char *s, Sig *sig)
{
    sig->n = 0;
    while (*s && sig->n < 64) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '?') {
            sig->b[sig->n] = 0;
            sig->m[sig->n++] = 0;
            while (*s == '?') s++;
        } else {
            sig->b[sig->n] = (uint8_t)strtoul(s, NULL, 16);
            sig->m[sig->n++] = 1;
            s += 2;
        }
    }
}

static uint64_t sig_scan(Attach *a, uint64_t base, uint64_t size, const char *pattern)
{
    Sig sig;
    sig_parse(pattern, &sig);
    uint8_t *buf = malloc(CHUNK + 64);
    if (!buf) return 0;
    uint64_t end = base + size, addr = base, hit = 0;
    MEMORY_BASIC_INFORMATION m;
    while (!hit && addr < end && VirtualQueryEx(a->handle, (LPCVOID)(uintptr_t)addr, &m, sizeof m)) {
        uint64_t rb = (uint64_t)(uintptr_t)m.BaseAddress, rs = m.RegionSize;
        if (!rs) break;
        if (readable(&m)) {
            uint64_t lo = rb > base ? rb : base, hi = rb + rs < end ? rb + rs : end;
            for (uint64_t pos = lo; !hit && pos < hi; pos += CHUNK) {
                size_t n = (size_t)((hi - pos) < (uint64_t)CHUNK + 64 ? (hi - pos) : (uint64_t)CHUNK + 64);
                if (!rd(a, pos, buf, n)) continue;
                for (size_t i = 0; i + sig.n <= n; i++) {
                    if (buf[i] != sig.b[0]) continue;
                    int k = 1;
                    for (; k < sig.n; k++)
                        if (sig.m[k] && buf[i + k] != sig.b[k]) break;
                    if (k == sig.n) { hit = pos + i; break; }
                }
            }
        }
        addr = rb + rs;
    }
    free(buf);
    return hit;
}

/* ---- PE export lookup ------------------------------------------------------ */
static uint64_t find_export(Attach *a, uint64_t mod, const char *sym)
{
    uint8_t hdr[0x1000];
    if (!rd(a, mod, hdr, sizeof hdr) || hdr[0] != 'M' || hdr[1] != 'Z') return 0;
    uint32_t lfanew = *(uint32_t *)(hdr + 0x3C);
    if (lfanew > sizeof hdr - 0x100) return 0;
    uint16_t magic = *(uint16_t *)(hdr + lfanew + 24);
    uint32_t dd = lfanew + 24 + (magic == 0x20B ? 112 : 96);
    uint32_t exp_rva = *(uint32_t *)(hdr + dd);
    if (!exp_rva) return 0;
    uint32_t ed[10];
    if (!rd(a, mod + exp_rva, ed, sizeof ed)) return 0;
    uint32_t n_funcs = ed[5], n_names = ed[6], a_funcs = ed[7], a_names = ed[8], a_ords = ed[9];
    if (n_names > 100000 || n_funcs > 100000) return 0;
    uint32_t *names = malloc(4 * n_names + 4), *funcs = malloc(4 * n_funcs + 4);
    uint16_t *ords = malloc(2 * n_names + 2);
    uint64_t result = 0;
    size_t sl = strlen(sym);
    if (names && funcs && ords && rd(a, mod + a_names, names, 4 * n_names) &&
        rd(a, mod + a_ords, ords, 2 * n_names) && rd(a, mod + a_funcs, funcs, 4 * n_funcs)) {
        for (uint32_t i = 0; i < n_names; i++) {
            char nm[64];
            if (!rd(a, mod + names[i], nm, sl + 1)) continue;
            if (!memcmp(nm, sym, sl + 1) && ords[i] < n_funcs) {
                result = mod + funcs[ords[i]];
                break;
            }
        }
    }
    free(names);
    free(funcs);
    free(ords);
    return result;
}

/* ---- per-emulator finders ------------------------------------------------- */
static int f_duckstation(Attach *a)
{
    uint64_t b, s;
    if (!main_module(a, &b, &s)) return 0;
    uint64_t sym = find_export(a, b, "RAM");
    if (sym) {
        a->ptr_addr = sym;
        snprintf(a->method, sizeof a->method, "DuckStation (RAM export)");
        return 1;
    }
    uint64_t hit = sig_scan(a, b, s, "48 89 0D ?? ?? ?? ?? B8");
    if (hit && (a->ptr_addr = rel32(a, hit + 3))) {
        snprintf(a->method, sizeof a->method, "DuckStation (signature)");
        return 1;
    }
    return 0;
}

static int f_epsxe(Attach *a)
{
    uint64_t b, s;
    if (!main_module(a, &b, &s)) return 0;
    uint64_t hit = sig_scan(a, b, s, "C1 E1 10 8D 89");
    uint32_t v = 0;
    if (hit && rd(a, hit + 5, &v, 4) && v) {
        a->static_base = v;
        snprintf(a->method, sizeof a->method, "ePSXe (signature)");
        return 1;
    }
    return 0;
}

static int f_retroarch(Attach *a)
{
    static const struct { const char *dll, *s64, *s32; int off32; } cores[] = {
        { "mednafen_psx_hw_libretro.dll", "48 8B 05 ?? ?? ?? ?? 41 81 E4 FF FF 1F 00", "A1 ?? ?? ?? ?? 81 E3 FF FF 1F 00", 1 },
        { "mednafen_psx_libretro.dll", "48 8B 05 ?? ?? ?? ?? 41 81 E4 FF FF 1F 00", "A1 ?? ?? ?? ?? 81 E3 FF FF 1F 00", 1 },
        { "swanstation_libretro.dll", "48 89 0D ?? ?? ?? ?? 89 35 ?? ?? ?? ?? 89 3D", "A1 ?? ?? ?? ?? 23 CB 8B", 1 },
        { "pcsx_rearmed_libretro.dll", "48 8B 35 ?? ?? ?? ?? 81 E2", "FF FF 1F 00 89 ?? ?? ?? A1", 9 },
    };
    for (size_t i = 0; i < sizeof cores / sizeof cores[0]; i++) {
        uint64_t b, s;
        if (!find_module(a, cores[i].dll, &b, &s)) continue;
        uint64_t hit = sig_scan(a, b, s, a->is64 ? cores[i].s64 : cores[i].s32);
        if (!hit) continue;
        a->ptr_addr = a->is64 ? rel32(a, hit + 3) : rd_ptr(a, hit + cores[i].off32);
        if (a->ptr_addr) {
            snprintf(a->method, sizeof a->method, "RetroArch (%s)", cores[i].dll);
            return 1;
        }
    }
    return 0;
}

static int f_mednafen(Attach *a)
{
    uint64_t b, s;
    if (!main_module(a, &b, &s)) return 0;
    uint64_t hit = sig_scan(a, b, s, a->is64 ? "89 01 0F B6 82" : "89 01 0F B6 82 ?? ?? ?? ?? C3");
    uint32_t v = 0;
    if (hit && rd(a, hit + 5, &v, 4) && v) {
        a->static_base = v;
        snprintf(a->method, sizeof a->method, "Mednafen (signature)");
        return 1;
    }
    return 0;
}

static int f_psx(Attach *a)
{
    static const struct { int off; const char *sig; } sigs[] = {
        { 2, "8B 15 ?? ?? ?? ?? 8D 34 1A" }, { 1, "A1 ?? ?? ?? ?? 8D 34 18" },
        { 1, "A1 ?? ?? ?? ?? 8B 7C 24 14" }, { 1, "A1 ?? ?? ?? ?? 8B 6C 24" },
    };
    uint64_t b, s;
    if (!main_module(a, &b, &s)) return 0;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        uint64_t hit = sig_scan(a, b, s, sigs[i].sig);
        if (hit && (a->ptr_addr = rd_ptr(a, hit + sigs[i].off))) {
            snprintf(a->method, sizeof a->method, "pSX (signature)");
            return 1;
        }
    }
    return 0;
}

static int f_pcsxredux(Attach *a)
{
    uint64_t b, s;
    if (!a->is64 || !main_module(a, &b, &s)) return 0;
    uint64_t h1 = sig_scan(a, b, s, "48 B9 ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 00 00 00 00");
    uint64_t h2 = sig_scan(a, b, s, "89 D1 C1 E9 10 48 8B");
    uint8_t off;
    if (h1 && h2 && rd(a, h2 + 8, &off, 1)) {
        uint64_t obj = rd_ptr(a, h1 + 2);
        if (obj) {
            a->ptr_addr = obj + off;
            snprintf(a->method, sizeof a->method, "PCSX-Redux (signature)");
            return 1;
        }
    }
    return 0;
}

static int f_xebra(Attach *a)
{
    uint64_t b, s;
    if (!main_module(a, &b, &s)) return 0;
    uint64_t hit = sig_scan(a, b, s, "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 89 C8 C1 F8 10");
    if (!hit) return 0;
    uint64_t tgt = rel32(a, hit + 1);
    uint64_t inner = tgt ? rd_ptr(a, tgt + 0x16A) : 0;
    if (inner) {
        a->ptr_addr = inner;
        snprintf(a->method, sizeof a->method, "Xebra (signature)");
        return 1;
    }
    return 0;
}

/* ---- validation + generic scan -------------------------------------------- */
static int validate(Attach *a, uint64_t base)
{
    uint8_t deck[80], mirror[80], opp, trunk[N_CARDS], edge[16];
    uint32_t chips;
    if (base & 15) return 0;   /* emulators allocate guest RAM at least 16-byte aligned */
    if (!(rd(a, base + RAM_DECK, deck, 80) && rd(a, base + RAM_DECK_MIRROR, mirror, 80) &&
          rd(a, base + RAM_STARCHIPS, &chips, 4) && rd(a, base + RAM_OPPONENT, &opp, 1) &&
          ram_looks_like_save(deck, mirror, chips, opp)))
        return 0;
    if (!rd(a, base + RAM_TRUNK, trunk, N_CARDS) || !rd(a, base, edge, 16) ||
        !rd(a, base + PS1_RAM_SIZE - 16, edge, 16))
        return 0;
    for (int i = 0; i < N_CARDS; i++)
        if (trunk[i] > 250) return 0;   /* the chest holds at most 250 of a card */
    uint8_t copies[N_CARDS + 1] = {0};
    for (int i = 0; i < 40; i++) {
        int id = deck[2 * i] | (deck[2 * i + 1] << 8);
        if (++copies[id] > 3) return 0; /* a deck holds at most 3 of a card */
    }
    return 1;
}

static int valid_id(const uint8_t *p)
{
    unsigned v = p[0] | (p[1] << 8);
    return v >= 1 && v <= N_CARDS;
}

static uint64_t generic_scan(Attach *a, volatile int *progress)
{
    uint64_t limit = a->is64 ? 0x7FFFFFFFFFFFull : 0xFFFFFFFFull;
    uint64_t total = 0, done = 0, addr = 0;
    MEMORY_BASIC_INFORMATION m;
    /* pass 1: size the work + try PS1-RAM-shaped allocations */
    while (addr < limit && VirtualQueryEx(a->handle, (LPCVOID)(uintptr_t)addr, &m, sizeof m)) {
        uint64_t rb = (uint64_t)(uintptr_t)m.BaseAddress, rs = m.RegionSize;
        if (!rs) break;
        if (readable(&m) && rs >= 0x10000) {
            total += rs;
            if ((rs == 0x200000 || rs == 0x800000) && validate(a, rb)) return rb;
        }
        addr = rb + rs;
    }
    if (!total) return 0;
    uint8_t *buf = malloc(CHUNK + 128);
    if (!buf) return 0;
    uint64_t found = 0;
    addr = 0;
    while (!found && addr < limit && VirtualQueryEx(a->handle, (LPCVOID)(uintptr_t)addr, &m, sizeof m)) {
        uint64_t rb = (uint64_t)(uintptr_t)m.BaseAddress, rs = m.RegionSize;
        if (!rs) break;
        uint64_t own = (uint64_t)(uintptr_t)buf;
        int self_buf = a->pid == GetCurrentProcessId() && own >= rb && own < rb + rs;  /* our own copy (tests) */
        if (readable(&m) && rs >= 0x10000 && !self_buf && m.Type != MEM_IMAGE) {
            for (uint64_t pos = rb; !found && pos < rb + rs; pos += CHUNK) {
                size_t n = (size_t)((rb + rs - pos) < (uint64_t)CHUNK + 128 ? (rb + rs - pos) : (uint64_t)CHUNK + 128);
                if (!rd(a, pos, buf, n)) continue;
                /* runs of >= 40 consecutive valid card ids (u16, even addresses) */
                size_t run = 0;
                for (size_t i = 0; i + 2 <= n && !found; i += 2) {
                    if (valid_id(buf + i)) {
                        run++;
                        if (run >= 40) {
                            uint64_t start = pos + i + 2 - 80;      /* deck would start here */
                            uint64_t c1 = start - RAM_DECK, c2 = start - RAM_DECK_MIRROR;
                            if (start >= RAM_DECK_MIRROR) {
                                if (!(c1 & 15) && validate(a, c1)) found = c1;
                                else if (!(c2 & 15) && validate(a, c2)) found = c2;
                            }
                        }
                    } else {
                        run = 0;
                    }
                }
            }
            done += rs;
            if (progress) *progress = (int)(done * 100 / total);
        }
        addr = rb + rs;
    }
    free(buf);
    return found;
}

/* ---- public API ------------------------------------------------------------ */
static uint64_t resolve(Attach *a)
{
    if (a->static_base) return a->static_base;
    if (a->ptr_addr) return rd_ptr(a, a->ptr_addr);
    return 0;
}

int emu_attach(Attach *a, const EmuProc *p, char *err, int errlen, volatile int *progress)
{
    memset(a, 0, sizeof *a);
    a->pid = p->pid;
    snprintf(a->exe, sizeof a->exe, "%s", p->exe);
    a->handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, p->pid);
    if (!a->handle) a->handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
    if (!a->handle) {
        snprintf(err, errlen, "Could not open %s (pid %lu). If the emulator runs as administrator, "
                 "run FM Tracker as administrator too.", p->exe, p->pid);
        return 0;
    }
    BOOL wow = FALSE;
    a->is64 = !(IsWow64Process(a->handle, &wow) && wow);

    int (*finder)(Attach *) = NULL;
    switch (p->kind) {
    case EMU_DUCKSTATION: finder = f_duckstation; break;
    case EMU_EPSXE: finder = f_epsxe; break;
    case EMU_RETROARCH: finder = f_retroarch; break;
    case EMU_MEDNAFEN: finder = f_mednafen; break;
    case EMU_PSX: finder = f_psx; break;
    case EMU_PCSXREDUX: finder = f_pcsxredux; break;
    case EMU_XEBRA: finder = f_xebra; break;
    default: break;
    }
    if (finder && finder(a)) {
        uint64_t base = resolve(a);
        static uint8_t probe[16];
        if (base && rd(a, base, probe, sizeof probe) && rd(a, base + PS1_RAM_SIZE - 16, probe, sizeof probe))
            return 1;
        if (a->ptr_addr && !base)
            return 1;   /* emulator found, no game booted yet: RAM pointer fills in later */
    }
    a->ptr_addr = a->static_base = 0;
    uint64_t base = generic_scan(a, progress);
    if (base) {
        a->static_base = base;
        snprintf(a->method, sizeof a->method, "memory scan");
        return 1;
    }
    snprintf(err, errlen, "Found %s but not the game's memory. Load your Forbidden Memories save "
             "(get to the map or deck screen), then press Attach again.", p->exe);
    CloseHandle(a->handle);
    a->handle = NULL;
    return 0;
}

int emu_read_ram(Attach *a, uint8_t *buf)
{
    uint64_t base = resolve(a);
    return base && rd(a, base, buf, PS1_RAM_SIZE);
}

int emu_alive(Attach *a)
{
    DWORD code = 0;
    return a->handle && GetExitCodeProcess(a->handle, &code) && code == STILL_ACTIVE;
}

void emu_detach(Attach *a)
{
    if (a->handle) CloseHandle(a->handle);
    memset(a, 0, sizeof *a);
}
