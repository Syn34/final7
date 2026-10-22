/* final7 - Win32 user interface.
 *
 * One window, seven pages (Summary, Duel & Rank, Card Overview, Drops, Duelists, Passwords, Log)
 * and an optional always-on-top overlay. Everything refreshes from a 250 ms timer
 * that copies the emulator's PS1 RAM and parses it (see game.c / emu.c).
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fm.h"
#include "art.h"
#include "stars.h"
#include <commdlg.h>

/* ---- palette --------------------------------------------------------------- */
#define C_BG     RGB(0x0e, 0x13, 0x20)
#define C_PANEL  RGB(0x16, 0x1d, 0x2e)
#define C_PANEL2 RGB(0x1d, 0x26, 0x3b)
#define C_LINE   RGB(0x2a, 0x35, 0x50)
#define C_TEXT   RGB(0xe9, 0xe4, 0xd4)
#define C_MUTED  RGB(0x8f, 0x98, 0xad)
#define C_GOLD   RGB(0xd9, 0xad, 0x4f)
#define C_GREEN  RGB(0x5f, 0xcf, 0x86)
#define C_RED    RGB(0xef, 0x6b, 0x6b)
#define C_BLUE   RGB(0x6f, 0xa8, 0xff)
#define C_POW    RGB(0xff, 0x4d, 0x4d)   /* POW is always red  */
#define C_TEC    RGB(0x6f, 0xb4, 0xff)   /* TEC is always blue */
#define C_UPBG   RGB(0x1e, 0x46, 0x30)
#define C_DOWNBG RGB(0x4a, 0x20, 0x26)
#define C_CURBG  RGB(0x3a, 0x32, 0x20)

static COLORREF letter_color(char c)
{
    switch (c) {
    case 'S': return RGB(0xb4, 0x6e, 0xff);   /* purple */
    case 'A': return C_POW;                   /* red    */
    default:  return C_TEC;                   /* B, C, D: blue */
    }
}

/* ---- control ids ------------------------------------------------------------ */
enum {
    ID_REFRESH = 101, ID_ATTACH, ID_DETACH, ID_DEMO, ID_OVERLAY, ID_MENU, ID_STARDISC,
    ID_TAB0 = 110,
    ID_TRACK = 120, ID_UNTRACK, ID_MOVEUP, ID_MOVEDN, ID_CLEAR, ID_RESETDEF,
    ID_PW_UNOWNED = 130, ID_PW_AFFORD, ID_PW_FAVONLY, ID_PW_FAV,
    ID_CARD_SEARCH = 140, ID_PW_SEARCH,
    ID_EMU = 150, ID_TIER,
    ID_LOG = 160,
    ID_LIST = 200,
};
enum { L_RANK, L_ALL, L_TRACK, L_DETAIL, L_PW, L_SUM_FAV, L_DROPDUEL, N_LISTS };
enum { PG_SUMMARY, PG_CARDS, PG_DROPS, PG_PW, N_PAGES };
static const char *const PAGE_NAMES[N_PAGES] = { "Summary", "Card Overview", "Drops", "Passwords" };
#define APP_NAME "final7"

#define WM_APP_ATTACHED (WM_APP + 1)
#define TIMER_POLL 1

/* ---- globals ---------------------------------------------------------------- */
static HINSTANCE g_inst;
static HWND g_main, g_pages[N_PAGES], g_sumcard, g_strip, g_stars, g_overlay;
static HIMAGELIST g_il;
static HWND g_emu_combo, g_tier_combo, g_card_search, g_pw_search, g_log;
static HWND g_btn[64];
static int g_page;
static int g_dpi = 96;
static HFONT f_ui, f_bold, f_small, f_smallb, f_title, f_huge, f_big, f_med, f_lp, f_lpnum, f_mono, f_count;
static HBRUSH br_bg, br_panel, br_panel2;

static uint8_t *g_ram;
static Snapshot g_snap;
static int g_have_snap;
static int g_mode;               /* 0 none, 1 live, 2 demo */
static Attach g_att;
static DuelTracker g_tr;
static volatile int g_attaching, g_progress;
static Attach g_pending_att;
static char g_attach_err[400];
static EmuProc g_emus[32];
static int g_nemus;
static char g_status[256] = "Select attach from dropdown to connect";
static COLORREF g_status_color = C_RED;

#define MAX_TRACK 7
static const int DEFAULT_TRACK[MAX_TRACK] = { ITEM_DRAGON, ITEM_THUNDER, 315, 332, 686, 713, 657 };
static int g_tracked[MAX_TRACK], g_ntracked;          /* Card Overview items shown on Summary */
static int g_iprev[ART_ITEMS + 1], g_ichg_delta[ART_ITEMS + 1], g_have_iprev;
static uint64_t g_ichg_time[ART_ITEMS + 1];
static int g_prev[N_CARDS + 1], g_have_prev;

typedef struct {
    char when[20];
    int opp, win, kind, score, turns, lp, free_duel, drop;
} Hist;
static Hist *g_hist;
static int g_nhist, g_caphist;
static int g_pend = -1;             /* history index still collecting drop / free-duel info */
static uint64_t g_pend_t;
static uint16_t g_pend_w, g_pend_l, g_pend_drops[10];
static int g_pend_ndrops;

static uint16_t (*g_pools)[4][N_CARDS];   /* [duelist 1..39][deck, sapow, bcd, satec] */
static uint8_t g_have_pool[N_DUELISTS + 1];

static int g_assume_win = 1, g_pw_unowned, g_pw_afford, g_pw_favonly;
static uint8_t g_fav[N_CARDS + 1];
static int g_favs[N_CARDS], g_nfavs;
static char g_dir[MAX_PATH];

typedef struct { int fw, fl, sw, sl, w, l, best; double chance; } DStat;
static DStat g_dstat[N_DUELISTS + 1];
static uint64_t g_dstat_t;

static const uint16_t *g_drop_pool;     /* live drop table shown on the duel page */
static int g_drop_tier = -1, g_drop_opp = -1;
static const uint16_t *g_detail_pool;
static int g_detail_duelist;

/* texts painted on pages */
static char t_tracked_sum[96], t_last_drops[512], t_pw_count[96], t_detail_title[64], t_fav_sum[96],
            t_detail_note[200];

#define S(x) MulDiv((x), g_dpi, 96)
#define HEADER_H 48   /* title bar height, tabs sit below it */
#define STRIP_H 124   /* summary picture strip: 96 px tiles + name */

static uint64_t now_ms(void) { return GetTickCount64(); }

/* ---- logging ---------------------------------------------------------------- */
static void logf_(const char *fmt, ...)
{
    char msg[600], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(line, sizeof line, "[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, msg);
    if (!g_log) return;
    int len = GetWindowTextLength(g_log);
    if (len > 200000) { SendMessage(g_log, EM_SETSEL, 0, 100000); SendMessage(g_log, EM_REPLACESEL, 0, (LPARAM)""); len = GetWindowTextLength(g_log); }
    SendMessage(g_log, EM_SETSEL, len, len);
    SendMessage(g_log, EM_REPLACESEL, 0, (LPARAM)line);
}

static const char *card_name(int id) { return (id >= 1 && id <= N_CARDS) ? CARDS[id - 1].name : "?"; }
static const char *duelist_name(int d) { return (d >= 1 && d <= N_DUELISTS) ? DUELISTS[d - 1] : "?"; }
static int snap_ok(void) { return g_have_snap && g_snap.save_loaded; }

/* ---- persistence -------------------------------------------------------------- */
static void data_path(char *out, const char *file) { snprintf(out, MAX_PATH, "%s\\%s", g_dir, file); }

static void save_tracked(void)
{
    char p[MAX_PATH];
    data_path(p, "overview.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < g_ntracked; i++) fprintf(f, "%d\n", g_tracked[i]);
    fclose(f);
}

static void load_tracked(void)
{
    char p[MAX_PATH];
    data_path(p, "overview.txt");
    FILE *f = fopen(p, "r");
    g_ntracked = 0;
    if (!f) {
        for (int i = 0; i < MAX_TRACK; i++) g_tracked[g_ntracked++] = DEFAULT_TRACK[i];
        return;
    }
    int id;
    while (g_ntracked < MAX_TRACK && fscanf(f, "%d", &id) == 1)
        if (id >= 1 && id <= ART_ITEMS) g_tracked[g_ntracked++] = id;
    fclose(f);
}

static void save_favs(void)
{
    char p[MAX_PATH];
    data_path(p, "favorites.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int id = 1; id <= N_CARDS; id++)
        if (g_fav[id]) fprintf(f, "%d\n", id);
    fclose(f);
}

static void load_favs(void)
{
    char p[MAX_PATH];
    data_path(p, "favorites.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    int id;
    while (fscanf(f, "%d", &id) == 1)
        if (id >= 1 && id <= N_CARDS) g_fav[id] = 1;
    fclose(f);
}

static void save_history(void)
{
    char p[MAX_PATH];
    data_path(p, "history.csv");
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "when;opponent;win;kind;score;turns;lp;free_duel;drop\n");
    for (int i = 0; i < g_nhist; i++) {
        Hist *h = &g_hist[i];
        fprintf(f, "%s;%d;%d;%d;%d;%d;%d;%d;%d\n", h->when, h->opp, h->win, h->kind, h->score, h->turns, h->lp,
                h->free_duel, h->drop);
    }
    fclose(f);
}

static Hist *hist_add(void)
{
    if (g_nhist == g_caphist) {
        int cap = g_caphist ? g_caphist * 2 : 256;
        Hist *n = realloc(g_hist, cap * sizeof *n);
        if (!n) return NULL;
        g_hist = n;
        g_caphist = cap;
    }
    Hist *h = &g_hist[g_nhist++];
    memset(h, 0, sizeof *h);
    return h;
}

static void load_history(void)
{
    char p[MAX_PATH], line[256];
    data_path(p, "history.csv");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Hist t;
        memset(&t, 0, sizeof t);
        char when[20];
        if (sscanf(line, "%19[^;];%d;%d;%d;%d;%d;%d;%d;%d", when, &t.opp, &t.win, &t.kind, &t.score, &t.turns,
                   &t.lp, &t.free_duel, &t.drop) == 9 && t.opp >= 1 && t.opp <= N_DUELISTS) {
            Hist *h = hist_add();
            if (!h) break;
            *h = t;
            snprintf(h->when, sizeof h->when, "%s", when);
        }
    }
    fclose(f);
}

static void save_pools(void)
{
    char p[MAX_PATH];
    data_path(p, "duelist_tables.bin");
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite("FMP1", 1, 4, f);
    fwrite(g_have_pool, 1, sizeof g_have_pool, f);
    fwrite(g_pools, sizeof g_pools[0], N_DUELISTS + 1, f);
    fclose(f);
}

static void load_pools(void)
{
    char p[MAX_PATH], magic[4];
    data_path(p, "duelist_tables.bin");
    FILE *f = fopen(p, "rb");
    if (!f) return;
    if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "FMP1", 4) &&
        fread(g_have_pool, 1, sizeof g_have_pool, f) == sizeof g_have_pool &&
        fread(g_pools, sizeof g_pools[0], N_DUELISTS + 1, f) == N_DUELISTS + 1) {
        fclose(f);
        return;
    }
    memset(g_have_pool, 0, sizeof g_have_pool);
    fclose(f);
}

static void save_settings(void)
{
    char p[MAX_PATH];
    data_path(p, "settings.ini");
    FILE *f = fopen(p, "w");
    if (!f) return;
    WINDOWPLACEMENT wp = { sizeof wp };
    GetWindowPlacement(g_main, &wp);
    RECT r = wp.rcNormalPosition;
    fprintf(f, "assume_win=%d\nx=%ld\ny=%ld\nw=%ld\nh=%ld\nmax=%d\n", g_assume_win, r.left, r.top,
            r.right - r.left, r.bottom - r.top, wp.showCmd == SW_SHOWMAXIMIZED);
    fclose(f);
}

static int g_set_x = CW_USEDEFAULT, g_set_y = CW_USEDEFAULT, g_set_w, g_set_h, g_set_max;

