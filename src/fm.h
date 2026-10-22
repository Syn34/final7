/* FM Tracker - shared declarations.
 *
 * Yu-Gi-Oh! Forbidden Memories (NTSC-U, SLUS-01411) live companion.
 * All RAM offsets are PS1 addresses masked to 0x1FFFFF.
 */
#ifndef FM_H
#define FM_H

#include <stdint.h>

#define N_CARDS      722
#define N_DUELISTS   39
#define PS1_RAM_SIZE 0x200000

/* ---- RAM map (sources: Data Crystal RAM map, Unchiga's FM recomp notes) ---- */
#define RAM_DECK          0x1D0200  /* 40 x u16 card id                     */
#define RAM_TRUNK         0x1D0250  /* 722 x u8 copies in the chest         */
#define RAM_DECK_MIRROR   0x1D3200  /* second copy of the save block        */
#define RAM_RECORDS       0x1D071C  /* (u16 win, u16 loss) at +id*4, id 1-39 */
#define RAM_LAST_DROPS    0x1D07BC  /* 10 x u16 most recent drops           */
#define RAM_STARCHIPS     0x1D07E0  /* u32                                  */
#define RAM_OPPONENT      0x09B361  /* u8, 1..39                            */
#define RAM_RANK_BLOCK    0x0E9FF0  /* player stats; opponent at +0x20      */
#define RAM_RANK_TABLE    0x1798A8  /* 10 rules x 5 x (s16 thr, s16 change) */
#define RAM_CPU_DECK_POOL 0x1781D8  /* 722 x u16 weights, sum 2048          */
#define RAM_CPU_DROP_SAPOW 0x17878C
#define RAM_CPU_DROP_BCD   0x178D40
#define RAM_CPU_DROP_SATEC 0x1792F4

typedef struct {
    int id;
    const char *name, *type, *attr;
    int level, atk, def;
    const char *password;   /* "" when the card has none */
    int cost;               /* starchips, -1 = cannot be bought */
} Card;

extern const Card CARDS[N_CARDS];          /* CARDS[id-1] */
extern const char *const DUELISTS[N_DUELISTS];
extern const int DIFFICULTY[N_DUELISTS];
extern const char *const DROP_TIER_NAMES[3];

int card_is_monster(int id);

/* ---- rank ---------------------------------------------------------------- */
#define N_RULES 10
typedef struct { int16_t thr, chg; } RankRow;

typedef struct {
    int rule;              /* index into the game's table */
    const char *name;
    int off, width;        /* location in the stats block */
} RankRule;
extern const RankRule RANK_RULES[N_RULES];   /* display order */
extern const RankRow DEFAULT_RANK_TABLE[N_RULES][5];

typedef struct {
    int victory;           /* s8 tag: 0 during the duel, 2 normal win, 40 Exodia, -40 deck-out */
    int counters[N_RULES]; /* indexed by the game's rule index */
    int lp, start_lp;
} Side;

typedef struct {
    int save_loaded;
    uint16_t deck[40];
    uint8_t deck_count[N_CARDS + 1];
    uint8_t trunk[N_CARDS + 1];
    uint32_t starchips;
    uint16_t rec_w[N_DUELISTS + 1], rec_l[N_DUELISTS + 1];
    uint16_t last_drops[10];
    int n_drops;
    int opponent;
    Side you, cpu;
    RankRow table[N_RULES][5];
    int table_live;
    uint16_t deck_pool[N_CARDS];
    uint16_t drop_pool[3][N_CARDS];
    int pools_valid;
} Snapshot;

void snapshot_parse(const uint8_t *ram, Snapshot *s);
int  ram_looks_like_save(const uint8_t *deck80, const uint8_t *mirror80, uint32_t starchips, int opp);
int  owned_total(const Snapshot *s, int id);

typedef struct {
    int value, change;
    char note[40];
} RankLine;
int  rank_breakdown(const Side *side, RankRow table[N_RULES][5], int assume_win_bonus, RankLine out[N_RULES]);
void rank_label(int score, char *letter, int *is_pow, char *label8);
int  drop_tier(int score);

/* ---- win chance ---------------------------------------------------------- */
double deck_power(const Snapshot *s);
double pool_power(const uint16_t *pool);
double win_chance(int wins, int losses, int difficulty, double my_power, double cpu_power,
                  int live, int my_lp, int cpu_lp, int start_lp);

/* ---- duel tracking ------------------------------------------------------- */
enum { EV_NONE = 0, EV_START, EV_END, EV_ABORT };
typedef struct {
    int active, opponent, first;
    uint64_t zero_lp_since;
    uint16_t start_w, start_l;
    uint16_t drops_at_start[10];
    int n_drops_at_start;
} DuelTracker;

typedef struct {
    int type;          /* EV_* */
    int opponent;
    int win;           /* EV_END: 1 win, 0 loss */
    int kind;          /* 0 normal, 1 exodia, 2 deck out */
    Side you, cpu;
} DuelEvent;

void tracker_reset(DuelTracker *t);
int  tracker_update(DuelTracker *t, const Snapshot *s, uint64_t now_ms, DuelEvent *ev);

/* ---- emulator access ----------------------------------------------------- */
enum { EMU_DUCKSTATION, EMU_EPSXE, EMU_RETROARCH, EMU_MEDNAFEN, EMU_PSX, EMU_PCSXREDUX,
       EMU_XEBRA, EMU_GENERIC };

typedef struct {
    unsigned long pid;
    char exe[260];
    int kind;
} EmuProc;

typedef struct {
    void *handle;
    int is64;
    unsigned long pid;
    char exe[260];
    uint64_t ptr_addr;     /* address holding a pointer to PS1 RAM */
    uint64_t static_base;  /* or a fixed PS1 RAM address */
    char method[96];
} Attach;

int  emu_find(EmuProc *out, int max);
int  emu_attach(Attach *a, const EmuProc *p, char *err, int errlen, volatile int *progress);
int  emu_read_ram(Attach *a, uint8_t *buf);   /* 1 ok, 0 unreadable */
int  emu_alive(Attach *a);
void emu_detach(Attach *a);

/* ---- demo ---------------------------------------------------------------- */
void demo_init(uint8_t *ram);
void demo_step(uint8_t *ram, uint64_t now_ms);

#endif
