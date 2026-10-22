"""Yu-Gi-Oh! Forbidden Memories (NTSC-U, SLUS-01411) RAM layout and duel logic.

Addresses are PS1 RAM offsets (0x80xxxxxx & 0x1FFFFF). Sources:
  * Data Crystal RAM map  https://datacrystal.tcrf.net/wiki/Yu-Gi-Oh!_Forbidden_Memories/RAM_map
  * Unchiga's FM recompilation notes (rank block, duel records, drop pools)
    https://github.com/Unchiga/YuGiOhForbiddenMemoriesRecomp
"""

from __future__ import annotations

import json
import math
import os
import struct
import time
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# RAM map
# --------------------------------------------------------------------------
DECK = 0x1D0200            # 40 x u16 card id
TRUNK = 0x1D0250           # 722 x u8 copies in the chest
DECK_MIRROR = 0x1D3200     # second copy of the save block
RECORDS = 0x1D071C         # (u16 win, u16 loss) x 40, duelist id 1..39 at +id*4 (Free Duel record)
LAST_DROPS = 0x1D07BC      # 10 x u16, most recent card drops
STARCHIPS = 0x1D07E0       # u32
OPPONENT_ID = 0x09B361     # u8, 1..39
TERRAIN = 0x09B364         # u8
RANK_BLOCK = 0x0E9FF0      # player's duel statistics; opponent at +0x20
RANK_TABLE = 0x1798A8      # 10 rules x 5 x (s16 threshold, s16 change); resident during duels
RANK_RESULT = 0x179A04     # 2 x s32 finished scores, written at duel end
CPU_DECK_POOL = 0x1781D8   # 722 x u16 weights (sum 2048) for the current opponent
CPU_DROP_POOLS = (0x17878C, 0x178D40, 0x1792F4)   # S/A-POW, B/C/D, S/A-TEC

N_CARDS = 722
N_DUELISTS = 39

DUELISTS = [
    "Simon Muran", "Teana", "Jono", "Villager 1", "Villager 2", "Villager 3", "Seto",
    "Heishin", "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi",
    "Yami Bakura", "Pegasus", "Isis", "Kaiba", "Mage Soldier", "Jono 2nd", "Teana 2nd",
    "Ocean Mage", "High Mage Secmeton", "Forest Mage", "High Mage Anubisius",
    "Mountain Mage", "High Mage Atenza", "Desert Mage", "High Mage Martis", "Meadow Mage",
    "High Mage Kepura", "Labyrinth Mage", "Seto 2nd", "Guardian Sebek", "Guardian Neku",
    "Heishin 2nd", "Seto 3rd", "DarkNite", "Nitemare", "Duel Master K",
]
# Rough difficulty 1 (easy) .. 5 (brutal); only used before any real data exists.
DIFFICULTY = [1, 1, 1, 1, 1, 1, 2, 3, 2, 2, 2, 3, 3, 3, 3, 3, 4, 3, 2, 2,
              3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 4]
DIFFICULTY_PRIOR = {1: 0.90, 2: 0.75, 3: 0.60, 4: 0.45, 5: 0.30}

# Rule order in the game's table, with the stat block offset and byte width.
RANK_RULES = [
    # (rule index, name, block offset, width)
    (0, "Turns", 0x01, 1),
    (1, "Effective attacks", 0x02, 1),
    (2, "Defensive wins", 0x03, 1),
    (3, "Face-down plays", 0x04, 1),
    (8, "Fusions", 0x08, 1),
    (9, "Equip magic", 0x09, 1),
    (4, "Pure magic", 0x05, 1),
    (5, "Trigger traps", 0x06, 1),
    (6, "Cards used", 0x18, 1),
    (7, "Remaining LP", 0x14, 2),
]
# Retail table, used only while the game's own copy is not in RAM (outside duels).
# Each row: (threshold, change) -- the first threshold strictly above the value applies.
DEFAULT_RANK_TABLE = {
    0: [(5, 12), (9, 8), (29, 0), (33, -8), (32767, -12)],
    1: [(2, 4), (4, 2), (10, 0), (20, -2), (32767, -4)],
    2: [(2, 0), (6, -10), (10, -20), (15, -30), (32767, -40)],
    3: [(1, 0), (11, -2), (21, -4), (31, -6), (32767, -8)],
    4: [(1, 2), (4, -4), (7, -8), (10, -12), (32767, -16)],
    5: [(1, 2), (3, -8), (5, -16), (7, -24), (32767, -32)],
    6: [(9, 15), (13, 12), (33, 0), (37, -5), (32767, -7)],
    7: [(100, -7), (1000, -5), (7000, 0), (8000, 4), (32767, 6)],
    8: [(1, 4), (5, 0), (10, -4), (15, -8), (32767, -12)],
    9: [(1, 4), (5, 0), (10, -4), (15, -8), (32767, -12)],
}
RANK_BASE = 50
NORMAL_WIN_BONUS = 2   # what a win by reducing LP to 0 adds (Exodia +40, deck-out -40)