static void load_settings(void)
{
    char p[MAX_PATH], line[128];
    data_path(p, "settings.ini");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "assume_win=%d", &v) == 1) g_assume_win = v;
        else if (sscanf(line, "x=%d", &v) == 1) g_set_x = v;
        else if (sscanf(line, "y=%d", &v) == 1) g_set_y = v;
        else if (sscanf(line, "w=%d", &v) == 1) g_set_w = v;
        else if (sscanf(line, "h=%d", &v) == 1) g_set_h = v;
        else if (sscanf(line, "max=%d", &v) == 1) g_set_max = v;
    }
    fclose(f);
}

/* ---- virtual list views ------------------------------------------------------- */
typedef struct VList {
    HWND hwnd;
    int ncols;
    const char *const *cols;
    const int *widths;
    const char *align;   /* 'l' or 'c' per column */
    int *rows, nrows;
    void (*text)(int item, int col, char *buf, int n);
    COLORREF (*color)(int item, COLORREF *bg);
    int sort_col, sort_desc;
} VList;
static VList g_lists[N_LISTS];

static void vlist_set_rows(VList *v, const int *items, int n);
static void vlist_fit(VList *v);

static double num_of(const char *s, int *ok)
{
    char tmp[64];
    int j = 0;
    for (; *s && j < 63; s++)
        if (*s != '%' && *s != ',' && *s != '+' && *s != ' ') tmp[j++] = *s;
    tmp[j] = 0;
    char *end;
    double d = strtod(tmp, &end);
    *ok = j > 0 && *end == 0;
    return d;
}

static VList *g_sorting;
static int vlist_cmp(const void *a, const void *b)
{
    char x[128], y[128];
    g_sorting->text(*(const int *)a, g_sorting->sort_col, x, sizeof x);
    g_sorting->text(*(const int *)b, g_sorting->sort_col, y, sizeof y);
    int ok1, ok2;
    double dx = num_of(x, &ok1), dy = num_of(y, &ok2);
    int r;
    if (ok1 && ok2) r = dx < dy ? -1 : (dx > dy ? 1 : 0);
    else if (ok1 != ok2) r = ok1 ? -1 : 1;
    else r = _stricmp(x, y);
    if (!r) r = *(const int *)a - *(const int *)b;
    return g_sorting->sort_desc ? -r : r;
}

static void vlist_sort(VList *v)
{
    if (v->sort_col < 0 || v->nrows < 2) return;
    g_sorting = v;
    qsort(v->rows, v->nrows, sizeof(int), vlist_cmp);
}

static void vlist_set_rows(VList *v, const int *items, int n)
{
    int *r = realloc(v->rows, (n ? n : 1) * sizeof(int));
    if (!r) return;
    v->rows = r;
    if (items) memcpy(v->rows, items, n * sizeof(int));
    v->nrows = n;
    vlist_sort(v);
    ListView_SetItemCountEx(v->hwnd, n, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(v->hwnd, NULL, FALSE);
}

static int vlist_selected(VList *v)
{
    int i = ListView_GetNextItem(v->hwnd, -1, LVNI_SELECTED);
    return (i >= 0 && i < v->nrows) ? v->rows[i] : -1;
}

static LRESULT CALLBACK list_subclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (m == WM_NOTIFY) {
        NMHDR *nh = (NMHDR *)l;
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                RECT rc;
                GetClientRect(nh->hwndFrom, &rc);
                FillRect(cd->hdc, &rc, br_panel2);
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            }
            if (cd->dwDrawStage == CDDS_POSTPAINT) {
                RECT rc, last;
                GetClientRect(nh->hwndFrom, &rc);
                int n = Header_GetItemCount(nh->hwndFrom);
                if (n > 0 && Header_GetItemRect(nh->hwndFrom, n - 1, &last) && last.right < rc.right) {
                    rc.left = last.right;
                    FillRect(cd->hdc, &rc, br_panel2);
                }
                return CDRF_DODEFAULT;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                VList *v = (VList *)GetWindowLongPtr(h, GWLP_USERDATA);
                RECT rc = cd->rc;
                FillRect(cd->hdc, &rc, br_panel2);
                HPEN pen = CreatePen(PS_SOLID, 1, C_LINE);
                HGDIOBJ op = SelectObject(cd->hdc, pen);
                MoveToEx(cd->hdc, rc.right - 1, rc.top + S(4), NULL);
                LineTo(cd->hdc, rc.right - 1, rc.bottom - S(4));
                SelectObject(cd->hdc, op);
                DeleteObject(pen);
                int col = (int)cd->dwItemSpec;
                char title[64];
                snprintf(title, sizeof title, "%s%s", v ? v->cols[col] : "",
                         (v && v->sort_col == col) ? (v->sort_desc ? " v" : " ^") : "");
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, C_GOLD);
                HGDIOBJ of = SelectObject(cd->hdc, f_smallb);
                rc.left += S(6);
                rc.right -= S(6);
                DrawText(cd->hdc, title, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                         ((v && v->align[col] == 'c') ? DT_CENTER : DT_LEFT));
                SelectObject(cd->hdc, of);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}

static void vlist_create(VList *v, int which, HWND parent, int ncols, const char *const *cols, const int *widths,
                         const char *align, void (*text)(int, int, char *, int), COLORREF (*color)(int, COLORREF *))
{
    v->ncols = ncols;
    v->cols = cols;
    v->widths = widths;
    v->align = align;
    v->text = text;
    v->color = color;
    v->sort_col = -1;
    v->hwnd = CreateWindowEx(0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                             LVS_SHOWSELALWAYS | LVS_SINGLESEL, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)(ID_LIST + which), g_inst, NULL);
    ListView_SetExtendedListViewStyle(v->hwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(v->hwnd, L"DarkMode_Explorer", NULL);
    ListView_SetBkColor(v->hwnd, C_PANEL);
    ListView_SetTextBkColor(v->hwnd, C_PANEL);
    ListView_SetTextColor(v->hwnd, C_TEXT);
    SendMessage(v->hwnd, WM_SETFONT, (WPARAM)f_ui, TRUE);
    for (int i = 0; i < ncols; i++) {
        LVCOLUMN c = { 0 };
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (char *)cols[i];
        c.cx = S(widths[i]);
        c.fmt = align[i] == 'c' ? LVCFMT_CENTER : LVCFMT_LEFT;
        ListView_InsertColumn(v->hwnd, i, &c);
    }
    SetWindowLongPtr(v->hwnd, GWLP_USERDATA, (LONG_PTR)v);
    SetWindowSubclass(v->hwnd, list_subclass, 1, 0);
}

/* ---- list contents -------------------------------------------------------------- */
static RankLine g_rank_lines[N_RULES];
static int g_rank_score;

static int duel_active(void) { return g_mode && g_tr.active; }

static void t_rank(int i, int col, char *b, int n)
{
    int act = duel_active();
    switch (col) {
    case 0: snprintf(b, n, "%s", RANK_RULES[i].name); break;
    case 1: act ? snprintf(b, n, "%d", g_rank_lines[i].value) : snprintf(b, n, "-"); break;
    case 2: act ? snprintf(b, n, "%+d", g_rank_lines[i].change) : snprintf(b, n, "-"); break;
    default: snprintf(b, n, "%s", act ? g_rank_lines[i].note : ""); break;
    }
}
static COLORREF c_rank(int i, COLORREF *bg)
{
    (void)bg;
    if (!duel_active()) return C_MUTED;
    return g_rank_lines[i].change > 0 ? C_GREEN : (g_rank_lines[i].change < 0 ? C_RED : C_TEXT);
}

static void owned_str(int id, char *b, int n)
{
    if (snap_ok()) snprintf(b, n, "%d", owned_total(&g_snap, id));
    else b[0] = 0;
}

static void pool_text(const uint16_t *pool, int id, int col, char *b, int n)
{
    switch (col) {
    case 0: snprintf(b, n, "%s", card_name(id)); break;
    case 1: snprintf(b, n, "%.1f%%", pool ? pool[id - 1] * 100.0 / 2048 : 0.0); break;
    default: owned_str(id, b, n); break;
    }
}
static COLORREF pool_color(int id)
{
    return (snap_ok() && owned_total(&g_snap, id) == 0) ? C_GOLD : C_TEXT;
}
static void t_detail(int id, int col, char *b, int n) { pool_text(g_detail_pool, id, col, b, n); }
static COLORREF c_pool(int id, COLORREF *bg) { (void)bg; return pool_color(id); }

/* ---- Card Overview items: the 725 entries of Card Overview v2.1 ------------- */
static const char *item_name(int i)
{
    if (i == ITEM_DRAGON) return "Dragon";
    if (i == ITEM_EQUIP) return "Equip";
    if (i == ITEM_THUNDER) return "Thunder";
    return card_name(i);
}

static const char *item_kind(int i)
{
    if (i == ITEM_EQUIP) return "Every Equip card";
    if (i > N_CARDS) return "Type, incl. subtypes";
    return CARDS[i - 1].type;
}

static int item_matches(int item, int id)
{
    if (item <= N_CARDS) return item == id;
    const Card *c = &CARDS[id - 1];
    if (item == ITEM_DRAGON) return !strcmp(c->type, "Dragon") || (c->sub & 1);   /* e.g. Dragon Zombie */
    if (item == ITEM_THUNDER) return !strcmp(c->type, "Thunder") || (c->sub & 2);
    return !strcmp(c->type, "Equip");
}

/* copies in the deck (where 0) or chest (where 1) */
static int item_count(int item, int where)
{
    if (!snap_ok()) return 0;
    int n = 0;
    if (item <= N_CARDS) return where ? g_snap.trunk[item] : g_snap.deck_count[item];
    if (where == 0) {
        for (int k = 0; k < 40; k++) n += item_matches(item, g_snap.deck[k]);
    } else {
        for (int id = 1; id <= N_CARDS; id++) if (item_matches(item, id)) n += g_snap.trunk[id];
    }
    return n;
}

static int is_tracked(int item)
{
    for (int i = 0; i < g_ntracked; i++) if (g_tracked[i] == item) return 1;
    return 0;
}

static void t_all(int item, int col, char *b, int n)
{
    int ok = snap_ok();
    switch (col) {
    case 0: snprintf(b, n, "%s", item_name(item)); break;
    case 1: snprintf(b, n, "%s", item_kind(item)); break;
    case 2: ok ? snprintf(b, n, "%d", item_count(item, 0)) : (b[0] = 0); break;
    case 3: ok ? snprintf(b, n, "%d", item_count(item, 1)) : (b[0] = 0); break;
    default: ok ? snprintf(b, n, "%d", item_count(item, 0) + item_count(item, 1)) : (b[0] = 0); break;
    }
}
static COLORREF c_all(int item, COLORREF *bg)
{
    (void)bg;
    if (is_tracked(item)) return C_GOLD;
    if (snap_ok() && item_count(item, 0) + item_count(item, 1) == 0) return C_MUTED;
    return C_TEXT;
}

static void t_track(int item, int col, char *b, int n)
{
    int ok = snap_ok();
    switch (col) {
    case 0: snprintf(b, n, "%s", item_name(item)); break;
    case 1: ok ? snprintf(b, n, "%d", item_count(item, 0)) : (b[0] = 0); break;
    case 2: ok ? snprintf(b, n, "%d", item_count(item, 1)) : (b[0] = 0); break;
    default: ok ? snprintf(b, n, "%d", item_count(item, 0) + item_count(item, 1)) : (b[0] = 0); break;
    }
}
static COLORREF c_track(int item, COLORREF *bg)
{
    (void)item; (void)bg;
    return C_TEXT;
}

