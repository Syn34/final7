"""Background poller: attaches to an emulator and produces Snapshots."""

from __future__ import annotations

import random
import struct
import threading
import time

from . import emulator, game


class LiveSource:
    """Reads PS1 RAM out of a real emulator process."""

    def __init__(self, log):
        self.log = log
        self.proc = None
        self.locator = None
        self.label = ""

    def validate(self, proc, base):
        return game.looks_like_fm_save(lambda off, size: proc.read(base + off, size))

    def attach(self, pid, name, kind, progress=None):
        self.detach()
        proc = emulator.Process(pid, name)
        loc = emulator.RamLocator(proc, kind, log=self.log)
        if not loc.locate(self.validate, progress=progress):
            proc.close()
            raise emulator.AttachError(
                f"Found {name} but not the game's memory. Load your Forbidden Memories save "
                "(get to the main map or deck screen), then attach again.")
        self.proc, self.locator = proc, loc
        self.label = f"{name} (pid {pid}) via {loc.method}"
        self.log(f"Attached: {self.label}")

    def detach(self):
        if self.proc:
            self.proc.close()
        self.proc = self.locator = None

    def read_ram(self):
        if not self.proc:
            return None
        if not self.proc.alive():
            self.log("Emulator closed.")
            self.detach()
            return None
        base = self.locator.resolve()
        if not base:
            return None
        return self.proc.read(base, emulator.PS1_RAM_SIZE)


class DemoSource:
    """A fake emulator that plays an endless series of scripted duels.

    Lets you try the tracker (and lets us test it) without a game running.
    """

    label = "Demo mode (simulated game)"

    def __init__(self):
        self.ram = bytearray(emulator.PS1_RAM_SIZE)
        rnd = random.Random(7)
        deck = [rnd.choice([1, 2, 5, 9, 14, 20, 23, 64, 113, 302, 330, 337, 347, 350, 400, 410, 412])
                for _ in range(40)]
        for i, cid in enumerate(deck):
            struct.pack_into("<H", self.ram, game.DECK + 2 * i, cid)
            struct.pack_into("<H", self.ram, game.DECK_MIRROR + 2 * i, cid)
        for cid in range(1, 400):
            self.ram[game.TRUNK + cid - 1] = rnd.choice([0, 0, 1, 2, 3, 5])
        struct.pack_into("<I", self.ram, game.STARCHIPS, 1234)
        for d in range(1, 40):
            struct.pack_into("<HH", self.ram, game.RECORDS + d * 4, rnd.randint(0, 12), rnd.randint(0, 6))
        # rank table
        for rule, rows in game.DEFAULT_RANK_TABLE.items():
            for i, (thr, chg) in enumerate(rows):
                struct.pack_into("<hh", self.ram, game.RANK_TABLE + rule * 20 + i * 4, thr, chg)
        self.rnd = rnd
        self.duel_t0 = None
        self.opponent = 1
        self.step = 0
        self.next_tick = 0

    def _pool(self, addr, cards):
        for i in range(game.N_CARDS):
            struct.pack_into("<H", self.ram, addr + 2 * i, 0)
        weights = [2048 // len(cards)] * len(cards)
        weights[0] += 2048 - sum(weights)
        for c, w in zip(cards, weights):
            struct.pack_into("<H", self.ram, addr + 2 * (c - 1), w)

    def _start_duel(self):
        self.opponent = self.rnd.randint(1, 39)
        self.ram[game.OPPONENT_ID] = self.opponent
        for side in (0, 0x20):
            b = game.RANK_BLOCK + side
            self.ram[b:b + 0x20] = bytes(0x20)
            struct.pack_into("<hh", self.ram, b + 0x14, 8000, 8000)
        pool = self.rnd.sample(range(1, 700), 30)
        self._pool(game.CPU_DECK_POOL, pool)
        for addr in game.CPU_DROP_POOLS:
            self._pool(addr, self.rnd.sample(range(1, 722), 25))
        self.step = 0

    def _tick_duel(self):
        b, c = game.RANK_BLOCK, game.RANK_BLOCK + 0x20
        r = self.rnd
        self.step += 1
        self.ram[b + 0x01] = min(255, self.step // 2 + 1)
        self.ram[c + 0x01] = min(255, self.step // 2)
        for off in (0x02, 0x04, 0x08, 0x09, 0x05, 0x18):
            if r.random() < 0.25:
                self.ram[b + off] = min(255, self.ram[b + off] + 1)
        lp_y = struct.unpack_from("<h", self.ram, b + 0x14)[0]
        lp_c = struct.unpack_from("<h", self.ram, c + 0x14)[0]
        if r.random() < 0.5:
            lp_c = max(0, lp_c - r.choice([300, 500, 800, 1200]))
        else:
            lp_y = max(0, lp_y - r.choice([200, 400, 700]))
        struct.pack_into("<h", self.ram, b + 0x14, lp_y)
        struct.pack_into("<h", self.ram, c + 0x14, lp_c)
        if lp_c == 0 or lp_y == 0:
            winner = b if lp_c == 0 else c
            self.ram[winner] = 2
            if winner == b:
                drop = r.randint(1, 400)
                self.ram[game.TRUNK + drop - 1] = min(250, self.ram[game.TRUNK + drop - 1] + 1)
                drops = list(struct.unpack_from("<10H", self.ram, game.LAST_DROPS))
                drops = [drop] + drops[:9]
                struct.pack_into("<10H", self.ram, game.LAST_DROPS, *drops)
            return True
        return False

    def read_ram(self):
        now = time.time()
        if now >= self.next_tick:
            if self.duel_t0 is None:
                self._start_duel()
                self.duel_t0 = now
                self.next_tick = now + 1.5
            elif self.duel_t0 == -1:
                self.duel_t0 = None
                self.next_tick = now + 1.0
            else:
                done = self._tick_duel()
                self.next_tick = now + (6.0 if done else 0.9)
                if done:
                    self.duel_t0 = -1
        return bytes(self.ram)

    def detach(self):
        pass


class Poller(threading.Thread):
    """Calls `on_snapshot(snapshot|None, status_text)` from a background thread."""

    def __init__(self, source, on_snapshot, interval=0.25):
        super().__init__(daemon=True)
        self.source = source
        self.on_snapshot = on_snapshot
        self.interval = interval
        self._halt = threading.Event()

    def run(self):
        while not self._halt.is_set():
            try:
                ram = self.source.read_ram()
                if ram is None:
                    self.on_snapshot(None, "Lost the emulator's memory.")
                else:
                    self.on_snapshot(game.parse(ram), self.source.label)
            except Exception as exc:  # keep polling through transient read errors
                self.on_snapshot(None, f"Read error: {exc}")
            self._halt.wait(self.interval)

    def stop(self):
        self._halt.set()