DROP_TIER_NAMES = ("S/A-POW", "B/C/D", "S/A-TEC")


def rank_label(score: int):
    """(letter, 'POW'|'TEC', full label) for a 0-99 score."""
    s = max(0, min(99, score))
    if s >= 50:
        letter = "DCBAS"[(s - 50) // 10]
        style = "POW"
    else:
        letter = "DCBAS"[(49 - s) // 10]
        style = "TEC"
    return letter, style, f"{letter}-{style}"


def drop_tier(score: int) -> int:
    letter, style, _ = rank_label(score)
    if letter in "SA":
        return 0 if style == "POW" else 2
    return 1


# --------------------------------------------------------------------------
# Card database
# --------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))


def load_cards():
    with open(os.path.join(HERE, "data", "cards.json"), encoding="utf-8") as fh:
        cards = json.load(fh)
    return {c["id"]: c for c in cards}


CARDS = load_cards()
NON_MONSTER = {"Magic", "Trap", "Equip", "Ritual"}


def is_monster(cid: int) -> bool:
    c = CARDS.get(cid)
    return bool(c) and c["type"] not in NON_MONSTER


# --------------------------------------------------------------------------
# A parsed view of PS1 RAM
# --------------------------------------------------------------------------
def _u16s(ram, off, n):
    return list(struct.unpack_from(f"<{n}H", ram, off))


def _valid_deck(ids):
    return len(ids) == 40 and all(1 <= i <= N_CARDS for i in ids)


def looks_like_fm_save(ram_slice_reader) -> bool:
    """`ram_slice_reader(off, size)` -> bytes|None. True if a save is loaded."""
    a = ram_slice_reader(DECK, 80)
    b = ram_slice_reader(DECK_MIRROR, 80)
    if not a or not b or not (_valid_deck(_u16s(a, 0, 40)) and _valid_deck(_u16s(b, 0, 40))):
        return False
    chips = ram_slice_reader(STARCHIPS, 4)
    opp = ram_slice_reader(OPPONENT_ID, 1)
    return bool(chips and opp) and struct.unpack("<I", chips)[0] <= 999999 and opp[0] <= N_DUELISTS


@dataclass
class SideStats:
    victory: int = 0
    counters: dict = field(default_factory=dict)   # rule index -> value
    lp: int = 0
    start_lp: int = 0


@dataclass
class Snapshot:
    save_loaded: bool
    deck: list
    trunk: list
    starchips: int
    records: dict           # duelist id -> (wins, losses)
    last_drops: list
    opponent: int
    terrain: int
    you: SideStats
    cpu: SideStats
    table: dict             # rule -> [(thr, change)]
    table_live: bool
    result_scores: tuple
    cpu_deck_pool: list
    cpu_drop_pools: list
    t: float

    @property
    def deck_counts(self):
        out = {}
        for cid in self.deck:
            out[cid] = out.get(cid, 0) + 1
        return out


def _read_table(ram):
    table = {}
    for rule in range(10):
        rows = []
        prev = -0x8000
        for i in range(5):
            thr, chg = struct.unpack_from("<hh", ram, RANK_TABLE + rule * 20 + i * 4)
            if thr < prev:
                return None
            prev = thr
            rows.append((thr, chg))
        if prev != 32767:
            return None
        table[rule] = rows
    return table


def _side(ram, base):
    st = SideStats()
    st.victory = struct.unpack_from("<b", ram, base)[0]
    for rule, _name, off, width in RANK_RULES:
        if width == 2:
            st.counters[rule] = struct.unpack_from("<h", ram, base + off)[0]
        else:
            st.counters[rule] = ram[base + off]
    st.lp = struct.unpack_from("<h", ram, base + 0x14)[0]
    st.start_lp = struct.unpack_from("<h", ram, base + 0x16)[0]
    return st


def parse(ram: bytes) -> Snapshot:
    deck = _u16s(ram, DECK, 40)
    mirror = _u16s(ram, DECK_MIRROR, 40)
    loaded = _valid_deck(deck) and _valid_deck(mirror)
    trunk = list(ram[TRUNK:TRUNK + N_CARDS])
    records = {}
    for d in range(1, N_DUELISTS + 1):
        records[d] = struct.unpack_from("<HH", ram, RECORDS + d * 4)
    table = _read_table(ram)
    live = table is not None
    pools = [_u16s(ram, a, N_CARDS) for a in CPU_DROP_POOLS]
    return Snapshot(
        save_loaded=loaded,
        deck=deck if loaded else [],
        trunk=trunk if loaded else [0] * N_CARDS,
        starchips=struct.unpack_from("<I", ram, STARCHIPS)[0] if loaded else 0,
        records=records if loaded else {},
        last_drops=[c for c in _u16s(ram, LAST_DROPS, 10) if 1 <= c <= N_CARDS] if loaded else [],
        opponent=ram[OPPONENT_ID],
        terrain=ram[TERRAIN],
        you=_side(ram, RANK_BLOCK),
        cpu=_side(ram, RANK_BLOCK + 0x20),
        table=table or DEFAULT_RANK_TABLE,
        table_live=live,
        result_scores=struct.unpack_from("<ii", ram, RANK_RESULT),
        cpu_deck_pool=_u16s(ram, CPU_DECK_POOL, N_CARDS),
        cpu_drop_pools=pools,
        t=time.time(),
    )


def pool_valid(pool) -> bool:
    return sum(pool) == 2048


# --------------------------------------------------------------------------
# Rank arithmetic
# --------------------------------------------------------------------------
def rule_change(table, rule, value):
    rows = table[rule]
    for thr, chg in rows:
        if value < thr:
            return chg
    return rows[-1][1]


def next_step(table, rule, value):
    """(value at which the change shifts, new change) or None."""
    rows = table[rule]
    for thr, chg in rows:
        if value < thr:
            if thr >= 32767:
                return None
            return thr, rule_change(table, rule, thr)
    return None


def rank_breakdown(side: SideStats, table, assume_win_bonus=True):
    """(score, rows) where rows = [(name, value, change, next_text)]."""
    victory = side.victory
    if victory == 0 and assume_win_bonus:
        victory = NORMAL_WIN_BONUS
    score = RANK_BASE + victory
    rows = []
    for rule, name, _off, _w in RANK_RULES:
        v = side.counters.get(rule, 0)
        if rule == 7:
            v = max(v, 0)
        chg = rule_change(table, rule, v)
        score += chg
        nxt = next_step(table, rule, v)
        if nxt is None:
            note = ""
        elif rule == 7:
            note = f"at {nxt[0]} LP: {nxt[1]:+d}"
        else:
            more = nxt[0] - v
            note = f"{more} more: {nxt[1]:+d}"
        rows.append((name, v, chg, note))
    return score, rows


# --------------------------------------------------------------------------
# Win chance
# --------------------------------------------------------------------------
def _logit(p):
    p = min(max(p, 0.01), 0.99)
    return math.log(p / (1 - p))


def _sigmoid(x):
    return 1 / (1 + math.exp(-x))


def deck_power(deck_ids):
    """Mean ATK of the five strongest monsters in a deck (a crude strength gauge)."""
    atks = sorted((CARDS[c]["atk"] for c in deck_ids if is_monster(c)), reverse=True)
    top = atks[:5] or [0]
    return sum(top) / len(top)


def pool_power(pool):
    """Expected ATK of the CPU's monster draws, weighted by its deck pool."""
    tot = w_atk = 0
    for idx, w in enumerate(pool):
        if w and is_monster(idx + 1):
            tot += w
            w_atk += w * CARDS[idx + 1]["atk"]
    return w_atk / tot if tot else 0


def win_chance(wins, losses, difficulty, my_power=None, cpu_power=None, live=None):
    """Estimated probability (0..1) of beating a duelist.

    Starts from a difficulty guess, shifts towards your actual record as games
    accumulate (Laplace-smoothed), nudges for deck strength vs the CPU's pool
    when known, and during a duel folds in the life-point situation.
    """
    prior = DIFFICULTY_PRIOR.get(difficulty, 0.6)
    x = _logit(prior)
    if my_power and cpu_power:
        x += (my_power - cpu_power * 1.6) / 900.0
    n = wins + losses
    if n:
        hist = (wins + 1) / (n + 2)
        w = n / (n + 4)
        x = (1 - w) * x + w * _logit(hist)
    if live:
        my_lp, cpu_lp, start = live
        if cpu_lp <= 0 < my_lp:
            return 1.0
        if my_lp <= 0:
            return 0.0
        start = max(start, 1)
        x += 3.2 * (my_lp - cpu_lp) / start
    return _sigmoid(x)


# --------------------------------------------------------------------------
# Duel tracking: detects start/end and records results
# --------------------------------------------------------------------------
class DuelTracker:
    """Watches successive snapshots and emits ('start'|'end', info) events."""

    def __init__(self):
        self.active = False
        self.opponent = 0
        self.zero_lp_since = None
        self.start_records = None
        self.last_turns = 0
        self.first = True

    @staticmethod
    def _fresh(s: Snapshot):
        y, c = s.you, s.cpu
        return (s.table_live and y.start_lp > 0 and y.lp == y.start_lp and c.lp == c.start_lp
                and all(v == 0 for r, v in y.counters.items() if r != 7)
                and y.victory == 0 and c.victory == 0 and 1 <= s.opponent <= N_DUELISTS)

    def update(self, s: Snapshot):
        events = []
        y, c = s.you, s.cpu
        in_progress = (s.table_live and 1 <= s.opponent <= N_DUELISTS and y.start_lp > 0
                       and y.victory == 0 and c.victory == 0)
        if not self.active:
            start = self._fresh(s)
            # attaching mid-duel: accept a running duel that is clearly not finished
            if not start and self.first and in_progress and y.lp > 0 and c.lp > 0:
                start = True
            if start:
                self.active = True
                self.opponent = s.opponent
                self.zero_lp_since = None
                self.start_records = dict(s.records)
                events.append(("start", {"opponent": s.opponent}))
        else:
            if self._fresh(s) and s.opponent != self.opponent:
                self.active = False   # a new duel began before we saw an end
                events.extend(self.update(s))
                self.first = False
                return events
            ended = None
            if y.victory != 0 and c.victory == 0:
                ended = "win"
            elif c.victory != 0 and y.victory == 0:
                ended = "loss"
            elif y.lp <= 0 or c.lp <= 0:
                now = s.t
                self.zero_lp_since = self.zero_lp_since or now
                if now - self.zero_lp_since > 4.0:
                    ended = "loss" if y.lp <= 0 else "win"
            elif not s.table_live:
                # left the duel without a decisive result (reset, state load...)
                self.active = False
                events.append(("abort", {"opponent": self.opponent}))
            if ended:
                self.active = False
                kind = "normal"
                if ended == "win":
                    if y.victory == 40:
                        kind = "exodia"
                    elif y.victory == -40:
                        kind = "deck out"
                events.append(("end", {"opponent": self.opponent, "result": ended, "kind": kind,
                                       "you": y, "cpu": c, "table": s.table}))
        self.first = False
        return events