static void t_pw(int id, int col, char *b, int n)
{
    const Card *c = &CARDS[id - 1];
    switch (col) {
    case 0: snprintf(b, n, "%s", g_fav[id] ? "*" : ""); break;
    case 1: snprintf(b, n, "%d", id); break;
    case 2: snprintf(b, n, "%s", c->name); break;
    case 3: snprintf(b, n, "%s", c->password[0] ? c->password : "-"); break;
    case 4: if (c->cost >= 0) snprintf(b, n, "%d", c->cost); else snprintf(b, n, "-"); break;
    case 5: snprintf(b, n, "%s", c->type); break;
    case 6: snprintf(b, n, "%s", c->attr); break;
    case 7: if (c->level) snprintf(b, n, "%d", c->level); else b[0] = 0; break;
    case 8: snprintf(b, n, "%d", c->atk); break;
    case 9: snprintf(b, n, "%d", c->def); break;
    default: owned_str(id, b, n); break;
    }
}
static COLORREF c_pw(int id, COLORREF *bg)
{
    (void)bg;
    const Card *c = &CARDS[id - 1];
    if (g_fav[id]) return C_GOLD;
    if (c->cost < 0) return C_MUTED;
    if (snap_ok() && (uint32_t)c->cost <= g_snap.starchips) return C_GREEN;
    return C_TEXT;
}

/* summary lists */
static void t_sumfav(int id, int col, char *b, int n)
{
    const Card *c = &CARDS[id - 1];
    switch (col) {
    case 0: snprintf(b, n, "%s", c->name); break;
    case 1: snprintf(b, n, "%s", c->password[0] ? c->password : "-"); break;
    case 2: if (c->cost >= 0) snprintf(b, n, "%d", c->cost); else snprintf(b, n, "-"); break;
    default: owned_str(id, b, n); break;
    }
}
static COLORREF c_sumfav(int id, COLORREF *bg)
{
    (void)bg;
    const Card *c = &CARDS[id - 1];
    if (c->cost < 0) return C_MUTED;
    if (snap_ok() && (uint32_t)c->cost <= g_snap.starchips) return C_GREEN;
    return C_TEXT;
}

/* drops tab duelist picker */
static void t_dropduel(int d, int col, char *b, int n)
{
    switch (col) {
    case 0: snprintf(b, n, "%d", d); break;
    case 1: snprintf(b, n, "%s", DUELISTS[d - 1]); break;
    default: snprintf(b, n, "%s", g_have_pool[d] ? "learned" : "-"); break;
    }
}
static COLORREF c_dropduel(int d, COLORREF *bg)
{
    if (duel_active() && g_tr.opponent == d) {
        *bg = C_CURBG;
        return RGB(0xff, 0xe6, 0xa8);
    }
    return g_have_pool[d] ? C_TEXT : C_MUTED;
}

/* ---- list (re)builders -------------------------------------------------------- */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++)
        if (!_strnicmp(hay, needle, nl)) return 1;
    return 0;
}

static void rebuild_all_cards(void)
{
    char q[128];
    GetWindowText(g_card_search, q, sizeof q);
    static int items[ART_ITEMS];
    int n = 0;
    static const int specials[] = { ITEM_DRAGON, ITEM_EQUIP, ITEM_THUNDER };
    for (int k = 0; k < 3; k++)   /* type entries first, like Card Overview v2.1 */
        if (!q[0] || contains_ci(item_name(specials[k]), q) || contains_ci("type", q)) items[n++] = specials[k];
    for (int id = 1; id <= N_CARDS; id++) {
        const Card *c = &CARDS[id - 1];
        if (q[0] && !(contains_ci(c->name, q) || contains_ci(c->type, q) || atoi(q) == id)) continue;
        items[n++] = id;
    }
    /* cards alphabetical, as in Card Overview v2.1 */
    for (int i = 4; i < n; i++)
        for (int j = i; j > 0 && items[j - 1] <= N_CARDS && _stricmp(item_name(items[j]), item_name(items[j - 1])) < 0; j--) {
            int t = items[j]; items[j] = items[j - 1]; items[j - 1] = t;
        }
    vlist_set_rows(&g_lists[L_ALL], items, n);
}

static void rebuild_tracked(void)
{
    vlist_set_rows(&g_lists[L_TRACK], g_tracked, g_ntracked);
    snprintf(t_tracked_sum, sizeof t_tracked_sum, "%d of %d slots used", g_ntracked, MAX_TRACK);
    if (g_strip) InvalidateRect(g_strip, NULL, FALSE);
}

static void rebuild_pw(void)
{
    char q[128];
    GetWindowText(g_pw_search, q, sizeof q);
    static int items[N_CARDS];
    int n = 0, qid = atoi(q);
    for (int id = 1; id <= N_CARDS; id++) {
        const Card *c = &CARDS[id - 1];
        if (q[0] && !(contains_ci(c->name, q) || contains_ci(c->type, q) || qid == id ||
                      (c->password[0] && strstr(c->password, q))))
            continue;
        if (g_pw_unowned && snap_ok() && owned_total(&g_snap, id) > 0) continue;
        if (g_pw_afford && !(snap_ok() && c->cost >= 0 && (uint32_t)c->cost <= g_snap.starchips)) continue;
        if (g_pw_favonly && !g_fav[id]) continue;
        items[n++] = id;
    }
    vlist_set_rows(&g_lists[L_PW], items, n);
    snprintf(t_pw_count, sizeof t_pw_count, "%d cards  |  gold = favorite, green = affordable", n);
    InvalidateRect(g_pages[PG_PW], NULL, TRUE);
}

static void rebuild_favs(void)
{
    g_nfavs = 0;
    for (int id = 1; id <= N_CARDS; id++)
        if (g_fav[id]) g_favs[g_nfavs++] = id;
    vlist_set_rows(&g_lists[L_SUM_FAV], g_favs, g_nfavs);
    snprintf(t_fav_sum, sizeof t_fav_sum, "%d favorite%s", g_nfavs, g_nfavs == 1 ? "" : "s");
    InvalidateRect(g_pages[PG_SUMMARY], NULL, TRUE);
}

static void toggle_fav(int id)
{
    if (id < 1 || id > N_CARDS) return;
    g_fav[id] = !g_fav[id];
    save_favs();
    rebuild_favs();
    if (g_pw_favonly) rebuild_pw();
    else InvalidateRect(g_lists[L_PW].hwnd, NULL, FALSE);
    logf_("%s %s favorites.", CARDS[id - 1].name, g_fav[id] ? "added to" : "removed from");
}

static void fill_pool_list(VList *v, const uint16_t *pool)
{
    static int items[2][N_CARDS];
    int *it = items[v == &g_lists[L_DETAIL]];
    int n = 0;
    if (pool) {
        for (int id = 1; id <= N_CARDS; id++)
            if (pool[id - 1]) it[n++] = id;
        /* default order: most likely first */
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && pool[it[j] - 1] > pool[it[j - 1] - 1]; j--) { int t = it[j]; it[j] = it[j - 1]; it[j - 1] = t; }
    }
    int sc = v->sort_col;
    if (sc < 0) v->sort_col = -1;
    vlist_set_rows(v, it, n);
}

static void fill_detail(void)
{
    int d = vlist_selected(&g_lists[L_DROPDUEL]);
    if (d < 1) return;
    g_detail_duelist = d;
    snprintf(t_detail_title, sizeof t_detail_title, "%s", DUELISTS[d - 1]);
    int tier = (int)SendMessage(g_tier_combo, CB_GETCURSEL, 0, 0);
    if (tier < 0) tier = 0;
    if (!g_have_pool[d]) {
        g_detail_pool = NULL;
        snprintf(t_detail_note, sizeof t_detail_note, "Duel this opponent once while attached and their deck "
                 "and drop tables are learned automatically.");
    } else {
        g_detail_pool = tier == 3 ? g_pools[d][0] : g_pools[d][tier + 1];
        snprintf(t_detail_note, sizeof t_detail_note, "%s", tier == 3 ? "Chance of each card in the CPU's deck draw." :
                 "Gold = a card you don't own yet.");
        if (duel_active() && g_tr.opponent == d) {
            char lab[8];
            rank_label(g_rank_score, NULL, NULL, lab);
            size_t l = strlen(t_detail_note);
            snprintf(t_detail_note + l, sizeof t_detail_note - l, "  Your rank right now: %s (%s table).", lab,
                     DROP_TIER_NAMES[drop_tier(g_rank_score)]);
        }
    }
    fill_pool_list(&g_lists[L_DETAIL], g_detail_pool);
    InvalidateRect(g_pages[PG_DROPS], NULL, TRUE);
}

/* ---- duelist stats / win chance ------------------------------------------------- */
static void record_for(int d, DStat *s)
{
    memset(s, 0, sizeof *s);
    s->best = -1;
    if (snap_ok()) { s->fw = g_snap.rec_w[d]; s->fl = g_snap.rec_l[d]; }
    int xw = 0, xl = 0;
    for (int i = 0; i < g_nhist; i++) {
        Hist *h = &g_hist[i];
        if (h->opp != d) continue;
        if (h->win) {
            s->sw++;
            if (!h->free_duel) xw++;
            if (s->best < 0 || abs(h->score - 50) > abs(s->best - 50)) s->best = h->score;
        } else {
            s->sl++;
            if (!h->free_duel) xl++;
        }
    }
    s->w = s->fw + xw;
    s->l = s->fl + xl;
}

static double chance_for(int d, int live, char *note, int nlen)
{
    DStat s;
    record_for(d, &s);
    double mine = snap_ok() ? deck_power(&g_snap) : 0, cpu = 0;
    if (live && g_snap.pools_valid) cpu = pool_power(g_snap.deck_pool);
    else if (g_have_pool[d]) cpu = pool_power(g_pools[d][0]);
    double p = win_chance(s.w, s.l, DIFFICULTY[d - 1], mine, cpu, live, g_snap.you.lp, g_snap.cpu.lp,
                          g_snap.you.start_lp);
    if (note) {
        char a[48], b[80] = "";
        if (s.w + s.l) snprintf(a, sizeof a, "record %d-%d", s.w, s.l);
        else snprintf(a, sizeof a, "no record yet");
        if (cpu > 0) snprintf(b, sizeof b, "  |  your top ATK %.0f vs CPU avg %.0f", mine, cpu);
        snprintf(note, nlen, "%s%s%s", a, b, live ? "  |  following life points" : "");
    }
    return p;
}

/* Chance of winning all of the Final 7 in a row (Seto 2nd, Guardian Sebek, Guardian Neku,
 * Heishin 2nd, Seto 3rd, DarkNite, Nitemare): the product of each duel's estimate. The one
 * you are fighting right now follows its live life points. */
static double final7_chance(void)
{
    double p = 1.0;
    for (int d = 32; d <= 38; d++)
        p *= chance_for(d, duel_active() && g_tr.opponent == d, NULL, 0);
    return p;
}

static void update_dstats(int force)
{
    uint64_t t = now_ms();
    if (!force && t - g_dstat_t < 1000) return;
    g_dstat_t = t;
    for (int d = 1; d <= N_DUELISTS; d++) {
        record_for(d, &g_dstat[d]);
        g_dstat[d].chance = chance_for(d, duel_active() && g_tr.opponent == d, NULL, 0);
    }
}

