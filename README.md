# FM Tracker

A live companion window for **Yu-Gi-Oh! Forbidden Memories** (PS1, North American version) running in an emulator on Windows. It reads the game's memory while you play. It never writes to the game, so your save and the emulator are not touched.

![Duel tab](docs/duel-tab.png)

## What it does

| Tab | What you get |
|---|---|
| **Duel & Rank** | Your duel rank updates live as you play: the letter (S/A/B/C/D), **POW or TEC**, and the 0–99 score. A breakdown shows every stat (turns, effective attacks, face-downs, fusions, equips, magic, traps, cards used, LP) with its points and how far you are from the next change ("2 more fusions: −4"). Also shows both players' life points, your **chance to win** that duel, and the opponent's drop table for the rank you're on track for. |
| **Card Overview** | Pick the cards you want to track. Their deck, trunk and total counts update by themselves whenever a card goes into or out of your deck or chest (drops, password buys, deck edits). Changes flash green or red. Buttons let you track your whole deck or the Exodia pieces in one click. |
| **Duelists** | All 39 opponents with the game's own Free Duel win/loss record, the duels the tracker has watched (story duels included), win rate, an estimated **chance to beat** each one, and your best rank against them. The opponent you're fighting is highlighted. After you've duelled someone once, their deck and drop tables are saved and can be browsed here. |
| **Passwords** | All 722 cards with their 8-digit password, starchip cost, type, attribute, level and ATK/DEF, plus how many you own. Search, filter to cards you don't own or can afford now, and double-click a row to copy the password. |
| **Overlay** | A small always-on-top window with just your rank and win chance, for keeping next to the emulator. |

## Running it

**Option A: Python (works right away)**
1. Install Python 3.8 or newer from [python.org](https://www.python.org/downloads/). Tick *"Add python.exe to PATH"*. No other packages are needed.
2. Double-click **`Start FM Tracker.bat`**, or `FMTracker.pyw`.

**Option B: standalone .exe.** Every push to `main` builds `FMTracker.exe` with GitHub Actions (look under *Actions → Build Windows exe → Artifacts*). Pushing a `v*` tag attaches the exe to a Release.

Want to look around without the game? Run **`Start FM Tracker (Demo).bat`**. It simulates a game playing endless duels.

### Attaching
1. Start your emulator and load Forbidden Memories.
2. Load your save and get to the main map or deck screen.
3. In FM Tracker, pick the emulator from the dropdown (↻ refreshes the list) and press **Attach**. If an emulator is already running when the tracker starts, it attaches by itself.

If the emulator runs as administrator, run the tracker as administrator too.

### Supported emulators
| Emulator | How the PS1 memory is found |
|---|---|
| DuckStation | its exported `RAM` symbol (signature fallback) |
| ePSXe | signature |
| RetroArch: SwanStation, Beetle PSX / PSX HW, PCSX ReARMed | signature in the core |
| Mednafen, pSX, PCSX-Redux (64-bit), Xebra | signature |
| BizHawk and anything else | full memory scan for your loaded save |

If a method fails (for example after an emulator update), the tracker falls back to scanning memory for your save. That scan takes a few seconds.

## How the numbers work

* **Rank.** The game starts every duel at 50 points. Each stat adds or removes points through a table that the game loads during duels, and the tracker reads that same table live. 0–49 is TEC and 50–99 is POW, so a lower score means a better TEC rank. The *"Include +2 win bonus"* box adds what a normal win by LP is worth (an Exodia win is +40, a deck-out win is −40). With the box ticked, the badge shows the rank you'd get if you won right now.
* **Chance to win / beat.** This is an estimate, not a game value. It starts from a rough difficulty guess for each duelist, moves toward your real record as you play more, and adjusts for the ATK of your five strongest deck monsters against the CPU's deck pool (once the tracker has seen it). During a duel it also follows the life-point gap.
* **Drops.** These are the opponent's real drop weights out of 2048 for the S/A-POW, B/C/D or S/A-TEC table, matching your current rank.

Your tracked-card list, duel history and learned duelist tables are saved in a `tracker_data` folder next to the program.

## Project layout
```
FMTracker.pyw          entry point
fmtracker/emulator.py  read-only process access + PS1 RAM location per emulator
fmtracker/game.py      FM memory map, rank math, duel detection, win estimate
fmtracker/backend.py   polling thread + demo simulator
fmtracker/app.py       the Tkinter interface
fmtracker/data/        card database (names, passwords, costs, stats)
tests/                 python -m unittest discover tests
```

## Credits
* Memory addresses: [Data Crystal RAM map](https://datacrystal.tcrf.net/wiki/Yu-Gi-Oh!_Forbidden_Memories/RAM_map) and the research notes in [Unchiga/YuGiOhForbiddenMemoriesRecomp](https://github.com/Unchiga/YuGiOhForbiddenMemoriesRecomp) (rank statistics block, duel records, CPU drop pools).
* Emulator RAM pointer signatures: adapted from [Jujstme/emu-help](https://github.com/Jujstme/emu-help) (MIT).
* Card data and passwords: exported from the catalog in [vincevandriel/yfm-fusion-companion](https://github.com/vincevandriel/yfm-fusion-companion) (MIT), which builds on [MarceloSilvarolla/YFM-Database-and-Fusion-Guide](https://github.com/MarceloSilvarolla/YFM-Database-and-Fusion-Guide).

Yu-Gi-Oh! is a trademark of Konami. This is an unofficial fan tool and doesn't include any game files.
