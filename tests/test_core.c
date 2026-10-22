/* Core tests. Host build:  gcc -Isrc tests/test_core.c src/game.c src/demo.c src/cards_data.c -lm
 * On Windows (or under Wine) also add src/emu.c and -DTEST_WIN32 to test attaching via the memory scan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm.h"
#ifdef TEST_WIN32
#include <windows.h>
__declspec(dllexport) uint8_t *RAM;   /* mimics DuckStation's exported RAM pointer */
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    uint8_t *ram = calloc(1, PS1_RAM_SIZE);
    static Snapshot s;
    char lab[8];

    snapshot_parse(ram, &s);
    CHECK(!s.save_loaded);

    demo_init(ram);
    snapshot_parse(ram, &s);
    CHECK(s.save_loaded);
    CHECK(s.starchips == 1234);
    CHECK(s.table_live);

    rank_label(95, NULL, NULL, lab); CHECK(!strcmp(lab, "S-POW"));
    rank_label(5, NULL, NULL, lab);  CHECK(!strcmp(lab, "S-TEC"));
    rank_label(50, NULL, NULL, lab); CHECK(!strcmp(lab, "D-POW"));
    rank_label(49, NULL, NULL, lab); CHECK(!strcmp(lab, "D-TEC"));
    CHECK(drop_tier(85) == 0 && drop_tier(15) == 2 && drop_tier(55) == 1);

    Side quick = { 0, { 3, 1, 0, 0, 0, 0, 5, 8000, 0, 0 }, 8000, 8000 };
    RankLine rl[N_RULES];
    RankRow t[N_RULES][5];
    memcpy(t, DEFAULT_RANK_TABLE, sizeof t);
    CHECK(rank_breakdown(&quick, t, 1, rl) >= 90);

    CHECK(!strcmp(CARDS[0].password, "89631139"));
    CHECK(card_is_monster(1) && !card_is_monster(330 /* a magic card id in range */) == !card_is_monster(330));

    /* full duel cycle through the tracker */
    DuelTracker tr;
    DuelEvent ev;
    tracker_reset(&tr);
    int starts = 0, ends = 0;
    for (uint64_t now = 1000; now < 300000; now += 500) {
        demo_step(ram, now);
        snapshot_parse(ram, &s);
        if (tracker_update(&tr, &s, now, &ev)) { starts += ev.type == EV_START; ends += ev.type == EV_END; }
    }
    CHECK(starts >= 3 && ends >= 3);

#ifdef TEST_WIN32
    /* attach to ourselves: the generic scan must find `ram` */
    EmuProc me = { GetCurrentProcessId(), "", EMU_GENERIC };
    GetModuleFileNameA(NULL, me.exe, sizeof me.exe);
    char *b = strrchr(me.exe, '\\');
    if (b) memmove(me.exe, b + 1, strlen(b));
    Attach a;
    char err[256];
    volatile int prog = 0;
    demo_init(ram);
    CHECK(emu_attach(&a, &me, err, sizeof err, &prog));
    uint8_t *copy = malloc(PS1_RAM_SIZE);
    CHECK(emu_read_ram(&a, copy) && !memcmp(copy, ram, PS1_RAM_SIZE));
    printf("scan found %p (ram at %p) via %s\n", (void *)(uintptr_t)a.static_base, (void *)ram, a.method);
    emu_detach(&a);

    /* DuckStation-style: pointer published through the "RAM" export of the main module */
    RAM = ram;
    me.kind = EMU_DUCKSTATION;
    CHECK(emu_attach(&a, &me, err, sizeof err, &prog));
    CHECK(emu_read_ram(&a, copy) && !memcmp(copy, ram, PS1_RAM_SIZE));
    printf("export lookup: %s\n", a.method);
    emu_detach(&a);
#endif
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
