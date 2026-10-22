/* final7 - game memory parsing, duel rank arithmetic, duel detection, win chance. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fm.h"

const char *const DUELISTS[N_DUELISTS] = {
    "Simon Muran", "Teana", "Jono", "Villager 1", "Villager 2", "Villager 3", "Seto",
    "Heishin", "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi",
    "Yami Bakura", "Pegasus", "Isis", "Kaiba", "Mage Soldier", "Jono 2nd", "Teana 2nd",
    "Ocean Mage", "High Mage Secmeton", "Forest Mage", "High Mage Anubisius",
    "Mountain Mage", "High Mage Atenza", "Desert Mage", "High Mage Martis", "Meadow Mage",
    "High Mage Kepura", "Labyrinth Mage", "Seto 2nd", "Guardian Sebek", "Guardian Neku",
    "Heishin 2nd", "Seto 3rd", "DarkNite", "Nitemare", "Duel Master K",
};

/* Rough difficulty 1 (easy) .. 5 (brutal); only matters before real results exist. */
const int DIFFICULTY[N_DUELISTS] = {
    1, 1, 1, 1, 1, 1, 2, 3, 2, 2, 2, 3, 3, 3, 3, 3, 4, 3, 2, 2,
    3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 4,
};

const char *const DROP_TIER_NAMES[3] = { "S/A-POW", "B/C/D", "S/A-TEC" };

/* Display order; `rule` is the game's table index (decomp: DUEL_RANK_RULE_*). */
const RankRule RANK_RULES[N_RULES] = {
    { 0, "Turns",             0x01, 1 },
    { 1, "Effective attacks", 0x02, 1 },
    { 2, "Defensive wins",    0x03, 1 },
    { 3, "Face-down plays",   0x04, 1 },
    { 8, "Fusions",           0x08, 1 },
    { 9, "Equip magic",       0x09, 1 },
    { 4, "Pure magic",        0x05, 1 },
    { 5, "Trigger traps",     0x06, 1 },
    { 6, "Cards used",        0x18, 1 },
    { 7, "Remaining LP",      0x14, 2 },
};

/* Retail values; the live table is read from RAM whenever the game has it loaded. */
const RankRow DEFAULT_RANK_TABLE[N_RULES][5] = {
    { {5, 12}, {9, 8}, {29, 0}, {33, -8}, {32767, -12} },        /* turns */
    { {2, 4}, {4, 2}, {10, 0}, {20, -2}, {32767, -4} },          /* effective attacks */
    { {2, 0}, {6, -10}, {10, -20}, {15, -30}, {32767, -40} },    /* defensive wins */
    { {1, 0}, {11, -2}, {21, -4}, {31, -6}, {32767, -8} },       /* face-down plays */
    { {1, 2}, {4, -4}, {7, -8}, {10, -12}, {32767, -16} },       /* pure magic */
    { {1, 2}, {3, -8}, {5, -16}, {7, -24}, {32767, -32} },       /* trigger traps */
    { {9, 15}, {13, 12}, {33, 0}, {37, -5}, {32767, -7} },       /* cards used */
    { {100, -7}, {1000, -5}, {7000, 0}, {8000, 4}, {32767, 6} }, /* remaining LP */
    { {1, 4}, {5, 0}, {10, -4}, {15, -8}, {32767, -12} },        /* fusions */
    { {1, 4}, {5, 0}, {10, -4}, {15, -8}, {32767, -12} },        /* equip magic */
};

#define RANK_BASE 50
#define NORMAL_WIN_BONUS 2

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int card_is_monster(int id)
{
    if (id < 1 || id > N_CARDS) return 0;
    const char *t = CARDS[id - 1].type;
    return strcmp(t, "Magic") && strcmp(t, "Trap") && strcmp(t, "Equip") && strcmp(t, "Ritual");
}

static int valid_deck(const uint8_t *p)
{
    for (int i = 0; i < 40; i++) {
        unsigned id = rd16(p + 2 * i);
        if (id < 1 || id > N_CARDS) return 0;
    }
    return 1;
}

int ram_looks_like_save(const uint8_t *deck80, const uint8_t *mirror80, uint32_t starchips, int opp)
{
    return valid_deck(deck80) && valid_deck(mirror80) && starchips <= 999999 && opp <= N_DUELISTS;
}

static void read_side(const uint8_t *ram, int base, Side *s)
{
    memset(s, 0, sizeof *s);
    s->victory = (int8_t)ram[base];
    for (int i = 0; i < N_RULES; i++) {
        const RankRule *r = &RANK_RULES[i];
        s->counters[r->rule] = r->width == 2 ? rds16(ram + base + r->off) : ram[base + r->off];
    }
    s->lp = rds16(ram + base + 0x14);
    s->start_lp = rds16(ram + base + 0x16);
}

static int read_table(const uint8_t *ram, RankRow out[N_RULES][5])
{
    for (int rule = 0; rule < N_RULES; rule++) {
        int prev = -0x8000;
        for (int i = 0; i < 5; i++) {
            const uint8_t *p = ram + RAM_RANK_TABLE + rule * 20 + i * 4;
            int thr = rds16(p), chg = rds16(p + 2);
            if (thr < prev) return 0;
            prev = thr;
            out[rule][i].thr = (int16_t)thr;
            out[rule][i].chg = (int16_t)chg;
        }
        if (prev != 32767) return 0;
    }
    return 1;
}