/* ---- duel events ---------------------------------------------------------------- */
static void select_drop_duelist(int d, int tier)
{
    VList *v = &g_lists[L_DROPDUEL];
    if (tier >= 0) SendMessage(g_tier_combo, CB_SETCURSEL, tier, 0);
    for (int i = 0; i < v->nrows; i++)
        if (v->rows[i] == d) {
            ListView_SetItemState(v->hwnd, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(v->hwnd, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(v->hwnd, i, FALSE);
            break;
        }
    fill_detail();
}

static void on_event(const DuelEvent *ev)
{
    if (ev->type == EV_START) {
        logf_("Duel started vs %s.", duelist_name(ev->opponent));
        InvalidateRect(g_lists[L_DROPDUEL].hwnd, NULL, FALSE);
    } else if (ev->type == EV_ABORT) {
        logf_("Duel vs %s left without a result (reset or state load).", duelist_name(ev->opponent));
    } else if (ev->type == EV_END) {
        RankLine rl[N_RULES];
        int score = rank_breakdown(&ev->you, g_snap.table, 0, rl);
        Hist *h = hist_add();
        if (!h) return;
        time_t tt = time(NULL);
        strftime(h->when, sizeof h->when, "%Y-%m-%d %H:%M", localtime(&tt));
        h->opp = ev->opponent;
        h->win = ev->win;
        h->kind = ev->kind;
        h->score = ev->win ? score : 0;
        h->turns = ev->you.counters[0];
        h->lp = ev->you.lp;
        g_pend = g_nhist - 1;
        g_pend_t = now_ms();
        g_pend_w = g_tr.start_w;
        g_pend_l = g_tr.start_l;
        memcpy(g_pend_drops, g_tr.drops_at_start, sizeof g_pend_drops);
        g_pend_ndrops = g_tr.n_drops_at_start;
        char lab[8];
        rank_label(score, NULL, NULL, lab);
        if (ev->win) logf_("Duel vs %s: WIN - %s (%d)", duelist_name(ev->opponent), lab, score);
        else logf_("Duel vs %s: LOSS", duelist_name(ev->opponent));
        save_history();
        update_dstats(1);
    }
}

static void follow_pending(void)
{
    if (g_pend < 0 || g_pend >= g_nhist) return;
    Hist *h = &g_hist[g_pend];
    int changed = 0;
    if (h->win && !h->drop && g_snap.n_drops &&
        (g_snap.n_drops != g_pend_ndrops || memcmp(g_snap.last_drops, g_pend_drops, sizeof g_pend_drops))) {
        h->drop = g_snap.last_drops[0];
        logf_("Card drop: %s", card_name(h->drop));
        changed = 1;
    }
    if (!h->free_duel && (g_snap.rec_w[h->opp] != g_pend_w || g_snap.rec_l[h->opp] != g_pend_l)) {
        h->free_duel = 1;
        changed = 1;
    }
    if (changed) save_history();
    if (now_ms() - g_pend_t > 90000 || (h->drop && h->free_duel)) g_pend = -1;
}

static void cache_pools(void)
{
    int d = g_snap.opponent;
    if (d < 1 || d > N_DUELISTS || g_have_pool[d] || !g_snap.pools_valid) return;
    InvalidateRect(g_lists[L_DROPDUEL].hwnd, NULL, FALSE);
    memcpy(g_pools[d][0], g_snap.deck_pool, sizeof g_pools[d][0]);
    for (int t = 0; t < 3; t++) memcpy(g_pools[d][t + 1], g_snap.drop_pool[t], sizeof g_pools[d][0]);
    g_have_pool[d] = 1;
    save_pools();
    logf_("Learned %s's deck and drop tables.", duelist_name(d));
    if (g_detail_duelist == d) fill_detail();
}

/* ---- per-tick processing -------------------------------------------------------- */
static uint32_t g_pw_sig;

static void set_status(const char *s, COLORREF c)
{
    if (strcmp(s, g_status) || c != g_status_color) {
        snprintf(g_status, sizeof g_status, "%s", s);
        g_status_color = c;
        RECT r = { 0, 0, S(2000), S(46) };
        InvalidateRect(g_main, &r, FALSE);
    }
}

static void process_snapshot(void)
{
    DuelEvent ev;
    if (tracker_update(&g_tr, &g_snap, now_ms(), &ev)) on_event(&ev);
    follow_pending();
    if (duel_active()) cache_pools();

    /* card totals (passwords tab refresh) and Card Overview deck counts */
    for (int id = 1; id <= N_CARDS; id++) g_prev[id] = owned_total(&g_snap, id);
    g_have_prev = 1;
    for (int item = 1; item <= ART_ITEMS; item++) {
        int d = item_count(item, 0);
        if (g_have_iprev && d != g_iprev[item]) {
            g_ichg_delta[item] = d - g_iprev[item];
            g_ichg_time[item] = now_ms();
            if (is_tracked(item)) logf_("%s in deck: %d -> %d", item_name(item), g_iprev[item], d);
        }
        g_iprev[item] = d;
    }
    g_have_iprev = 1;
    rebuild_tracked();

    /* last drops text */
    t_last_drops[0] = 0;
    for (int i = 0; i < g_snap.n_drops; i++) {
        size_t l = strlen(t_last_drops);
        snprintf(t_last_drops + l, sizeof t_last_drops - l, "%s%s", i ? ", " : "", card_name(g_snap.last_drops[i]));
    }
    if (!t_last_drops[0]) snprintf(t_last_drops, sizeof t_last_drops, "-");

    /* rank */
    const Side *side = &g_snap.you;
    g_rank_score = rank_breakdown(side, g_snap.table, g_assume_win, g_rank_lines);

    /* possible drops */
    int tier = drop_tier(g_rank_score);
    int opp = duel_active() ? g_tr.opponent : -1;
    const uint16_t *pool = (duel_active() && g_snap.pools_valid) ? g_snap.drop_pool[tier] : NULL;
    static uint16_t shown[N_CARDS];
    if (tier != g_drop_tier || opp != g_drop_opp || (pool != NULL) != (g_drop_pool != NULL)) {
        g_drop_tier = tier;
        g_drop_opp = opp;
        if (pool) { memcpy(shown, pool, sizeof shown); g_drop_pool = shown; } else g_drop_pool = NULL;
        if (opp > 0) select_drop_duelist(opp, tier);   /* Drops tab follows the duel */
    }

    update_dstats(0);

    uint32_t sig = g_snap.starchips * 2654435761u;
    for (int id = 1; id <= N_CARDS; id++) sig = sig * 31 + (uint32_t)g_prev[id];
    if (sig != g_pw_sig) {
        g_pw_sig = sig;
        if (g_pw_unowned || g_pw_afford) rebuild_pw();
    }

    /* repaint visible things */
    for (int i = 0; i < N_LISTS; i++)
        if (IsWindowVisible(g_lists[i].hwnd)) InvalidateRect(g_lists[i].hwnd, NULL, FALSE);
    InvalidateRect(g_sumcard, NULL, FALSE);
    InvalidateRect(g_strip, NULL, FALSE);
    if (g_overlay) InvalidateRect(g_overlay, NULL, FALSE);
    if (IsWindowVisible(g_pages[PG_SUMMARY])) InvalidateRect(g_pages[PG_SUMMARY], NULL, FALSE);
    if (IsWindowVisible(g_pages[PG_CARDS])) InvalidateRect(g_pages[PG_CARDS], NULL, FALSE);
    RECT r = { 0, S(46), S(2000), S(96) };
    InvalidateRect(g_main, &r, FALSE);
}

static void detach_all(void)
{
    if (g_mode == 1) emu_detach(&g_att);
    g_mode = 0;
    g_have_snap = 0;
    tracker_reset(&g_tr);
    g_have_prev = 0;
    set_status("Select attach from dropdown to connect", C_RED);
    InvalidateRect(g_main, NULL, FALSE);
    InvalidateRect(g_sumcard, NULL, FALSE);
}

static void on_tick(void)
{
    char buf[300];
    if (g_attaching) {
        snprintf(buf, sizeof buf, "Attaching... %s", g_progress ? "" : "");
        if (g_progress) snprintf(buf, sizeof buf, "Scanning emulator memory... %d%%", g_progress);
        set_status(buf, C_BLUE);
        return;
    }
    if (g_mode == 0) return;
    int ok;
    if (g_mode == 1) {
        if (!emu_alive(&g_att)) {
            logf_("Emulator closed - detached.");
            detach_all();
            return;
        }
        ok = emu_read_ram(&g_att, g_ram);
    } else {
        demo_step(g_ram, now_ms());
        ok = 1;
    }
    if (!ok) {
        snprintf(buf, sizeof buf, "%s - waiting for the game to start", g_mode == 1 ? g_att.exe : "Demo");
        set_status(buf, C_GOLD);
        return;
    }
    snapshot_parse(g_ram, &g_snap);
    g_have_snap = 1;
    if (!g_snap.save_loaded) {
        snprintf(buf, sizeof buf, "%s - waiting for a save to load", g_mode == 1 ? g_att.exe : "Demo");
        set_status(buf, C_GOLD);
        return;
    }
    if (g_mode == 1) snprintf(buf, sizeof buf, "%s (pid %lu) via %s", g_att.exe, g_att.pid, g_att.method);
    else snprintf(buf, sizeof buf, "Demo mode (simulated game)");
    set_status(buf, C_GREEN);
    process_snapshot();
}

/* ---- attaching -------------------------------------------------------------------- */
static void refresh_emus(void)
{
    g_nemus = emu_find(g_emus, 32);
    SendMessage(g_emu_combo, CB_RESETCONTENT, 0, 0);
    if (!g_nemus) SendMessage(g_emu_combo, CB_ADDSTRING, 0, (LPARAM)"No emulator found - start one, then refresh");
    for (int i = 0; i < g_nemus; i++) {
        char s[300];
        snprintf(s, sizeof s, "%s  (pid %lu)", g_emus[i].exe, g_emus[i].pid);
        SendMessage(g_emu_combo, CB_ADDSTRING, 0, (LPARAM)s);
    }
    SendMessage(g_emu_combo, CB_SETCURSEL, 0, 0);
}

static EmuProc g_attach_target;
static int g_attach_quiet;

static DWORD WINAPI attach_thread(LPVOID p)
{
    (void)p;
    int ok = emu_attach(&g_pending_att, &g_attach_target, g_attach_err, sizeof g_attach_err, &g_progress);
    PostMessage(g_main, WM_APP_ATTACHED, ok, 0);
    return 0;
}

static void start_attach(int quiet)
{
    if (g_attaching) return;
    if (!g_nemus) refresh_emus();
    if (!g_nemus) {
        if (!quiet)
            MessageBox(g_main, "No supported emulator is running.\n\nSupported: DuckStation, ePSXe, RetroArch "
                       "(SwanStation / Beetle PSX / PCSX ReARMed), Mednafen, pSX, PCSX-Redux, Xebra, BizHawk "
                       "(memory scan).", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    int i = (int)SendMessage(g_emu_combo, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= g_nemus) i = 0;
    detach_all();
    g_attach_target = g_emus[i];
    g_attach_quiet = quiet;
    g_attaching = 1;
    g_progress = 0;
    logf_("Attaching to %s (pid %lu)...", g_attach_target.exe, g_attach_target.pid);
    HANDLE th = CreateThread(NULL, 0, attach_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

static void on_attached(int ok)
{
    g_attaching = 0;
    if (!ok) {
        set_status("Select attach from dropdown to connect", C_RED);
        logf_("%s", g_attach_err);
        if (!g_attach_quiet) MessageBox(g_main, g_attach_err, APP_NAME, MB_ICONWARNING);
        return;
    }
    g_att = g_pending_att;
    g_mode = 1;
    tracker_reset(&g_tr);
    g_have_prev = 0;
    logf_("Attached to %s via %s.", g_att.exe, g_att.method);
    on_tick();
}

static void start_demo(void)
{
    detach_all();
    demo_init(g_ram);
    g_mode = 2;
    tracker_reset(&g_tr);
    logf_("Demo mode: a simulated game is playing endless duels.");
    on_tick();
}

/* ---- tracked cards actions ---------------------------------------------------------- */
static void tracked_changed(void)
{
    save_tracked();
    rebuild_tracked();
    InvalidateRect(g_lists[L_ALL].hwnd, NULL, FALSE);
    InvalidateRect(g_pages[PG_CARDS], NULL, TRUE);
}

static void track_item(int item)
{
    if (item < 1 || is_tracked(item)) return;
    if (g_ntracked >= MAX_TRACK) {
        MessageBox(g_main, "All 7 Summary slots are in use. Untrack one first.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    g_tracked[g_ntracked++] = item;
    tracked_changed();
}

static void untrack_selected(void)
{
    int id = vlist_selected(&g_lists[L_TRACK]);
    if (id < 1) return;
    for (int i = 0; i < g_ntracked; i++)
        if (g_tracked[i] == id) {
            memmove(g_tracked + i, g_tracked + i + 1, (g_ntracked - i - 1) * sizeof(int));
            g_ntracked--;
            break;
        }
    tracked_changed();
}

static void move_selected(int dir)
{
    int id = vlist_selected(&g_lists[L_TRACK]);
    for (int i = 0; i < g_ntracked; i++)
        if (g_tracked[i] == id && i + dir >= 0 && i + dir < g_ntracked) {
            int t = g_tracked[i]; g_tracked[i] = g_tracked[i + dir]; g_tracked[i + dir] = t;
            tracked_changed();
            VList *v = &g_lists[L_TRACK];
            for (int r = 0; r < v->nrows; r++)
                if (v->rows[r] == id) ListView_SetItemState(v->hwnd, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            break;
        }
}

static void copy_password(int which)
{
    int id = vlist_selected(&g_lists[which]);
    if (id < 1 || !CARDS[id - 1].password[0]) return;
    const char *pw = CARDS[id - 1].password;
    if (OpenClipboard(g_main)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(pw) + 1);
        if (h) {
            memcpy(GlobalLock(h), pw, strlen(pw) + 1);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
        CloseClipboard();
        logf_("Copied password for %s: %s", CARDS[id - 1].name, pw);
    }
}

/* ---- painting helpers ---------------------------------------------------------------- */
static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text_at(HDC dc, int x, int y, const char *s, HFONT f, COLORREF c)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOut(dc, x, y, s, (int)strlen(s));
    SelectObject(dc, o);
}

static void text_rect(HDC dc, RECT r, const char *s, HFONT f, COLORREF c, UINT fmt)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawText(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, o);
}

static void frame(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

static int text_w(HDC dc, const char *s, HFONT f)
{
    SIZE sz;
    HGDIOBJ o = SelectObject(dc, f);
    GetTextExtentPoint32(dc, s, (int)strlen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

/* "S-POW": letter coloured by grade, POW red / TEC blue */
static void draw_rank_label(HDC dc, RECT r, int score, HFONT f)
{
    char letter, l[2] = { 0, 0 };
    int pow;
    rank_label(score, &letter, &pow, NULL);
    l[0] = letter;
    const char *style = pow ? "POW" : "TEC";
    int x = r.left;
    text_rect(dc, (RECT){ x, r.top, r.right, r.bottom }, l, f, letter_color(letter), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += text_w(dc, l, f);
    text_rect(dc, (RECT){ x, r.top, r.right, r.bottom }, "-", f, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += text_w(dc, "-", f);
    text_rect(dc, (RECT){ x, r.top, r.right, r.bottom }, style, f, pow ? C_POW : C_TEC, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* ---- main window header ---------------------------------------------------------------- */
static void paint_main(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = S(HEADER_H);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bm);
    fill(dc, 0, 0, W, H, C_BG);

    /* title + status */
    text_at(dc, S(16), S(8), APP_NAME, f_title, C_GOLD);
    int tx = S(16) + text_w(dc, APP_NAME, f_title) + S(18);
    HBRUSH db = CreateSolidBrush(g_status_color);
    HGDIOBJ obr = SelectObject(dc, db);
    HGDIOBJ open_ = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, tx, S(17), tx + S(11), S(28));
    SelectObject(dc, open_);
    SelectObject(dc, obr);
    DeleteObject(db);
    RECT sr = { tx + S(18), S(12), W - S(560), S(34) };
    text_rect(dc, sr, g_status, f_ui, C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    /* below the header */
    RECT rest = { 0, H, W, rc.bottom };
    FillRect(wdc, &rest, br_bg);
    EndPaint(h, &ps);
}

/* ---- rank meter: TEC <- S A B C D | D C B A S -> POW ---------------------------------------- */
static RECT g_toggle_rc;

static int draw_rank_meter(HDC dc, int bx, int by, int bw, int show, int score)
{
    for (int i = 0; i < 10; i++) {
        int x0 = bx + i * bw / 10, x1 = bx + (i + 1) * bw / 10;
        char c = i < 5 ? "SABCD"[i] : "DCBAS"[i - 5];
        COLORREF col = i < 5 ? C_TEC : C_POW;
        RECT cell = { x0 + 1, by + S(6), x1 - 1, by + S(22) };
        fill(dc, cell.left, cell.top, cell.right - cell.left, cell.bottom - cell.top, C_PANEL);
        frame(dc, cell, col);
        char cs[2] = { c, 0 };
        text_rect(dc, cell, cs, f_smallb, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (show) {
        int s2 = score < 0 ? 0 : (score > 99 ? 99 : score);
        int mx = bx + s2 * (bw - 2) / 99 + 1;
        POINT tri[3] = { { mx - S(5), by - S(2) }, { mx + S(5), by - S(2) }, { mx, by + S(6) } };
        HBRUSH gb = CreateSolidBrush(C_GOLD);
        HPEN gp = CreatePen(PS_SOLID, S(2), C_GOLD);
        HGDIOBJ o1 = SelectObject(dc, gb), o2 = SelectObject(dc, gp);
        Polygon(dc, tri, 3);
        MoveToEx(dc, mx, by + S(4), NULL);
        LineTo(dc, mx, by + S(26));
        SelectObject(dc, o1);
        SelectObject(dc, o2);
        DeleteObject(gb);
        DeleteObject(gp);
    }
    RECT r = { bx, by + S(28), bx + bw, by + S(44) };
    text_rect(dc, r, "TEC  <  0 -------- 50 -------- 99  >  POW", f_small, C_MUTED, DT_CENTER | DT_SINGLELINE);
    return by + S(46);
}

/* ---- summary card ------------------------------------------------------------------------ */
static void paint_sumcard(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bm);
    fill(dc, 0, 0, W, H, C_PANEL2);
    frame(dc, rc, C_LINE);
    int x = S(16), y = S(12);
    text_at(dc, x, y, "RIGHT NOW", f_smallb, C_MUTED);
    y += S(20);
    char line[96], pct[16];
    if (duel_active()) snprintf(line, sizeof line, "Dueling %s", duelist_name(g_tr.opponent));
    else snprintf(line, sizeof line, "%s", g_mode ? "No duel in progress" : "Not attached");
    text_at(dc, x, y, line, f_bold, C_GOLD);
    y += S(26);
    RECT lr = { x, y, x + S(150), y + S(60) };
    if (duel_active()) draw_rank_label(dc, lr, g_rank_score, f_big);
    else text_rect(dc, lr, "-", f_big, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    snprintf(pct, sizeof pct, "-");
    COLORREF pc = C_MUTED;
    if (g_mode && snap_ok()) {
        double p = final7_chance();
        snprintf(pct, sizeof pct, "%.0f%%", p * 100);
        pc = p >= 0.6 ? C_GREEN : (p >= 0.35 ? C_GOLD : C_RED);
    }
    RECT pr = { W / 2, y, W - x, y + S(60) };
    text_rect(dc, pr, pct, f_big, pc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    y += S(60);
    if (duel_active()) snprintf(line, sizeof line, "rank score %d%s", g_rank_score, g_snap.table_live ? "" : " (est.)");
    else snprintf(line, sizeof line, "%s", g_mode ? "rank shows when a duel starts" : "choose Attach from the Menu");
    text_at(dc, x, y, line, f_small, C_MUTED);
    RECT cr = { W / 2, y, W - x, y + S(18) };
    text_rect(dc, cr, "Beat Final 7", f_small, C_MUTED, DT_RIGHT | DT_SINGLELINE);
    y += S(28);
    y = draw_rank_meter(dc, x, y, W - 2 * x, duel_active(), g_rank_score);
    /* win bonus toggle */
    SetRect(&g_toggle_rc, x, y, W - x, y + S(22));
    RECT box = { x, y + S(4), x + S(14), y + S(18) };
    frame(dc, box, C_MUTED);
    if (g_assume_win) fill(dc, box.left + S(3), box.top + S(3), S(8), S(8), C_GOLD);
    text_at(dc, x + S(22), y + S(2), "Include +2 win bonus", f_small, C_TEXT);
    y += S(32);
    fill(dc, x, y, W - 2 * x, 1, C_LINE);
    y += S(12);
    char v[96];
    struct { const char *k; char val[96]; } rows[1];
    int nr = 0;
    snprintf(rows[nr].val, 96, "%s", snap_ok() ? (snprintf(v, sizeof v, "%u", g_snap.starchips), v) : "-");
    rows[nr++].k = "Starchips";
    for (int i = 0; i < nr && y + S(20) < H; i++) {
        text_at(dc, x, y, rows[i].k, f_small, C_MUTED);
        RECT r = { x + S(110), y, W - x, y + S(20) };
        text_rect(dc, r, rows[i].val, f_ui, C_TEXT, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(22);
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK sum_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: paint_sumcard(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (PtInRect(&g_toggle_rc, p)) {
            g_assume_win = !g_assume_win;
            if (g_have_snap) g_rank_score = rank_breakdown(&g_snap.you, g_snap.table, g_assume_win, g_rank_lines);
            InvalidateRect(h, NULL, FALSE);
            InvalidateRect(g_lists[L_RANK].hwnd, NULL, FALSE);
            save_settings();
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        if (PtInRect(&g_toggle_rc, p)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        break;
    }
    }
    return DefWindowProc(h, m, w, l);
}

/* ---- summary picture strip ---------------------------------------------------------------- */
static void paint_strip(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bm);
    fill(dc, 0, 0, W, H, C_PANEL);
    int gap = 0, th = S(96);   /* pictures touch: no space between them */
    if (g_ntracked && g_ntracked * th + (g_ntracked - 1) * gap > H)   /* shrink to fit the window height */
        th = (H - gap * (g_ntracked - 1)) / g_ntracked;
    int tw = th * 102 / 96;
    for (int i = 0; i < g_ntracked; i++) {
        int item = g_tracked[i], y = i * (th + gap);
        fill(dc, 0, y, tw, th, RGB(0, 0, 0));
        art_draw(dc, item, 0, y, tw, th);
        /* count box, bottom-right of the picture, like Card Overview v2.1 */
        char num[8] = "-";
        if (snap_ok()) snprintf(num, sizeof num, "%d", item_count(item, 0));
        int bw = th * 30 / 96, bh = bw;
        RECT box = { tw - bw - S(2), y + th - bh - S(2), tw - S(2), y + th - S(2) };
        fill(dc, box.left, box.top, bw, bh, RGB(0, 0, 0));
        frame(dc, box, RGB(0xc8, 0xc8, 0xc8));
        text_rect(dc, box, num, f_count, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (!g_ntracked) {
        RECT r = { 0, 0, W, S(200) };
        text_rect(dc, r, "Nothing tracked. Pick up to 7 on the Card Overview tab.", f_ui, C_MUTED, DT_LEFT | DT_WORDBREAK);
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK strip_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) { paint_strip(h); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProc(h, m, w, l);
}

/* ---- guardian star chart ---------------------------------------------------------------- */
typedef struct { const char *name; COLORREF col; int id; } Star;
static const Star RING_A[4] = { { "Sun", RGB(0xff, 0xc4, 0x3d), STAR_SUN }, { "Moon", RGB(0xc9, 0xd1, 0xe0), STAR_MOON },
                                { "Venus", RGB(0xff, 0x8f, 0xc7), STAR_VENUS }, { "Mercury", RGB(0x4f, 0xd6, 0xc4), STAR_MERCURY } };
static const Star RING_B[6] = { { "Mars", RGB(0xff, 0x5a, 0x4a), STAR_MARS }, { "Jupiter", RGB(0xff, 0xa0, 0x4a), STAR_JUPITER },
                                { "Saturn", RGB(0xd8, 0xc0, 0x84), STAR_SATURN }, { "Uranus", RGB(0x6f, 0xe0, 0xf0), STAR_URANUS },
                                { "Pluto", RGB(0xb4, 0x6e, 0xff), STAR_PLUTO }, { "Neptune", RGB(0x5a, 0x8c, 0xff), STAR_NEPTUNE } };

static void arrow(HDC dc, double x0, double y0, double x1, double y1, double shrink, COLORREF col)
{
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len; dy /= len;
    x0 += dx * shrink; y0 += dy * shrink;
    x1 -= dx * shrink; y1 -= dy * shrink;
    HPEN pen = CreatePen(PS_SOLID, S(2), col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    MoveToEx(dc, (int)x0, (int)y0, NULL);
    LineTo(dc, (int)x1, (int)y1);
    double hl = S(9), hw = S(5);
    POINT tri[3] = { { (int)x1, (int)y1 },
                     { (int)(x1 - dx * hl + dy * hw), (int)(y1 - dy * hl - dx * hw) },
                     { (int)(x1 - dx * hl - dy * hw), (int)(y1 - dy * hl + dx * hw) } };
    Polygon(dc, tri, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

static void draw_ring(HDC dc, const Star *st, int n, int cx, int cy, int rad)
{
    double px[6], py[6];
    for (int i = 0; i < n; i++) {
        double a = -1.5707963 + i * 6.2831853 / n;   /* start at the top, clockwise */
        px[i] = cx + rad * cos(a);
        py[i] = cy + rad * sin(a);
    }
    int nr = S(26);
    if (stars_have()) {   /* the game's own icons, pixel-sharp (whole multiples of 16), name underneath */
        int sz = rad / 2 / 16 * 16;
        if (sz > 64) sz = 64;
        if (sz < 32) sz = 32;
        for (int i = 0; i < n; i++) arrow(dc, px[i], py[i], px[(i + 1) % n], py[(i + 1) % n], sz / 2 + S(10), C_MUTED);
        for (int i = 0; i < n; i++) {
            int ix = (int)px[i] - sz / 2, iy = (int)py[i] - sz / 2 - S(6);
            stars_draw(dc, st[i].id, ix, iy, sz, sz, C_PANEL2);
            RECT t = { (int)px[i] - S(40), iy + sz + S(1), (int)px[i] + S(40), iy + sz + S(15) };
            text_rect(dc, t, st[i].name, f_smallb, st[i].col, DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
        return;
    }
    for (int i = 0; i < n; i++) arrow(dc, px[i], py[i], px[(i + 1) % n], py[(i + 1) % n], nr + S(3), C_MUTED);
    for (int i = 0; i < n; i++) {
        HBRUSH b = CreateSolidBrush(C_PANEL2);
        HPEN p = CreatePen(PS_SOLID, S(2), st[i].col);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        Ellipse(dc, (int)px[i] - nr, (int)py[i] - nr, (int)px[i] + nr, (int)py[i] + nr);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
        RECT t = { (int)px[i] - nr, (int)py[i] - nr, (int)px[i] + nr, (int)py[i] + nr };
        text_rect(dc, t, st[i].name, f_smallb, st[i].col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void paint_stars(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bm);
    fill(dc, 0, 0, W, H, C_PANEL2);
    frame(dc, rc, C_LINE);
    int legend = S(26);
    int ch = H - legend;
    int rad = ch / 2 - S(46);
    if (rad > W / 4 - S(40)) rad = W / 4 - S(40);
    if (rad < S(30)) rad = S(30);
    draw_ring(dc, RING_A, 4, W / 4, ch / 2, rad);
    draw_ring(dc, RING_B, 6, W * 3 / 4, ch / 2, rad);
    RECT lr = { S(10), H - legend, W - S(10), H - S(6) };
    text_rect(dc, lr, stars_have() ? "Arrow points at the star it beats: the winner gets +500 ATK/DEF in battle."
                                   : "Arrow points at the star it beats (+500).  Menu > Load star icons from game disc for the game's icons.",
              f_small, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK stars_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) { paint_stars(h); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProc(h, m, w, l);
}

/* ---- overlay ------------------------------------------------------------------------ */
static void paint_overlay(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bm);
    fill(dc, 0, 0, W, H, C_PANEL2);
    if (duel_active()) {
        char lab[8], letter, pct[16], info[128];
        rank_label(g_rank_score, &letter, NULL, lab);
        double p = chance_for(g_tr.opponent, 1, NULL, 0);
        snprintf(pct, sizeof pct, "%.0f%%", p * 100);
        RECT a = { S(10), S(4), W / 2 + S(20), H - S(26) };
        draw_rank_label(dc, a, g_rank_score, f_big);
        RECT b = { W / 2, S(4), W - S(10), H - S(26) };
        text_rect(dc, b, pct, f_big, p >= 0.6 ? C_GREEN : (p >= 0.35 ? C_GOLD : C_RED), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        snprintf(info, sizeof info, "score %d  |  %s  |  LP %d / %d", g_rank_score, duelist_name(g_tr.opponent),
                 g_snap.you.lp, g_snap.cpu.lp);
        RECT c = { S(10), H - S(26), W - S(10), H - S(6) };
        text_rect(dc, c, info, f_small, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT a = { 0, 0, W, H };
        text_rect(dc, a, "waiting for a duel", f_ui, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: paint_overlay(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: g_overlay = NULL; return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void toggle_overlay(void)
{
    if (g_overlay) { DestroyWindow(g_overlay); return; }
    RECT r = { 0, 0, S(320), S(96) };
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    g_overlay = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "FMOverlay", APP_NAME,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, NULL, NULL, g_inst, NULL);
}

/* ---- layout -------------------------------------------------------------------------- */
typedef struct { RECT r; const char *text; char *dyn; HFONT *font; COLORREF color; UINT fmt; } PText;
#define MAX_PTEXT 12
static PText g_ptext[N_PAGES][MAX_PTEXT];
static int g_nptext[N_PAGES];
static RECT g_edit_frames[2];

static void ptext(int page, int x, int y, int w, int h, const char *text, char *dyn, HFONT *font, COLORREF c, UINT fmt)
{
    if (g_nptext[page] >= MAX_PTEXT) return;
    PText *p = &g_ptext[page][g_nptext[page]++];
    SetRect(&p->r, x, y, x + w, y + h);
    p->text = text;
    p->dyn = dyn;
    p->font = font;
    p->color = c;
    p->fmt = fmt;
}

static void place(HWND h, int x, int y, int w, int hh) { MoveWindow(h, x, y, w < 1 ? 1 : w, hh < 1 ? 1 : hh, TRUE); }

static void layout(void)
{
    RECT rc;
    GetClientRect(g_main, &rc);
    int W = rc.right, H = rc.bottom;
    /* top-right controls */
    int x = W - S(14), y = S(10), bh = S(28);
    x -= S(96);
    place(g_btn[ID_MENU - 100], x, y, S(96), bh);
    x -= S(6) + S(250);
    place(g_emu_combo, x, y + S(1), S(250), S(300));
    for (int i = 0; i < N_PAGES; i++) place(g_btn[ID_TAB0 - 100 + i], S(12) + i * S(128), S(HEADER_H), S(124), S(32));

    int py = S(HEADER_H + 34), pw = W - S(20), ph = H - py - S(10);
    for (int i = 0; i < N_PAGES; i++) place(g_pages[i], S(10), py, pw, ph);
    W = pw;
    H = ph;
    memset(g_nptext, 0, sizeof g_nptext);

    /* summary page */
    {
        int lw = S(380), x2 = S(12) + lw + S(14);
        int ch = S(268);
        place(g_sumcard, S(12), S(12), lw, ch);
        ptext(PG_SUMMARY, S(12), ch + S(22), lw, S(22), "Rank breakdown", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        place(g_lists[L_RANK].hwnd, S(12), ch + S(48), lw, H - ch - S(48) - S(12));
        /* tracked cards: one vertical column */
        int sw = S(102);
        ptext(PG_SUMMARY, x2, S(10), sw + S(14), S(22), "Tracked cards", NULL, &f_bold, C_GOLD, DT_SINGLELINE | DT_END_ELLIPSIS);
        place(g_strip, x2, S(36), sw, H - S(36) - S(12));
        /* favorite passwords: small section, bottom right */
        int fx = x2 + sw + S(16), fw = W - fx - S(12), fh = S(250), fy = H - fh - S(12);
        if (fw < S(200)) fw = S(200);
        ptext(PG_SUMMARY, fx, fy, fw, S(22), "Favorite passwords", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        ptext(PG_SUMMARY, fx + fw / 2, fy + S(2), fw - fw / 2, S(22), NULL, t_fav_sum, &f_small, C_MUTED, DT_SINGLELINE | DT_RIGHT);
        place(g_lists[L_SUM_FAV].hwnd, fx, fy + S(26), fw, fh - S(26));
        ptext(PG_SUMMARY, fx, S(10), fw, S(22), "Guardian star chart", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        place(g_stars, fx, S(36), fw, fy - S(36) - S(16));
    }
    /* cards page */
    {
        int mid = S(150);
        int w1 = (W - S(48) - mid) * 52 / 100, mx = S(12) + w1 + S(12), x3 = mx + mid + S(12), w3 = W - x3 - S(12);
        place(g_card_search, S(16), S(14), w1 - S(8), S(22));
        SetRect(&g_edit_frames[0], S(12), S(10), S(12) + w1, S(40));
        place(g_lists[L_ALL].hwnd, S(12), S(48), w1, H - S(48) - S(30));
        ptext(PG_CARDS, S(12), H - S(24), w1, S(18), "Dragon and Thunder include subtypes (Dragon Zombie counts as a Dragon). "
              "Double-click to track.", NULL, &f_small, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        int by = H / 2 - S(150);
        int ids[] = { ID_TRACK, ID_UNTRACK, ID_MOVEUP, ID_MOVEDN, ID_RESETDEF, ID_CLEAR };
        int gaps[] = { 0, 38, 62, 38, 62, 38 };
        for (int i = 0; i < 6; i++) {
            by += S(gaps[i]);
            place(g_btn[ids[i] - 100], mx, by, mid, S(30));
        }
        ptext(PG_CARDS, x3, S(10), w3, S(22), "Tracked on Summary", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        ptext(PG_CARDS, x3 + w3 / 2, S(12), w3 - w3 / 2, S(22), NULL, t_tracked_sum, &f_small, C_MUTED, DT_SINGLELINE | DT_RIGHT);
        ptext(PG_CARDS, x3, S(42), w3, S(20), "Counts update live from your deck and trunk.",
              NULL, &f_small, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        place(g_lists[L_TRACK].hwnd, x3, S(74), w3, H - S(74) - S(30));
    }
    /* drops page */
    {
        int lw = S(320), x2 = S(12) + lw + S(14), w2 = W - x2 - S(12);
        ptext(PG_DROPS, S(12), S(10), lw, S(22), "Duelists", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        place(g_lists[L_DROPDUEL].hwnd, S(12), S(38), lw, H - S(38) - S(12));
        ptext(PG_DROPS, x2, S(10), w2, S(22), NULL, t_detail_title, &f_bold, C_GOLD, DT_SINGLELINE);
        place(g_tier_combo, x2, S(38), S(190), S(200));
        place(g_lists[L_DETAIL].hwnd, x2, S(74), w2, H - S(74) - S(52));
        ptext(PG_DROPS, x2, H - S(46), w2, S(42), NULL, t_detail_note, &f_small, C_MUTED, DT_WORDBREAK);
    }
    /* passwords page */
    {
        ptext(PG_PW, S(12), S(14), S(170), S(22), "All card passwords", NULL, &f_bold, C_GOLD, DT_SINGLELINE);
        place(g_pw_search, S(194), S(14), S(232), S(22));
        SetRect(&g_edit_frames[1], S(190), S(10), S(430), S(40));
        place(g_btn[ID_PW_UNOWNED - 100], S(442), S(10), S(110), S(30));
        place(g_btn[ID_PW_AFFORD - 100], S(556), S(10), S(140), S(30));
        place(g_btn[ID_PW_FAVONLY - 100], S(700), S(10), S(130), S(30));
        place(g_btn[ID_PW_FAV - 100], W - S(12) - S(150), S(10), S(150), S(30));
        ptext(PG_PW, S(12), S(46), W / 2, S(18), "Enter at the in-game Password screen. Double-click copies; "
              "Space or the Fav column stars a card.", NULL, &f_small, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        ptext(PG_PW, W / 2, S(46), W / 2 - S(12), S(18), NULL, t_pw_count, &f_small, C_MUTED, DT_SINGLELINE | DT_RIGHT);
        place(g_lists[L_PW].hwnd, S(12), S(70), W - S(24), H - S(82));
    }
    for (int i = 0; i < N_LISTS; i++) vlist_fit(&g_lists[i]);
    for (int i = 0; i < N_PAGES; i++) InvalidateRect(g_pages[i], NULL, TRUE);
    InvalidateRect(g_main, NULL, FALSE);
}

static void vlist_fit(VList *v)
{
    RECT r;
    GetClientRect(v->hwnd, &r);
    int avail = r.right - GetSystemMetrics(SM_CXVSCROLL) - S(2);
    int stretch = 0, others = 0;
    for (int i = 0; i < v->ncols; i++)
        if (v->widths[i] > v->widths[stretch]) stretch = i;
    for (int i = 0; i < v->ncols; i++)
        if (i != stretch) { others += S(v->widths[i]); ListView_SetColumnWidth(v->hwnd, i, S(v->widths[i])); }
    int w = avail - others;
    if (w < S(90)) w = S(90);
    ListView_SetColumnWidth(v->hwnd, stretch, w);
}

static void show_page(int p)
{
    g_page = p;
    for (int i = 0; i < N_PAGES; i++) ShowWindow(g_pages[i], i == p ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < N_PAGES; i++) InvalidateRect(g_btn[ID_TAB0 - 100 + i], NULL, FALSE);
    if (p == PG_DROPS && vlist_selected(&g_lists[L_DROPDUEL]) < 1)
        select_drop_duelist(duel_active() ? g_tr.opponent : 1, duel_active() ? drop_tier(g_rank_score) : -1);
}

/* ---- pages ------------------------------------------------------------------------------ */
static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int page = (int)GetWindowLongPtr(h, GWLP_USERDATA);
    switch (m) {
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, br_panel);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (int i = 0; i < g_nptext[page]; i++) {
            PText *t = &g_ptext[page][i];
            RECT r = t->r;
            FillRect(dc, &r, br_panel);
            text_rect(dc, r, t->dyn ? t->dyn : t->text, *t->font, t->color, t->fmt);
        }
        if (page == PG_CARDS) { FillRect(dc, &g_edit_frames[0], br_panel2); frame(dc, g_edit_frames[0], C_LINE); }
        if (page == PG_PW) { FillRect(dc, &g_edit_frames[1], br_panel2); frame(dc, g_edit_frames[1], C_LINE); }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, (HWND)l == g_log ? C_PANEL : C_PANEL2);
        return (LRESULT)((HWND)l == g_log ? br_panel : br_panel2);
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
        return SendMessage(g_main, m, w, l);
    }
    return DefWindowProc(h, m, w, l);
}

/* ---- owner-drawn buttons ------------------------------------------------------------------ */
static void draw_button(DRAWITEMSTRUCT *d)
{
    int id = (int)d->CtlID;
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    char text[64];
    GetWindowText(d->hwndItem, text, sizeof text);
    int pressed = (d->itemState & ODS_SELECTED) != 0, disabled = (d->itemState & ODS_DISABLED) != 0;
    if (id >= ID_TAB0 && id < ID_TAB0 + N_PAGES) {
        int sel = (id - ID_TAB0) == g_page;
        FillRect(dc, &r, br_bg);
        RECT t = r;
        t.bottom += 0;
        fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, sel ? C_PANEL : C_BG);
        if (sel) fill(dc, r.left, r.top, r.right - r.left, S(3), C_GOLD);
        text_rect(dc, t, text, f_bold, sel ? C_GOLD : C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (id == ID_PW_UNOWNED || id == ID_PW_AFFORD || id == ID_PW_FAVONLY) {
        int on = id == ID_PW_UNOWNED ? g_pw_unowned : (id == ID_PW_AFFORD ? g_pw_afford : g_pw_favonly);
        fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, C_PANEL);
        int cy = (r.top + r.bottom) / 2;
        RECT box = { r.left + S(4), cy - S(7), r.left + S(18), cy + S(7) };
        frame(dc, box, on ? C_GOLD : C_MUTED);
        if (on) fill(dc, box.left + S(3), box.top + S(3), S(8), S(8), C_GOLD);
        RECT t = r;
        t.left += S(26);
        text_rect(dc, t, text, f_ui, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int primary = id == ID_MENU || id == ID_TRACK;
    int active = id == ID_OVERLAY && g_overlay;
    COLORREF bg = primary ? (pressed ? RGB(0xf0, 0xc7, 0x6a) : C_GOLD) : (pressed || active ? C_LINE : C_PANEL2);
    if (disabled) bg = C_PANEL;
    HWND parent = GetParent(d->hwndItem);
    RECT pr = r;
    FillRect(dc, &pr, parent == g_main ? br_bg : br_panel);
    HBRUSH b = CreateSolidBrush(bg);
    HPEN p = CreatePen(PS_SOLID, 1, primary ? C_GOLD : C_LINE);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(6), S(6));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
    RECT tr = r;
    if (id == ID_MENU) {   /* down-arrow */
        tr.right -= S(16);
        int cx = r.right - S(16), cy = (r.top + r.bottom) / 2;
        POINT tri[3] = { { cx - S(5), cy - S(2) }, { cx + S(5), cy - S(2) }, { cx, cy + S(4) } };
        HBRUSH ab = CreateSolidBrush(RGB(0x1b, 0x14, 0x06));
        HGDIOBJ o1 = SelectObject(dc, ab), o2 = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, o1);
        SelectObject(dc, o2);
        DeleteObject(ab);
    }
    text_rect(dc, tr, text, f_bold, disabled ? C_MUTED : (primary ? RGB(0x1b, 0x14, 0x06) : C_TEXT),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void star_cache_path(char *out) { data_path(out, "star_icons.bin"); }

static void pick_star_disc(void)
{
    char file[MAX_PATH] = "";
    OPENFILENAMEA of = { sizeof of };
    of.hwndOwner = g_main;
    of.lpstrFilter = "PlayStation disc images (*.bin;*.iso;*.img)\0*.bin;*.iso;*.img\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = sizeof file;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    of.lpstrTitle = "Choose your Yu-Gi-Oh! Forbidden Memories disc image";
    if (!GetOpenFileNameA(&of)) return;
    char err[300], cache[MAX_PATH];
    if (stars_load_from_disc(file, err, sizeof err)) {
        star_cache_path(cache);
        stars_save(cache);
        logf_("Loaded guardian star icons from %s", file);
        InvalidateRect(g_stars, NULL, FALSE);
    } else {
        MessageBox(g_main, err, APP_NAME, MB_ICONWARNING);
    }
}

static void show_main_menu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenu(m, MF_STRING | (g_attaching ? MF_GRAYED : 0), ID_ATTACH, "Attach");
    AppendMenu(m, MF_STRING | (g_mode ? 0 : MF_GRAYED), ID_DETACH, "Detach");
    AppendMenu(m, MF_STRING, ID_REFRESH, "Refresh emulator list");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING | (g_mode == 2 ? MF_CHECKED : 0), ID_DEMO, "Demo");
    AppendMenu(m, MF_STRING | (g_overlay ? MF_CHECKED : 0), ID_OVERLAY, "Overlay");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, ID_STARDISC, "Load star icons from game disc...");
    RECT r;
    GetWindowRect(g_btn[ID_MENU - 100], &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom + S(2), 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd) PostMessage(g_main, WM_COMMAND, cmd, 0);
}

static HWND mkbutton(HWND parent, int id, const char *text)
{
    HWND h = CreateWindow("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, parent,
                          (HMENU)(INT_PTR)id, g_inst, NULL);
    g_btn[id - 100] = h;
    return h;
}

/* ---- list notifications --------------------------------------------------------------------- */
static LRESULT on_list_notify(NMHDR *nh)
{
    int which = (int)nh->idFrom - ID_LIST;
    if (which < 0 || which >= N_LISTS) return 0;
    VList *v = &g_lists[which];
    switch (nh->code) {
    case LVN_GETDISPINFO: {
        NMLVDISPINFO *di = (NMLVDISPINFO *)nh;
        if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && di->item.iItem < v->nrows)
            v->text(v->rows[di->item.iItem], di->item.iSubItem, di->item.pszText, di->item.cchTextMax);
        if ((di->item.mask & LVIF_IMAGE) && (which == L_ALL || which == L_TRACK) && di->item.iItem >= 0 &&
            di->item.iItem < v->nrows)
            di->item.iImage = v->rows[di->item.iItem] - 1;
        return 0;
    }
    case NM_CUSTOMDRAW: {
        NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)nh;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int i = (int)cd->nmcd.dwItemSpec;
            COLORREF bg = C_PANEL;
            cd->clrText = (i >= 0 && i < v->nrows && v->color) ? v->color(v->rows[i], &bg) : C_TEXT;
            if (ListView_GetItemState(v->hwnd, i, LVIS_SELECTED) & LVIS_SELECTED) {
                bg = RGB(0x33, 0x40, 0x6a);
                cd->clrText = RGB(255, 255, 255);
                cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
            }
            cd->clrTextBk = bg;
            return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
    }
    case LVN_COLUMNCLICK: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nh;
        if (v->sort_col == lv->iSubItem) v->sort_desc = !v->sort_desc;
        else { v->sort_col = lv->iSubItem; v->sort_desc = 0; }
        vlist_sort(v);
        InvalidateRect(v->hwnd, NULL, FALSE);
        InvalidateRect(ListView_GetHeader(v->hwnd), NULL, FALSE);
        return 0;
    }
    case NM_DBLCLK:
        if (which == L_ALL) track_item(vlist_selected(v));
        else if (which == L_TRACK) untrack_selected();
        else if (which == L_PW || which == L_SUM_FAV) copy_password(which);
        return 0;
    case NM_CLICK: {
        NMITEMACTIVATE *ia = (NMITEMACTIVATE *)nh;
        if (which == L_PW && ia->iSubItem == 0 && ia->iItem >= 0 && ia->iItem < v->nrows) toggle_fav(v->rows[ia->iItem]);
        return 0;
    }
    case LVN_ITEMCHANGED: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nh;
        if (which == L_DROPDUEL && (lv->uNewState & LVIS_SELECTED)) fill_detail();
        return 0;
    }
    case LVN_KEYDOWN: {
        NMLVKEYDOWN *k = (NMLVKEYDOWN *)nh;
        if (which == L_TRACK && k->wVKey == VK_DELETE) untrack_selected();
        if (which == L_ALL && k->wVKey == VK_RETURN) track_item(vlist_selected(v));
        if ((which == L_PW || which == L_SUM_FAV) && k->wVKey == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) copy_password(which);
        if (which == L_PW && k->wVKey == VK_SPACE) toggle_fav(vlist_selected(v));
        return 0;
    }
    }
    return 0;
}

/* ---- creation ------------------------------------------------------------------------------- */
static HWND mkpage(int i)
{
    HWND h = CreateWindowEx(WS_EX_CONTROLPARENT, "FMPage", "", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, g_main,
                            NULL, g_inst, NULL);
    SetWindowLongPtr(h, GWLP_USERDATA, i);
    return h;
}

static HWND mkedit(HWND parent, int id)
{
    HWND h = CreateWindowEx(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, parent,
                            (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)f_ui, TRUE);
    SendMessage(h, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search...");
    return h;
}

static void create_children(void)
{
    for (int i = 0; i < N_PAGES; i++) {
        g_pages[i] = mkpage(i);
        mkbutton(g_main, ID_TAB0 + i, PAGE_NAMES[i]);
    }
    g_emu_combo = CreateWindow("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10,
                               g_main, (HMENU)(INT_PTR)ID_EMU, g_inst, NULL);
    SendMessage(g_emu_combo, WM_SETFONT, (WPARAM)f_ui, TRUE);
    SetWindowTheme(g_emu_combo, L"DarkMode_CFD", NULL);
    mkbutton(g_main, ID_MENU, "Menu");

    static const char *const pool_cols[] = { "Card", "Chance", "Owned" };
    static const int pool_w[] = { 200, 70, 64 };
    /* summary page */
    HWND p = g_pages[PG_SUMMARY];
    g_sumcard = CreateWindow("FMSum", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, p, NULL, g_inst, NULL);
    static const char *const rank_cols[] = { "Stat", "Value", "Points", "Next change" };
    static const int rank_w[] = { 124, 46, 50, 150 };
    vlist_create(&g_lists[L_RANK], L_RANK, p, 4, rank_cols, rank_w, "lccl", t_rank, c_rank);
    int ten[N_RULES];
    for (int i = 0; i < N_RULES; i++) ten[i] = i;
    vlist_set_rows(&g_lists[L_RANK], ten, N_RULES);
    g_strip = CreateWindow("FMStrip", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, p, NULL, g_inst, NULL);
    g_stars = CreateWindow("FMStars", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, p, NULL, g_inst, NULL);
    static const char *const sf_cols[] = { "Card", "Password", "Cost", "Owned" };
    static const int sf_w[] = { 200, 100, 70, 64 };
    vlist_create(&g_lists[L_SUM_FAV], L_SUM_FAV, p, 4, sf_cols, sf_w, "lccc", t_sumfav, c_sumfav);

    /* cards page */
    p = g_pages[PG_CARDS];
    g_card_search = mkedit(p, ID_CARD_SEARCH);
    g_il = art_imagelist(S(30), S(28));
    static const char *const all_cols[] = { "Card / type", "Kind", "In deck", "In trunk", "Total" };
    static const int all_w[] = { 220, 120, 62, 66, 54 };
    vlist_create(&g_lists[L_ALL], L_ALL, p, 5, all_cols, all_w, "llccc", t_all, c_all);
    mkbutton(p, ID_TRACK, "Track  >");
    mkbutton(p, ID_UNTRACK, "<  Untrack");
    mkbutton(p, ID_MOVEUP, "Move up");
    mkbutton(p, ID_MOVEDN, "Move down");
    mkbutton(p, ID_RESETDEF, "Reset to default");
    mkbutton(p, ID_CLEAR, "Clear all");
    static const char *const tr_cols[] = { "Card / type", "In deck", "In trunk", "Total" };
    static const int tr_w[] = { 200, 66, 70, 56 };
    vlist_create(&g_lists[L_TRACK], L_TRACK, p, 4, tr_cols, tr_w, "lccc", t_track, c_track);
    if (g_il) {
        ListView_SetImageList(g_lists[L_ALL].hwnd, g_il, LVSIL_SMALL);
        ListView_SetImageList(g_lists[L_TRACK].hwnd, g_il, LVSIL_SMALL);
    }

    int dl[N_DUELISTS];
    for (int i = 0; i < N_DUELISTS; i++) dl[i] = i + 1;

    /* drops page */
    p = g_pages[PG_DROPS];
    static const char *const dd_cols[] = { "#", "Duelist", "Tables" };
    static const int dd_w[] = { 36, 170, 76 };
    vlist_create(&g_lists[L_DROPDUEL], L_DROPDUEL, p, 3, dd_cols, dd_w, "clc", t_dropduel, c_dropduel);
    vlist_set_rows(&g_lists[L_DROPDUEL], dl, N_DUELISTS);
    g_tier_combo = CreateWindow("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 10, 10, p,
                                (HMENU)(INT_PTR)ID_TIER, g_inst, NULL);
    SendMessage(g_tier_combo, WM_SETFONT, (WPARAM)f_ui, TRUE);
    SetWindowTheme(g_tier_combo, L"DarkMode_CFD", NULL);
    for (int i = 0; i < 3; i++) {
        char s[48];
        snprintf(s, sizeof s, "%s drops", DROP_TIER_NAMES[i]);
        SendMessage(g_tier_combo, CB_ADDSTRING, 0, (LPARAM)s);
    }
    SendMessage(g_tier_combo, CB_ADDSTRING, 0, (LPARAM)"Deck pool");
    SendMessage(g_tier_combo, CB_SETCURSEL, 0, 0);
    vlist_create(&g_lists[L_DETAIL], L_DETAIL, p, 3, pool_cols, pool_w, "lcc", t_detail, c_pool);
    snprintf(t_detail_title, sizeof t_detail_title, "Select a duelist");

    /* passwords page */
    p = g_pages[PG_PW];
    g_pw_search = mkedit(p, ID_PW_SEARCH);
    mkbutton(p, ID_PW_UNOWNED, "Not owned");
    mkbutton(p, ID_PW_AFFORD, "Affordable now");
    mkbutton(p, ID_PW_FAVONLY, "Favorites only");
    mkbutton(p, ID_PW_FAV, "Favorite / Unfavorite");
    static const char *const pw_cols[] = { "Fav", "ID", "Name", "Password", "Cost", "Type", "Attr", "Lv", "ATK", "DEF",
                                           "Owned" };
    static const int pw_w[] = { 40, 44, 240, 100, 70, 116, 70, 40, 60, 60, 66 };
    vlist_create(&g_lists[L_PW], L_PW, p, 11, pw_cols, pw_w, "cllcclccccc", t_pw, c_pw);

    rebuild_all_cards();
    rebuild_tracked();
    rebuild_pw();
    rebuild_favs();
    snprintf(t_last_drops, sizeof t_last_drops, "-");
    update_dstats(1);
}

/* ---- main window procedure --------------------------------------------------------------------- */
static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_main = h;
        create_children();
        return 0;
    case WM_SIZE:
        if (g_pages[0]) layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = S(1060);
        mm->ptMinTrackSize.y = S(660);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint_main(h); return 0;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_PANEL2);
        return (LRESULT)br_panel2;
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT *)l);
        return TRUE;
    case WM_NOTIFY:
        return on_list_notify((NMHDR *)l);
    case WM_TIMER:
        if (w == TIMER_POLL) on_tick();
        else if (w == 2) {
            KillTimer(h, 2);
            refresh_emus();
            if (g_nemus && !g_mode) start_attach(1);
        }
        return 0;
    case WM_APP_ATTACHED:
        on_attached((int)w);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id >= ID_TAB0 && id < ID_TAB0 + N_PAGES) { show_page(id - ID_TAB0); return 0; }
        switch (id) {
        case ID_MENU: show_main_menu(); break;
        case ID_STARDISC: pick_star_disc(); break;
        case ID_REFRESH: refresh_emus(); logf_("Found %d emulator process(es).", g_nemus); break;
        case ID_ATTACH: start_attach(0); break;
        case ID_DETACH: detach_all(); logf_("Detached."); break;
        case ID_DEMO: start_demo(); break;
        case ID_OVERLAY: toggle_overlay(); break;
        case ID_TRACK: track_item(vlist_selected(&g_lists[L_ALL])); break;
        case ID_UNTRACK: untrack_selected(); break;
        case ID_MOVEUP: move_selected(-1); break;
        case ID_MOVEDN: move_selected(1); break;
        case ID_RESETDEF:
            g_ntracked = 0;
            for (int i = 0; i < MAX_TRACK; i++) g_tracked[g_ntracked++] = DEFAULT_TRACK[i];
            tracked_changed();
            break;
        case ID_CLEAR:
            if (g_ntracked && MessageBox(h, "Remove all tracked cards from Summary?", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_ntracked = 0;
                tracked_changed();
            }
            break;
        case ID_PW_UNOWNED: g_pw_unowned = !g_pw_unowned; InvalidateRect(g_btn[id - 100], NULL, FALSE); rebuild_pw(); break;
        case ID_PW_AFFORD: g_pw_afford = !g_pw_afford; InvalidateRect(g_btn[id - 100], NULL, FALSE); rebuild_pw(); break;
        case ID_PW_FAVONLY: g_pw_favonly = !g_pw_favonly; InvalidateRect(g_btn[id - 100], NULL, FALSE); rebuild_pw(); break;
        case ID_PW_FAV: {
            int fid = vlist_selected(&g_lists[L_PW]);
            if (fid > 0) toggle_fav(fid);
            else MessageBox(h, "Select a card in the list first.", APP_NAME, MB_ICONINFORMATION);
            break;
        }
        case ID_CARD_SEARCH: if (code == EN_CHANGE) rebuild_all_cards(); break;
        case ID_PW_SEARCH: if (code == EN_CHANGE) rebuild_pw(); break;
        case ID_TIER: if (code == CBN_SELCHANGE) fill_detail(); break;
        }
        return 0;
    }
    case WM_CLOSE:
        save_settings();
        save_tracked();
        save_history();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_mode == 1) emu_detach(&g_att);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static HFONT mkfont(const char *face, int pt, int weight)
{
    return CreateFont(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (g_dpi < 96) g_dpi = 96;

    GetModuleFileName(NULL, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    strncat(g_dir, "\\final7_data", MAX_PATH - strlen(g_dir) - 1);
    CreateDirectory(g_dir, NULL);

    g_ram = calloc(1, PS1_RAM_SIZE);
    g_pools = calloc(N_DUELISTS + 1, sizeof *g_pools);
    if (!g_ram || !g_pools) return 1;
    tracker_reset(&g_tr);
    load_settings();
    load_tracked();
    load_history();
    load_pools();
    load_favs();
    art_init();
    {
        char cache[MAX_PATH], exedir[MAX_PATH];
        data_path(cache, "star_icons.bin");
        if (!stars_load(cache)) {
            GetModuleFileName(NULL, exedir, MAX_PATH);
            char *sl = strrchr(exedir, '\\');
            if (sl) *sl = 0;
            if (stars_autoload(exedir)) stars_save(cache);
        }
    }

    f_ui = mkfont("Segoe UI", 10, FW_NORMAL);
    f_bold = mkfont("Segoe UI", 10, FW_BOLD);
    f_small = mkfont("Segoe UI", 9, FW_NORMAL);
    f_smallb = mkfont("Segoe UI", 8, FW_BOLD);
    f_title = mkfont("Segoe UI", 16, FW_BOLD);
    f_huge = mkfont("Segoe UI", 72, FW_BOLD);
    f_big = mkfont("Segoe UI", 22, FW_BOLD);
    f_med = mkfont("Segoe UI", 13, FW_BOLD);
    f_lp = mkfont("Segoe UI", 30, FW_BOLD);
    f_lpnum = mkfont("Segoe UI", 17, FW_BOLD);
    f_mono = mkfont("Consolas", 10, FW_NORMAL);
    f_count = mkfont("Impact", 15, FW_NORMAL);
    br_bg = CreateSolidBrush(C_BG);
    br_panel = CreateSolidBrush(C_PANEL);
    br_panel2 = CreateSolidBrush(C_PANEL2);

    WNDCLASSEX wc = { sizeof wc };
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.hIconSm = wc.hIcon;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = "FMTrackerMain";
    wc.hbrBackground = br_bg;
    RegisterClassEx(&wc);
    wc.hIcon = wc.hIconSm = NULL;
    wc.lpfnWndProc = page_proc;
    wc.lpszClassName = "FMPage";
    wc.hbrBackground = br_panel;
    RegisterClassEx(&wc);
    wc.lpfnWndProc = stars_proc;
    wc.lpszClassName = "FMStars";
    RegisterClassEx(&wc);
    wc.lpfnWndProc = strip_proc;
    wc.lpszClassName = "FMStrip";
    RegisterClassEx(&wc);
    wc.lpfnWndProc = sum_proc;
    wc.lpszClassName = "FMSum";
    RegisterClassEx(&wc);
    wc.lpfnWndProc = overlay_proc;
    wc.lpszClassName = "FMOverlay";
    wc.hIcon = wc.hIconSm = LoadIcon(inst, MAKEINTRESOURCE(1));
    RegisterClassEx(&wc);

    int ww = g_set_w > 400 ? g_set_w : S(1260), wh = g_set_h > 300 ? g_set_h : S(820);
    HWND h = CreateWindowEx(0, "FMTrackerMain", APP_NAME,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, g_set_x, g_set_y, ww, wh, NULL, NULL, inst, NULL);
    if (!h) return 1;
    /* keep the window on a visible monitor */
    RECT wr;
    GetWindowRect(h, &wr);
    if (!MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) SetWindowPos(h, NULL, 40, 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    layout();
    show_page(PG_SUMMARY);
    ShowWindow(h, g_set_max ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(h);
    logf_(APP_NAME " ready. Start your emulator, load your save, then press Attach (or Demo to try it).");
    logf_("Data folder: %s", g_dir);
    refresh_emus();
    SetTimer(h, TIMER_POLL, 250, NULL);
    if (cmd && strstr(cmd, "--page=")) show_page(atoi(strstr(cmd, "--page=") + 7) % N_PAGES);
    if (cmd && strstr(cmd, "--demo")) start_demo();
    else SetTimer(h, 2, 600, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
