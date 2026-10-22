/* final7 - deck-vs-deck duel simulator (see sim.c). */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>

extern const int N_FUSIONS;
extern const uint16_t FUSIONS[][3];   /* low id, high id, result; sorted */
extern const int N_EQUIPS;
extern const uint16_t EQUIPS[][2];    /* monster, equip; sorted */

/* Fraction of `runs` simulated duels that `deck` (40 card ids) wins against a CPU
 * whose 40 cards are drawn from `pool` (722 weights summing to 2048). */
double sim_win_rate(const uint16_t deck[40], const uint16_t pool[722], int runs, uint32_t seed);

int sim_fuse(int a, int b);   /* fusion result id, or 0 */

#endif