static int read_pool(const uint8_t *ram, int addr, uint16_t *out)
{
    unsigned sum = 0;
    for (int i = 0; i < N_CARDS; i++) {
        out[i] = rd16(ram + addr + 2 * i);
        sum += out[i];
    }
    return sum == 2048;
}

void snapshot_parse(const uint8_t *ram, Snapshot *s)
{
    memset(s, 0, sizeof *s);
    s->opponent = ram[RAM_OPPONENT];
    s->starchips = rd32(ram + RAM_STARCHIPS);
    s->save_loaded = ram_looks_like_save(ram + RAM_DECK, ram + RAM_DECK_MIRROR, s->starchips, 0);
    if (s->save_loaded) {
        for (int i = 0; i < 40; i++) {
            s->deck[i] = rd16(ram + RAM_DECK + 2 * i);
            if (s->deck_count[s->deck[i]] < 255) s->deck_count[s->deck[i]]++;
        }
        for (int id = 1; id <= N_CARDS; id++) s->trunk[id] = ram[RAM_TRUNK + id - 1];
        for (int d = 1; d <= N_DUELISTS; d++) {
            s->rec_w[d] = rd16(ram + RAM_RECORDS + d * 4);
            s->rec_l[d] = rd16(ram + RAM_RECORDS + d * 4 + 2);
        }
        for (int i = 0; i < 10; i++) {
            unsigned id = rd16(ram + RAM_LAST_DROPS + 2 * i);
            if (id >= 1 && id <= N_CARDS) s->last_drops[s->n_drops++] = (uint16_t)id;
        }
    } else {
        s->starchips = 0;
    }
    read_side(ram, RAM_RANK_BLOCK, &s->you);
    read_side(ram, RAM_RANK_BLOCK + 0x20, &s->cpu);
    s->table_live = read_table(ram, s->table);
    if (!s->table_live) memcpy(s->table, DEFAULT_RANK_TABLE, sizeof s->table);
    int ok = read_pool(ram, RAM_CPU_DECK_POOL, s->deck_pool);
    ok &= read_pool(ram, RAM_CPU_DROP_SAPOW, s->drop_pool[0]);
    ok &= read_pool(ram, RAM_CPU_DROP_BCD, s->drop_pool[1]);
    ok &= read_pool(ram, RAM_CPU_DROP_SATEC, s->drop_pool[2]);
    s->pools_valid = ok;
}

int owned_total(const Snapshot *s, int id)
{
    return s->trunk[id] + s->deck_count[id];
}

/* ---- rank ---------------------------------------------------------------- */
static int rule_change(RankRow t[N_RULES][5], int rule, int v)
{
    for (int i = 0; i < 5; i++)
        if (v < t[rule][i].thr) return t[rule][i].chg;
    return t[rule][4].chg;
}

int rank_breakdown(const Side *side, RankRow t[N_RULES][5], int assume_win_bonus, RankLine out[N_RULES])
{
    int victory = side->victory;
    if (victory == 0 && assume_win_bonus) victory = NORMAL_WIN_BONUS;
    int score = RANK_BASE + victory;
    for (int i = 0; i < N_RULES; i++) {
        int rule = RANK_RULES[i].rule;
        int v = side->counters[rule];
        if (rule == 7 && v < 0) v = 0;
        int chg = rule_change(t, rule, v);
        score += chg;
        out[i].value = v;
        out[i].change = chg;
        out[i].note[0] = 0;
        for (int k = 0; k < 5; k++) {
            if (v < t[rule][k].thr) {
                int thr = t[rule][k].thr;
                if (thr < 32767) {
                    int next = rule_change(t, rule, thr);
                    if (rule == 7)
                        snprintf(out[i].note, sizeof out[i].note, "at %d LP: %+d", thr, next);
                    else
                        snprintf(out[i].note, sizeof out[i].note, "%d more: %+d", thr - v, next);
                }
                break;
            }
        }
    }
    return score;
}

void rank_label(int score, char *letter, int *is_pow, char *label8)
{
    int s = score < 0 ? 0 : (score > 99 ? 99 : score);
    static const char L[] = "DCBAS";
    char c;
    int pow = s >= 50;
    c = pow ? L[(s - 50) / 10] : L[(49 - s) / 10];
    if (letter) *letter = c;
    if (is_pow) *is_pow = pow;
    if (label8) snprintf(label8, 8, "%c-%s", c, pow ? "POW" : "TEC");
}

int drop_tier(int score)
{
    char c;
    int pow;
    rank_label(score, &c, &pow, NULL);
    if (c == 'S' || c == 'A') return pow ? 0 : 2;
    return 1;
}

/* ---- win chance ---------------------------------------------------------- */
static double logit(double p)
{
    if (p < 0.01) p = 0.01;
    if (p > 0.99) p = 0.99;
    return log(p / (1 - p));
}

