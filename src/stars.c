/* final7 - guardian star icons, read straight from your Forbidden Memories disc image.
 *
 * The ten 16x16 4-bit icons live in WA_MRG.MRG (the "boot sheet"): star N's
 * pixels start at 0xB51840 + ((N-1) % 8) * 8 + ((N-1) / 8) * 0x800, 16 rows
 * 128 bytes apart, with a 16-colour palette at 0xB60500 (see the texture-pack
 * notes in Unchiga's Forbidden Memories decompilation). Works with raw
 * 2352-byte .bin images and plain 2048-byte .iso images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "stars.h"

#define ICON_BASE 0xB51840u
#define CLUT_OFF  0xB60500u

static uint32_t g_icons[STAR_COUNT + 1][16 * 16];   /* 0xAARRGGBB, index = game star id 1..10 */
static int g_have;

typedef struct {
    FILE *f;
    int raw;            /* 2352-byte sectors */
    uint32_t sectors;
} Disc;

static int read_sector(Disc *d, uint32_t lba, uint8_t *out2048)
{
    long long off = (long long)lba * (d->raw ? 2352 : 2048) + (d->raw ? 24 : 0);
    if (_fseeki64(d->f, off, SEEK_SET)) return 0;
    return fread(out2048, 1, 2048, d->f) == 2048;
}

/* Find a file by name anywhere in the ISO9660 tree; depth-limited. */
static int find_file(Disc *d, uint32_t lba, uint32_t size, const char *name, int depth, uint32_t *flba, uint32_t *fsize)
{
    uint8_t sec[2048];
    for (uint32_t done = 0; done < size; done += 2048, lba++) {
        if (!read_sector(d, lba, sec)) return 0;
        for (int p = 0; p < 2048;) {
            int len = sec[p];
            if (!len || p + len > 2048) break;
            uint32_t elba, esize;
            memcpy(&elba, sec + p + 2, 4);
            memcpy(&esize, sec + p + 10, 4);
            int flags = sec[p + 25], nlen = sec[p + 32];
            char nm[64] = {0};
            memcpy(nm, sec + p + 33, nlen < 63 ? nlen : 63);
            char *semi = strchr(nm, ';');
            if (semi) *semi = 0;
            if (nlen == 1 && (nm[0] == 0 || nm[0] == 1)) { p += len; continue; }   /* . and .. */
            if (flags & 2) {
                if (depth < 4 && find_file(d, elba, esize, name, depth + 1, flba, fsize)) return 1;
            } else if (!_stricmp(nm, name)) {
                *flba = elba;
                *fsize = esize;
                return 1;
            }
            p += len;
        }
    }
    return 0;
}

static int read_file_bytes(Disc *d, uint32_t flba, uint32_t off, uint8_t *out, uint32_t n)
{
    uint8_t sec[2048];
    while (n) {
        uint32_t s = off / 2048, o = off % 2048, take = 2048 - o;
        if (take > n) take = n;
        if (!read_sector(d, flba + s, sec)) return 0;
        memcpy(out, sec + o, take);
        out += take; off += take; n -= take;
    }
    return 1;
}

