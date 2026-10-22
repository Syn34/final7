/* final7 - deck-vs-deck duel simulator.
 *
 * Plays many quick duels between your current 40-card deck and a CPU deck drawn
 * from a duelist's deck pool (the game builds the CPU's 40 cards from those
 * weights), and reports how often you win. It is a simplified Forbidden
 * Memories duel, not an emulation:
 *
 *   - both sides hold 5 cards and draw back up each turn; an empty deck loses
 *   - one play per turn: the strongest of a single monster, a fusion of two
 *     hand cards, or a fusion chain of three (real fusion results), plus the
 *     best compatible equip in hand (+500, Megamorph +1000); or an equip on the
 *     monster already on the field; or Raigeki / Dark Hole to clear a stronger
 *     enemy monster
 *   - one monster each on the field, always in attack position; a stronger
 *     monster destroys the weaker and deals the difference, equal ones trade,
 *     an empty field takes a direct hit
 *
 * Guardian stars, field bonuses, traps, defense position and the CPU's AI
 * quirks are left out, so treat the result as a deck-strength comparison.
 */
#include <string.h>

#include "fm.h"
#include "sim.h"

#define HAND 5
#define START_LP 8000
#define CARD_RAIGEKI 337
#define CARD_DARKHOLE 336
#define CARD_MEGAMORPH 657

static uint32_t rng_state;
static uint32_t rng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static int atk_of(int id) { return card_is_monster(id) ? CARDS[id - 1].atk : -1; }
static int is_equip(int id) { return id >= 1 && id <= N_CARDS && !strcmp(CARDS[id - 1].type, "Equip"); }

int sim_fuse(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    int lo = 0, hi = N_FUSIONS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = FUSIONS[mid][0] != a ? FUSIONS[mid][0] - a : FUSIONS[mid][1] - b;
        if (!c) return FUSIONS[mid][2];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int equips_on(int monster, int equip)
{
    int lo = 0, hi = N_EQUIPS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = EQUIPS[mid][0] != monster ? EQUIPS[mid][0] - monster : EQUIPS[mid][1] - equip;
        if (!c) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int equip_bonus(int equip) { return equip == CARD_MEGAMORPH ? 1000 : 500; }

typedef struct {
    uint16_t deck[40];
    int top;            /* next card to draw */
    uint16_t hand[HAND];
    int nhand;
    int lp;
    int field_id;       /* card on the field (0 = none) */
    int field_atk;      /* its attack including equips */
} Player;

static void shuffle(uint16_t *d, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rng() % (uint32_t)(i + 1));
        uint16_t t = d[i]; d[i] = d[j]; d[j] = t;
    }
}

static int draw_up(Player *s)
{
    while (s->nhand < HAND) {
        if (s->top >= 40) return s->nhand > 0;   /* out of cards: lose once the hand is empty */
        s->hand[s->nhand++] = s->deck[s->top++];
    }
    return 1;
}

static void remove_cards(Player *s, const int *idx, int n)
{
    /* remove hand indices (any order) */
    int mark[HAND] = {0};
    for (int k = 0; k < n; k++) mark[idx[k]] = 1;
    int w = 0;
    for (int i = 0; i < s->nhand; i++) if (!mark[i]) s->hand[w++] = s->hand[i];
    s->nhand = w;
}

/* best equip in hand (not in `used`) for monster `m`; returns hand index or -1 */
static int best_equip(const Player *s, int m, const int *used, int nused, int *bonus)
{
    int best = -1;
    *bonus = 0;
    for (int i = 0; i < s->nhand; i++) {
        int skip = 0;
        for (int k = 0; k < nused; k++) if (used[k] == i) skip = 1;
        if (skip || !is_equip(s->hand[i]) || !equips_on(m, s->hand[i])) continue;
        int b = equip_bonus(s->hand[i]);
        if (b > *bonus) { *bonus = b; best = i; }
    }
    return best;
}

