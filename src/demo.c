/* final7 - a fake game that plays endless duels, for trying the tracker out. */
#include <string.h>

#include "fm.h"

static uint32_t seed = 7;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) % (unsigned)n); }

static void w16(uint8_t *ram, int off, int v) { ram[off] = (uint8_t)v; ram[off + 1] = (uint8_t)(v >> 8); }
static int r16s(const uint8_t *ram, int off) { return (int16_t)(ram[off] | (ram[off + 1] << 8)); }

static uint64_t next_tick;
static int phase;   /* 0 = start next duel, 1 = dueling, 2 = results screen */
static int step;

void demo_init(uint8_t *ram)
{
    static const int pick[] = { 1, 2, 5, 9, 14, 20, 23, 64, 113, 302, 330, 337, 347, 350, 400, 410, 412 };
    memset(ram, 0, PS1_RAM_SIZE);
    seed = 7;
    for (int i = 0; i < 40; i++) {
        int id = pick[i % (sizeof pick / sizeof pick[0])];   /* legal deck: at most 3 copies */
        w16(ram, RAM_DECK + 2 * i, id);
        w16(ram, RAM_DECK_MIRROR + 2 * i, id);
    }
    for (int id = 1; id < 400; id++) {
        static const int c[] = { 0, 0, 1, 2, 3, 5 };
        ram[RAM_TRUNK + id - 1] = (uint8_t)c[rnd(6)];
    }
    ram[RAM_STARCHIPS] = 1234 & 0xFF;
    ram[RAM_STARCHIPS + 1] = 1234 >> 8;
    for (int d = 1; d <= N_DUELISTS; d++) {
        w16(ram, RAM_RECORDS + d * 4, rnd(13));
        w16(ram, RAM_RECORDS + d * 4 + 2, rnd(7));
    }
    for (int r = 0; r < N_RULES; r++)
        for (int i = 0; i < 5; i++) {
            w16(ram, RAM_RANK_TABLE + r * 20 + i * 4, DEFAULT_RANK_TABLE[r][i].thr);
            w16(ram, RAM_RANK_TABLE + r * 20 + i * 4 + 2, DEFAULT_RANK_TABLE[r][i].chg);
        }
    phase = 0;
    next_tick = 0;
}

static void make_pool(uint8_t *ram, int addr, int n)
{
    for (int i = 0; i < N_CARDS; i++) w16(ram, addr + 2 * i, 0);
    int left = 2048;
    for (int k = 0; k < n; k++) {
        int id = 1 + rnd(N_CARDS);
        int w = (k == n - 1) ? left : 2048 / n;
        int cur = ram[addr + 2 * (id - 1)] | (ram[addr + 2 * (id - 1) + 1] << 8);
        w16(ram, addr + 2 * (id - 1), cur + w);
        left -= w;
    }
}

void demo_step(uint8_t *ram, uint64_t now)
{
    if (now < next_tick) return;
    const int b = RAM_RANK_BLOCK, c = RAM_RANK_BLOCK + 0x20;
    if (phase == 0) {
        ram[RAM_OPPONENT] = (uint8_t)(1 + rnd(N_DUELISTS));
        memset(ram + b, 0, 0x40);
        w16(ram, b + 0x14, 8000); w16(ram, b + 0x16, 8000);
        w16(ram, c + 0x14, 8000); w16(ram, c + 0x16, 8000);
        make_pool(ram, RAM_CPU_DECK_POOL, 30);
        make_pool(ram, RAM_CPU_DROP_SAPOW, 25);
        make_pool(ram, RAM_CPU_DROP_BCD, 25);
        make_pool(ram, RAM_CPU_DROP_SATEC, 25);
        step = 0;
        phase = 1;
        next_tick = now + 1500;
        return;
    }
    if (phase == 2) {
        phase = 0;
        next_tick = now + 1000;
        return;
    }
    step++;
    ram[b + 0x01] = (uint8_t)(step / 2 + 1);
    ram[c + 0x01] = (uint8_t)(step / 2);
    static const int offs[] = { 0x02, 0x04, 0x08, 0x09, 0x05, 0x18 };
    for (int i = 0; i < 6; i++)
        if (rnd(4) == 0 && ram[b + offs[i]] < 255) ram[b + offs[i]]++;
    int ly = r16s(ram, b + 0x14), lc = r16s(ram, c + 0x14);
    static const int hit_c[] = { 300, 500, 800, 1200 }, hit_y[] = { 200, 400, 700 };
    if (rnd(2)) lc -= hit_c[rnd(4)]; else ly -= hit_y[rnd(3)];
    if (lc < 0) lc = 0;
    if (ly < 0) ly = 0;
    w16(ram, b + 0x14, ly);
    w16(ram, c + 0x14, lc);
    if (lc == 0 || ly == 0) {
        if (lc == 0) {
            ram[b] = 2;
            int drop = 1 + rnd(400);
            if (ram[RAM_TRUNK + drop - 1] < 250) ram[RAM_TRUNK + drop - 1]++;
            memmove(ram + RAM_LAST_DROPS + 2, ram + RAM_LAST_DROPS, 18);
            w16(ram, RAM_LAST_DROPS, drop);
        } else {
            ram[c] = 2;
        }
        phase = 2;
        next_tick = now + 6000;
    } else {
        next_tick = now + 900;
    }
}
