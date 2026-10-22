/* final7 - card / type pictures.
 *
 * The 725 pictures (722 cards + Dragon, Equip and Thunder type icons) are the
 * set used by Card Overview v2.1 by @mergy_, packed into the exe as RCDATA 2
 * (see tools/build_art.py). PNGs are decoded on demand with GDI+.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <string.h>

#include "art.h"

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GpStartupInput;

int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
int WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
int WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *hbm, DWORD background);
int WINAPI GdipDisposeImage(void *image);

static const unsigned char *g_blob;
static DWORD g_blob_len;
static struct { uint32_t off, len; } g_index[ART_ITEMS + 1];
static HBITMAP g_cache[ART_ITEMS + 1];
static int g_ready;

int art_init(void)
{
    ULONG_PTR token;
    GpStartupInput in = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&token, &in, NULL) != 0) return 0;
    HRSRC r = FindResource(NULL, MAKEINTRESOURCE(2), RT_RCDATA);
    if (!r) return 0;
    HGLOBAL h = LoadResource(NULL, r);
    g_blob = h ? LockResource(h) : NULL;
    g_blob_len = SizeofResource(NULL, r);
    if (!g_blob || g_blob_len < 8 || memcmp(g_blob, "FMA1", 4)) return 0;
    uint32_t n;
    memcpy(&n, g_blob + 4, 4);
    for (uint32_t i = 0; i < n && 8 + 10 * (i + 1) <= g_blob_len; i++) {
        const unsigned char *e = g_blob + 8 + 10 * i;
        uint16_t item;
        uint32_t off, len;
        memcpy(&item, e, 2);
        memcpy(&off, e + 2, 4);
        memcpy(&len, e + 6, 4);
        if (item >= 1 && item <= ART_ITEMS && off + len <= g_blob_len) {
            g_index[item].off = off;
            g_index[item].len = len;
        }
    }
    g_ready = 1;
    return 1;
}

HBITMAP art_get(int item)
{
    if (!g_ready || item < 1 || item > ART_ITEMS || !g_index[item].len) return NULL;
    if (g_cache[item]) return g_cache[item];
    IStream *st = SHCreateMemStream(g_blob + g_index[item].off, g_index[item].len);
    if (!st) return NULL;
    void *bmp = NULL;
    HBITMAP hb = NULL;
    if (GdipCreateBitmapFromStream(st, &bmp) == 0 && bmp) {
        GdipCreateHBITMAPFromBitmap(bmp, &hb, 0xFF000000);
        GdipDisposeImage(bmp);
    }
    IStream_Release(st);
    g_cache[item] = hb;
    return hb;
}

void art_draw(HDC dc, int item, int x, int y, int w, int h)
{
    HBITMAP hb = art_get(item);
    if (!hb) return;
    BITMAP bm;
    GetObject(hb, sizeof bm, &bm);
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, hb);
    int prev = SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, w, h, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    SetStretchBltMode(dc, prev);
    SelectObject(mem, old);
    DeleteDC(mem);
}

HIMAGELIST art_imagelist(int w, int h)
{
    HIMAGELIST il = ImageList_Create(w, h, ILC_COLOR32, ART_ITEMS, 0);
    if (!il) return NULL;
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    for (int item = 1; item <= ART_ITEMS; item++) {
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
        void *bits;
        HBITMAP thumb = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(dc, thumb);
        RECT r = { 0, 0, w, h };
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        art_draw(dc, item, 0, 0, w, h);
        SelectObject(dc, old);
        /* opaque alpha so ILC_COLOR32 does not treat the picture as transparent */
        for (int i = 0; i < w * h; i++) ((unsigned char *)bits)[i * 4 + 3] = 0xFF;
        ImageList_Add(il, thumb, NULL);
        DeleteObject(thumb);
        /* free the full-size decode; the summary strip re-decodes the few it needs */
        if (g_cache[item]) { DeleteObject(g_cache[item]); g_cache[item] = NULL; }
    }
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return il;
}