static void take_turn(Player *me, Player *opp)
{
    /* find the strongest play */
    int best_val = -1, best_id = 0, best_used[4], best_n = 0;
    for (int i = 0; i < me->nhand; i++) {
        int a = me->hand[i];
        if (atk_of(a) < 0) continue;
        int used[4] = { i }, n = 1, eb, ei = best_equip(me, a, used, n, &eb);
        int v = atk_of(a) + eb;
        if (v > best_val) { best_val = v; best_id = a; best_n = 0; best_used[best_n++] = i; if (ei >= 0) best_used[best_n++] = ei; }
        for (int j = 0; j < me->nhand; j++) {
            if (j == i) continue;
            int r = sim_fuse(a, me->hand[j]);
            if (!r || atk_of(r) < 0) continue;
            int u2[4] = { i, j };
            ei = best_equip(me, r, u2, 2, &eb);
            v = atk_of(r) + eb;
            if (v > best_val) { best_val = v; best_id = r; best_n = 0; best_used[best_n++] = i; best_used[best_n++] = j; if (ei >= 0) best_used[best_n++] = ei; }
            for (int k = 0; k < me->nhand; k++) {
                if (k == i || k == j) continue;
                int r2 = sim_fuse(r, me->hand[k]);
                if (!r2 || atk_of(r2) < 0) continue;
                int u3[4] = { i, j, k };
                ei = best_equip(me, r2, u3, 3, &eb);
                v = atk_of(r2) + eb;
                if (v > best_val) {
                    best_val = v; best_id = r2; best_n = 0;
                    best_used[best_n++] = i; best_used[best_n++] = j; best_used[best_n++] = k;
                    if (ei >= 0) best_used[best_n++] = ei;
                }
            }
        }
    }
    /* equip the monster already out */
    int eq_val = -1, eq_idx = -1;
    if (me->field_id) {
        int eb, ei = best_equip(me, me->field_id, NULL, 0, &eb);
        if (ei >= 0) { eq_val = me->field_atk + eb; eq_idx = ei; }
    }
    /* field-clearing magic */
    int clear_idx = -1;
    for (int i = 0; i < me->nhand; i++)
        if (me->hand[i] == CARD_RAIGEKI || me->hand[i] == CARD_DARKHOLE) clear_idx = i;

    int mine = me->field_id ? me->field_atk : -1;
    int top = best_val > eq_val ? best_val : eq_val;
    if (clear_idx >= 0 && opp->field_id && opp->field_atk >= (top > mine ? top : mine)) {
        if (me->hand[clear_idx] == CARD_DARKHOLE) { me->field_id = 0; me->field_atk = 0; }
        opp->field_id = 0; opp->field_atk = 0;
        remove_cards(me, &clear_idx, 1);
    } else if (best_val > mine && best_val >= eq_val) {
        me->field_id = best_id; me->field_atk = best_val;
        remove_cards(me, best_used, best_n);
    } else if (eq_val > mine) {
        me->field_atk = eq_val;
        remove_cards(me, &eq_idx, 1);
    } else if (me->nhand == HAND) {
        /* nothing better: throw away the weakest card so the hand keeps moving */
        int w = 0;
        for (int i = 1; i < me->nhand; i++) if (atk_of(me->hand[i]) < atk_of(me->hand[w])) w = i;
        remove_cards(me, &w, 1);
    }
    /* battle */
    if (me->field_id) {
        if (!opp->field_id) opp->lp -= me->field_atk;
        else if (me->field_atk > opp->field_atk) { opp->lp -= me->field_atk - opp->field_atk; opp->field_id = 0; opp->field_atk = 0; }
        else if (me->field_atk == opp->field_atk) { opp->field_id = me->field_id = 0; opp->field_atk = me->field_atk = 0; }
    }
}

static void build_cpu_deck(const uint16_t pool[722], uint16_t out[40])
{
    for (int k = 0; k < 40; k++) {
        uint32_t r = rng() % 2048u, acc = 0;
        int pick = 1;
        for (int i = 0; i < N_CARDS; i++) {
            acc += pool[i];
            if (r < acc) { pick = i + 1; break; }
        }
        out[k] = (uint16_t)pick;
    }
}

double sim_win_rate(const uint16_t deck[40], const uint16_t pool[722], int runs, uint32_t seed)
{
    rng_state = seed ? seed : 0x1234567u;
    int wins = 0;
    for (int r = 0; r < runs; r++) {
        Player a, b;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        memcpy(a.deck, deck, sizeof a.deck);
        build_cpu_deck(pool, b.deck);
        shuffle(a.deck, 40);
        shuffle(b.deck, 40);
        a.lp = b.lp = START_LP;
        int you_first = r & 1, result = 0;
        for (int turn = 0; turn < 200 && !result; turn++) {
            Player *me = ((turn & 1) == 0) == you_first ? &a : &b;
            Player *opp = me == &a ? &b : &a;
            if (!draw_up(me)) { result = me == &a ? -1 : 1; break; }
            take_turn(me, opp);
            if (opp->lp <= 0) result = me == &a ? 1 : -1;
        }
        if (result > 0) wins++;
    }
    return runs ? (double)wins / runs : 0;
}
