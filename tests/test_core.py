"""Run with:  python -m unittest discover tests"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fmtracker import emulator, game  # noqa: E402
from fmtracker.backend import DemoSource  # noqa: E402


class FakeProc:
    """Pretends to be an emulator process whose PS1 RAM sits at `base` inside a big blob."""

    def __init__(self, ram, base, pad=0x300000):
        self.mem_base = 0x10000000
        rnd = __import__("random").Random(1)
        noise = bytearray(rnd.getrandbits(8) for _ in range(pad))
        self.blob = noise + bytearray(ram) + noise
        self.ram_at = self.mem_base + pad
        self.is64 = True

    def regions(self, start=0, end=None):
        yield self.mem_base, len(self.blob), emulator.MEM_COMMIT, 0x04, emulator.MEM_PRIVATE

    def read(self, addr, size):
        off = addr - self.mem_base
        if off < 0 or off + size > len(self.blob):
            return None
        return bytes(self.blob[off:off + size])


class CoreTests(unittest.TestCase):
    def setUp(self):
        self.ram = DemoSource().ram

    def test_parse_save(self):
        s = game.parse(bytes(self.ram))
        self.assertTrue(s.save_loaded)
        self.assertEqual(len(s.deck), 40)
        self.assertEqual(s.starchips, 1234)

    def test_no_save(self):
        s = game.parse(bytes(emulator.PS1_RAM_SIZE))
        self.assertFalse(s.save_loaded)

    def test_generic_scan_finds_ram(self):
        proc = FakeProc(self.ram, 0)
        validate = lambda p, b: game.looks_like_fm_save(lambda o, n: p.read(b + o, n))  # noqa: E731
        self.assertEqual(emulator.generic_scan(proc, validate), proc.ram_at)

    def test_rank_bands(self):
        self.assertEqual(game.rank_label(95)[2], "S-POW")
        self.assertEqual(game.rank_label(5)[2], "S-TEC")
        self.assertEqual(game.rank_label(50)[2], "D-POW")
        self.assertEqual(game.rank_label(49)[2], "D-TEC")
        self.assertEqual(game.drop_tier(85), 0)
        self.assertEqual(game.drop_tier(15), 2)
        self.assertEqual(game.drop_tier(55), 1)

    def test_fast_win_scores_high(self):
        side = game.SideStats(victory=0, counters={0: 3, 1: 1, 2: 0, 3: 0, 4: 0, 5: 0, 6: 5, 7: 8000, 8: 0, 9: 0},
                              lp=8000, start_lp=8000)
        score, _ = game.rank_breakdown(side, game.DEFAULT_RANK_TABLE)
        self.assertGreaterEqual(score, 90)   # quick clean win -> S-POW

    def test_sig_compile(self):
        pat = emulator.Process.compile_sig("48 89 0D ?? ?? ?? ?? B8")
        self.assertIsNotNone(pat.search(b"\x00\x48\x89\x0d\x01\x02\x03\x04\xb8"))

    def test_passwords_present(self):
        self.assertEqual(game.CARDS[1]["password"], "89631139")
        self.assertGreater(sum(1 for c in game.CARDS.values() if c["password"]), 690)


if __name__ == "__main__":
    unittest.main()