static double sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

double deck_power(const Snapshot *s)
{
    int atk[40], n = 0;
    for (int i = 0; i < 40; i++)
        if (card_is_monster(s->deck[i])) atk[n++] = CARDS[s->deck[i] - 1].atk;
    if (!n) return 0;
    for (int i = 1; i < n; i++)            /* insertion sort, descending */
        for (int j = i; j > 0 && atk[j] > atk[j - 1]; j--) { int t = atk[j]; atk[j] = atk[j - 1]; atk[j - 1] = t; }
    int k = n < 5 ? n : 5;
    double sum = 0;
    for (int i = 0; i < k; i++) sum += atk[i];
    return sum / k;
}

double pool_power(const uint16_t *pool)
{
    double tot = 0, w_atk = 0;
    for (int i = 0; i < N_CARDS; i++) {
        if (pool[i] && card_is_monster(i + 1)) {
            tot += pool[i];
            w_atk += pool[i] * (double)CARDS[i].atk;
        }
    }
    return tot > 0 ? w_atk / tot : 0;
}

static const double DIFF_PRIOR[6] = { 0.6, 0.90, 0.75, 0.60, 0.45, 0.30 };

double win_chance(int wins, int losses, int difficulty, double my_power, double cpu_power,
                  int live, int my_lp, int cpu_lp, int start_lp)
{
    double x = logit(DIFF_PRIOR[difficulty >= 1 && difficulty <= 5 ? difficulty : 0]);
    if (my_power > 0 && cpu_power > 0)
        x += (my_power - cpu_power * 1.6) / 900.0;
    int n = wins + losses;
    if (n) {
        double hist = (wins + 1.0) / (n + 2.0);
        double w = n / (n + 4.0);
        x = (1 - w) * x + w * logit(hist);
    }
    if (live) {
        if (cpu_lp <= 0 && my_lp > 0) return 1.0;
        if (my_lp <= 0) return 0.0;
        if (start_lp < 1) start_lp = 8000;
        x += 3.2 * (my_lp - cpu_lp) / (double)start_lp;
    }
    return sigmoid(x);
}

/* ---- duel tracking ------------------------------------------------------- */
void tracker_reset(DuelTracker *t)
{
    memset(t, 0, sizeof *t);
    t->first = 1;
}

static int fresh_duel(const Snapshot *s)
{
    const Side *y = &s->you, *c = &s->cpu;
    if (!(s->table_live && y->start_lp > 0 && y->lp == y->start_lp && c->lp == c->start_lp &&
          y->victory == 0 && c->victory == 0 && s->opponent >= 1 && s->opponent <= N_DUELISTS))
        return 0;
    for (int r = 0; r < N_RULES; r++)
        if (r != 7 && y->counters[r] != 0) return 0;
    return 1;
}

static void arm(DuelTracker *t, const Snapshot *s)
{
    t->active = 1;
    t->opponent = s->opponent;
    t->zero_lp_since = 0;
    t->start_w = s->rec_w[s->opponent];
    t->start_l = s->rec_l[s->opponent];
    memcpy(t->drops_at_start, s->last_drops, sizeof t->drops_at_start);
    t->n_drops_at_start = s->n_drops;
}

int tracker_update(DuelTracker *t, const Snapshot *s, uint64_t now, DuelEvent *ev)
{
    const Side *y = &s->you, *c = &s->cpu;
    memset(ev, 0, sizeof *ev);
    int in_progress = s->table_live && s->opponent >= 1 && s->opponent <= N_DUELISTS &&
                      y->start_lp > 0 && y->victory == 0 && c->victory == 0;
    if (!t->active) {
        int start = fresh_duel(s);
        if (!start && t->first && in_progress && y->lp > 0 && c->lp > 0) start = 1; /* attached mid-duel */
        t->first = 0;
        if (start) {
            arm(t, s);
            ev->type = EV_START;
            ev->opponent = s->opponent;
            return 1;
        }
        return 0;
    }
    t->first = 0;
    if (fresh_duel(s) && s->opponent != t->opponent) {   /* new duel began; we missed the end */
        arm(t, s);
        ev->type = EV_START;
        ev->opponent = s->opponent;
        return 1;
    }
    int ended = -1;
    if (y->victory != 0 && c->victory == 0) ended = 1;
    else if (c->victory != 0 && y->victory == 0) ended = 0;
    else if (y->lp <= 0 || c->lp <= 0) {
        if (!t->zero_lp_since) t->zero_lp_since = now;
        if (now - t->zero_lp_since > 4000) ended = y->lp <= 0 ? 0 : 1;
    } else if (!s->table_live) {
        t->active = 0;
        ev->type = EV_ABORT;
        ev->opponent = t->opponent;
        return 1;
    }
    if (ended >= 0) {
        t->active = 0;
        ev->type = EV_END;
        ev->opponent = t->opponent;
        ev->win = ended;
        ev->kind = (ended && y->victory == 40) ? 1 : ((ended && y->victory == -40) ? 2 : 0);
        ev->you = *y;
        ev->cpu = *c;
        return 1;
    }
    return 0;
}