static uint32_t ps1_color(uint16_t c)
{
    if (c == 0) return 0;   /* fully transparent */
    uint32_t r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Open a disc image and locate WA_MRG.MRG; on success d->f stays open. */
static int open_wa(const char *path, Disc *d, uint32_t *flba, uint32_t *fsize, char *err, int errlen)
{
    d->f = fopen(path, "rb");
    d->raw = 0;
    if (!d->f) { snprintf(err, errlen, "Could not open %s", path); return 0; }
    uint8_t head[16];
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (fread(head, 1, 16, d->f) == 16 && !memcmp(head, sync, 12)) d->raw = 1;
    uint8_t pvd[2048];
    if (!read_sector(d, 16, pvd) || memcmp(pvd + 1, "CD001", 5)) {
        fclose(d->f);
        snprintf(err, errlen, "%s is not a PlayStation disc image.", path);
        return 0;
    }
    uint32_t root_lba, root_size;
    memcpy(&root_lba, pvd + 156 + 2, 4);
    memcpy(&root_size, pvd + 156 + 10, 4);
    if (!find_file(d, root_lba, root_size, "WA_MRG.MRG", 0, flba, fsize) || *fsize < CLUT_OFF + 32) {
        fclose(d->f);
        snprintf(err, errlen, "WA_MRG.MRG was not found on this disc. Is it Yu-Gi-Oh! Forbidden Memories (USA)?");
        return 0;
    }
    return 1;
}

int disc_read_wa(const char *path, uint32_t off, void *buf, uint32_t n)
{
    Disc d;
    uint32_t flba, fsize;
    char err[256];
    if (!open_wa(path, &d, &flba, &fsize, err, sizeof err)) return 0;
    int ok = off + n <= fsize && read_file_bytes(&d, flba, off, buf, n);
    fclose(d.f);
    return ok;
}

int disc_find_in_folder(const char *folder, char *out, int outlen)
{
    static const char *const pats[] = { "*.bin", "*.iso", "*.img" };
    for (int k = 0; k < 3; k++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s\\%s", folder, pats[k]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char full[MAX_PATH], err[256];
            Disc d;
            uint32_t flba, fsize;
            snprintf(full, sizeof full, "%s\\%s", folder, fd.cFileName);
            if (open_wa(full, &d, &flba, &fsize, err, sizeof err)) {
                fclose(d.f);
                FindClose(h);
                snprintf(out, outlen, "%s", full);
                return 1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return 0;
}

int stars_load_from_disc(const char *path, char *err, int errlen)
{
    Disc d;
    uint32_t flba, fsize;
    if (!open_wa(path, &d, &flba, &fsize, err, errlen)) return 0;
    uint8_t clut_raw[32];
    uint8_t rows[2][16 * 128];
    int ok = read_file_bytes(&d, flba, CLUT_OFF, clut_raw, 32) &&
             read_file_bytes(&d, flba, ICON_BASE, rows[0], sizeof rows[0]) &&
             read_file_bytes(&d, flba, ICON_BASE + 0x800, rows[1], sizeof rows[1]);
    fclose(d.f);
    if (!ok) { snprintf(err, errlen, "Could not read the star icons from the disc."); return 0; }
    uint32_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = ps1_color((uint16_t)(clut_raw[2 * i] | (clut_raw[2 * i + 1] << 8)));
    int lit = 0;
    for (int star = 1; star <= STAR_COUNT; star++) {
        const uint8_t *blk = rows[(star - 1) / 8] + ((star - 1) % 8) * 8;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t b = blk[y * 128 + x / 2];
                uint32_t c = pal[(x & 1) ? (b >> 4) : (b & 15)];
                g_icons[star][y * 16 + x] = c;
                lit += c != 0;
            }
    }
    if (lit < 100) { snprintf(err, errlen, "The disc's star icons came out empty."); return 0; }
    g_have = 1;
    return 1;
}

int stars_have(void) { return g_have; }

int stars_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("FMS1", 1, 4, f);
    fwrite(g_icons, sizeof g_icons, 1, f);
    fclose(f);
    return 1;
}

int stars_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char m[4];
    int ok = fread(m, 1, 4, f) == 4 && !memcmp(m, "FMS1", 4) && fread(g_icons, sizeof g_icons, 1, f) == 1;
    fclose(f);
    g_have = ok;
    return ok;
}

/* Look for a disc image next to the exe and pull the icons from it. */
int stars_autoload(const char *folder)
{
    char path[MAX_PATH], err[256];
    return disc_find_in_folder(folder, path, sizeof path) && stars_load_from_disc(path, err, sizeof err);
}

/* Draw star `id` (1..10) scaled to w x h over background `bg`, pixel-sharp. */
void stars_draw(HDC dc, int id, int x, int y, int w, int h, COLORREF bg)
{
    if (!g_have || id < 1 || id > STAR_COUNT) return;
    uint32_t px[16 * 16];
    uint32_t back = (GetRValue(bg) << 16) | (GetGValue(bg) << 8) | GetBValue(bg);
    for (int i = 0; i < 256; i++) px[i] = g_icons[id][i] ? (g_icons[id][i] & 0xFFFFFF) : back;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 16, -16, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    int old = SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, 16, 16, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, old);
}
