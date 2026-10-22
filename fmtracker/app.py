"""FM Tracker - live companion for Yu-Gi-Oh! Forbidden Memories on PS1 emulators."""

from __future__ import annotations

import json
import os
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

from . import emulator, game
from .backend import DemoSource, LiveSource, Poller

APP_NAME = "FM Tracker"
if getattr(sys, "frozen", False):          # packaged .exe: keep data beside the exe
    _APP_DIR = os.path.dirname(sys.executable)
else:
    _APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(_APP_DIR, "tracker_data")

# palette
BG = "#0e1320"
PANEL = "#161d2e"
PANEL2 = "#1d263b"
LINE = "#2a3550"
TEXT = "#e9e4d4"
MUTED = "#8f98ad"
GOLD = "#d9ad4f"
GREEN = "#5fcf86"
RED = "#ef6b6b"
BLUE = "#6fa8ff"
POW_COLOR = "#ff9a52"
TEC_COLOR = "#7fc8ff"

UI_FONT = "Segoe UI" if sys.platform == "win32" else "DejaVu Sans"
MONO_FONT = "Consolas" if sys.platform == "win32" else "DejaVu Sans Mono"

LETTER_COLORS = {"S": "#ffd85a", "A": "#ff9f43", "B": "#7bd389", "C": "#74b9ff", "D": "#a4a9b8"}


# --------------------------------------------------------------------------
# persistence
# --------------------------------------------------------------------------
def _load_json(name, default):
    try:
        with open(os.path.join(DATA_DIR, name), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default


def _save_json(name, data):
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        tmp = os.path.join(DATA_DIR, name + ".tmp")
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=1)
        os.replace(tmp, os.path.join(DATA_DIR, name))
    except OSError:
        pass


def card_name(cid):
    c = game.CARDS.get(cid)
    return c["name"] if c else f"#{cid}"


# --------------------------------------------------------------------------
# small widgets
# --------------------------------------------------------------------------
class SortableTree(ttk.Treeview):
    """Treeview whose column headers sort on click."""

    def __init__(self, master, columns, widths, anchors=None, **kw):
        super().__init__(master, columns=[c for c in columns], show="headings", **kw)
        anchors = anchors or {}
        self._sort_state = {}
        for col, w in zip(columns, widths):
            self.heading(col, text=col, command=lambda c=col: self.sort_by(c))
            self.column(col, width=w, minwidth=30, anchor=anchors.get(col, "w"), stretch=(w >= 160))

    def sort_by(self, col):
        rev = self._sort_state.get(col, False)
        rows = [(self.set(k, col), k) for k in self.get_children("")]

        def key(v):
            s = v[0].replace("%", "").replace(",", "").replace("+", "").strip()
            try:
                return (0, float(s))
            except ValueError:
                return (1, s.lower())
        rows.sort(key=key, reverse=rev)
        for i, (_, k) in enumerate(rows):
            self.move(k, "", i)
        self._sort_state[col] = not rev


def scrolled(parent, tree_factory):
    frame = ttk.Frame(parent, style="Panel.TFrame")
    tree = tree_factory(frame)
    sb = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=sb.set)
    tree.grid(row=0, column=0, sticky="nsew")
    sb.grid(row=0, column=1, sticky="ns")
    frame.rowconfigure(0, weight=1)
    frame.columnconfigure(0, weight=1)
    return frame, tree


# --------------------------------------------------------------------------
# main window
# --------------------------------------------------------------------------
class TrackerApp:
    def __init__(self, root: tk.Tk, demo=False):
        self.root = root
        self.settings = _load_json("settings.json", {})
        self.history = _load_json("history.json", [])
        self.pool_cache = _load_json("cpu_pools.json", {})   # duelist id -> {deck, drops:[...]}
        self.tracked = [c for c in self.settings.get("tracked", [1, 17, 18, 19, 20, 21]) if c in game.CARDS]

        self.source = None
        self.poller = None
        self.latest = None
        self.latest_status = "Not attached"
        self.lock = threading.Lock()
        self.tracker = game.DuelTracker()
        self.prev_totals = {}
        self.changes = {}            # cid -> (delta, timestamp)
        self.pending_end = None      # history entry still collecting drop / record info
        self.attaching = False
        self.emulators = []
        self.mini = None

        self._setup_style()
        self._build()
        self.refresh_emulators()
        self.root.after(250, self._tick)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        if demo:
            self.start_demo()
        elif self.settings.get("auto_attach", True):
            self.root.after(600, self.auto_attach)

    # ------------------------------------------------------------------ style
    def _setup_style(self):
        r = self.root
        r.title(APP_NAME)
        r.configure(bg=BG)
        r.geometry(self.settings.get("geometry", "1240x800"))
        r.minsize(1000, 640)
        st = ttk.Style(r)
        st.theme_use("clam")
        st.configure(".", background=BG, foreground=TEXT, fieldbackground=PANEL2, bordercolor=LINE,
                     font=(UI_FONT, 10), lightcolor=PANEL, darkcolor=PANEL, troughcolor=PANEL)
        st.configure("TFrame", background=BG)
        st.configure("Panel.TFrame", background=PANEL)
        st.configure("TLabel", background=BG, foreground=TEXT)
        st.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        st.configure("Muted.TLabel", background=PANEL, foreground=MUTED, font=(UI_FONT, 9))
        st.configure("Head.TLabel", background=PANEL, foreground=GOLD, font=(UI_FONT, 11, "bold"))
        st.configure("Title.TLabel", background=BG, foreground=GOLD, font=(UI_FONT, 16, "bold"))
        st.configure("Stat.TLabel", background=BG, foreground=TEXT, font=(UI_FONT, 10))
        st.configure("TButton", background=PANEL2, foreground=TEXT, padding=(8, 4), borderwidth=1,
                     focusthickness=0)
        st.map("TButton", background=[("active", LINE), ("disabled", PANEL)],
               foreground=[("disabled", MUTED)])
        st.configure("Gold.TButton", background=GOLD, foreground="#1b1406", font=(UI_FONT, 10, "bold"))
        st.map("Gold.TButton", background=[("active", "#f0c76a")])
        st.configure("TNotebook", background=BG, borderwidth=0, tabmargins=(8, 6, 8, 0))
        st.configure("TNotebook.Tab", background=PANEL, foreground=MUTED, padding=(16, 7),
                     font=(UI_FONT, 10, "bold"), borderwidth=0)
        st.map("TNotebook.Tab", background=[("selected", PANEL2)], foreground=[("selected", GOLD)])
        st.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=TEXT, rowheight=24,
                     borderwidth=0, font=(UI_FONT, 10))
        st.map("Treeview", background=[("selected", "#33406a")], foreground=[("selected", "#ffffff")])
        st.configure("Treeview.Heading", background=PANEL2, foreground=GOLD, relief="flat",
                     font=(UI_FONT, 9, "bold"), padding=(6, 4))
        st.map("Treeview.Heading", background=[("active", LINE)])
        st.configure("TEntry", fieldbackground=PANEL2, foreground=TEXT, insertcolor=TEXT, padding=5)
        st.configure("TCombobox", fieldbackground=PANEL2, background=PANEL2, foreground=TEXT,
                     arrowcolor=GOLD, padding=4)
        st.map("TCombobox", fieldbackground=[("readonly", PANEL2)], foreground=[("readonly", TEXT)])
        r.option_add("*TCombobox*Listbox.background", PANEL2)
        r.option_add("*TCombobox*Listbox.foreground", TEXT)
        st.configure("TCheckbutton", background=PANEL, foreground=TEXT)
        st.map("TCheckbutton", background=[("active", PANEL)])
        st.configure("Vertical.TScrollbar", background=PANEL2, troughcolor=PANEL, arrowcolor=MUTED,
                     bordercolor=PANEL)
        st.configure("Horizontal.TProgressbar", background=GOLD, troughcolor=PANEL2)

    # ------------------------------------------------------------------ layout
    def _build(self):
        top = ttk.Frame(self.root, padding=(14, 10, 14, 4))
        top.pack(fill="x")
        ttk.Label(top, text="⟁ FM Tracker", style="Title.TLabel").pack(side="left")
        self.status_dot = tk.Canvas(top, width=14, height=14, bg=BG, highlightthickness=0)
        self.status_dot.pack(side="left", padx=(16, 6))
        self.status_var = tk.StringVar(value="Not attached")
        ttk.Label(top, textvariable=self.status_var, style="Stat.TLabel").pack(side="left")

        right = ttk.Frame(top)
        right.pack(side="right")
        self.emu_var = tk.StringVar()
        self.emu_box = ttk.Combobox(right, textvariable=self.emu_var, width=26, state="readonly")
        self.emu_box.pack(side="left", padx=4)
        ttk.Button(right, text="↻", width=3, command=self.refresh_emulators).pack(side="left", padx=2)
        self.attach_btn = ttk.Button(right, text="Attach", style="Gold.TButton", command=self.attach_selected)
        self.attach_btn.pack(side="left", padx=4)
        ttk.Button(right, text="Detach", command=self.detach).pack(side="left", padx=2)
        ttk.Button(right, text="Demo", command=self.start_demo).pack(side="left", padx=2)
        ttk.Button(right, text="Overlay", command=self.toggle_mini).pack(side="left", padx=(8, 0))

        stats = ttk.Frame(self.root, padding=(14, 0, 14, 6))
        stats.pack(fill="x")
        self.stat_vars = {}
        for key in ("Starchips", "Deck", "Trunk cards", "Unique owned", "Opponent"):
            v = tk.StringVar(value="—")
            self.stat_vars[key] = v
            f = ttk.Frame(stats)
            f.pack(side="left", padx=(0, 22))
            ttk.Label(f, text=key.upper(), foreground=MUTED, font=(UI_FONT, 8, "bold")).pack(anchor="w")
            ttk.Label(f, textvariable=v, font=(UI_FONT, 12, "bold")).pack(anchor="w")

        self.nb = ttk.Notebook(self.root)
        self.nb.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self._build_duel_tab()
        self._build_cards_tab()
        self._build_duelists_tab()
        self._build_passwords_tab()
        self._build_log_tab()

    # ---- duel tab
    def _build_duel_tab(self):
        tab = ttk.Frame(self.nb, style="Panel.TFrame", padding=12)
        self.nb.add(tab, text="  Duel & Rank  ")
        tab.columnconfigure(0, weight=0)
        tab.columnconfigure(1, weight=1)
        tab.rowconfigure(1, weight=1)

        # big rank badge
        badge = tk.Frame(tab, bg=PANEL2, highlightbackground=LINE, highlightthickness=1)
        badge.grid(row=0, column=0, rowspan=2, sticky="nsw", padx=(0, 12))
        tk.Label(badge, text="LIVE DUEL RANK", bg=PANEL2, fg=MUTED, font=(UI_FONT, 9, "bold")).pack(pady=(14, 0))
        self.rank_letter = tk.Label(badge, text="–", bg=PANEL2, fg=MUTED, font=(UI_FONT, 84, "bold"), width=2)
        self.rank_letter.pack(padx=30)
        self.rank_style = tk.Label(badge, text="", bg=PANEL2, fg=MUTED, font=(UI_FONT, 22, "bold"))
        self.rank_style.pack()
        self.rank_score = tk.Label(badge, text="score —", bg=PANEL2, fg=TEXT, font=(UI_FONT, 12))
        self.rank_score.pack(pady=(4, 0))
        self.rank_bar = tk.Canvas(badge, width=230, height=26, bg=PANEL2, highlightthickness=0)
        self.rank_bar.pack(pady=(10, 2), padx=14)
        tk.Label(badge, text="TEC ◂  0 ─────── 50 ─────── 99  ▸ POW", bg=PANEL2, fg=MUTED,
                 font=(UI_FONT, 8)).pack()
        self.assume_win = tk.BooleanVar(value=self.settings.get("assume_win", True))
        tk.Checkbutton(badge, text="Include +2 win bonus", variable=self.assume_win, bg=PANEL2, fg=MUTED,
                       selectcolor=PANEL, activebackground=PANEL2, activeforeground=TEXT,
                       font=(UI_FONT, 9)).pack(pady=(8, 0))
        sep = tk.Frame(badge, bg=LINE, height=1)
        sep.pack(fill="x", padx=14, pady=12)
        tk.Label(badge, text="CHANCE TO WIN", bg=PANEL2, fg=MUTED, font=(UI_FONT, 9, "bold")).pack()
        self.win_pct = tk.Label(badge, text="—", bg=PANEL2, fg=GOLD, font=(UI_FONT, 34, "bold"))
        self.win_pct.pack()
        self.win_note = tk.Label(badge, text="", bg=PANEL2, fg=MUTED, font=(UI_FONT, 9), wraplength=230,
                                 justify="center")
        self.win_note.pack(padx=10, pady=(0, 12))

        # header with LP
        head = ttk.Frame(tab, style="Panel.TFrame")
        head.grid(row=0, column=1, sticky="ew")
        self.duel_title = ttk.Label(head, text="No duel in progress", style="Head.TLabel",
                                    font=(UI_FONT, 15, "bold"))
        self.duel_title.pack(anchor="w")
        lp = tk.Frame(head, bg=PANEL)
        lp.pack(fill="x", pady=(8, 10))
        self.lp_you = self._lp_meter(lp, "YOU", GREEN)
        self.lp_cpu = self._lp_meter(lp, "OPPONENT", RED)

        body = ttk.Frame(tab, style="Panel.TFrame")
        body.grid(row=1, column=1, sticky="nsew")
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        body.rowconfigure(1, weight=1)
        ttk.Label(body, text="Rank breakdown", style="Head.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(body, text="Possible drops at this rank", style="Head.TLabel").grid(row=0, column=1, sticky="w",
                                                                                     padx=(12, 0))
        f, self.rank_tree = scrolled(body, lambda m: SortableTree(
            m, ("Stat", "Value", "Points", "Next change"), (150, 70, 70, 150),
            {"Value": "center", "Points": "center"}, height=10))
        f.grid(row=1, column=0, sticky="nsew", pady=(6, 0))
        self.rank_tree.tag_configure("pos", foreground=GREEN)
        self.rank_tree.tag_configure("neg", foreground=RED)
        self.rank_tree.tag_configure("zero", foreground=TEXT)
        f2, self.drop_tree = scrolled(body, lambda m: SortableTree(
            m, ("Card", "Chance", "Owned"), (165, 64, 64), {"Chance": "center", "Owned": "center"}))
        f2.grid(row=1, column=1, sticky="nsew", pady=(6, 0), padx=(12, 0))
        self.drop_tree.tag_configure("new", foreground=GOLD)
        self.drop_caption = ttk.Label(body, text="", style="Muted.TLabel")
        self.drop_caption.grid(row=2, column=1, sticky="w", padx=(12, 0), pady=(4, 0))

        ttk.Label(body, text="Recent duels", style="Head.TLabel").grid(row=3, column=0, sticky="w", pady=(10, 0))
        f3, self.recent_tree = scrolled(body, lambda m: SortableTree(
            m, ("When", "Opponent", "Result", "Rank", "Score", "Drop"), (120, 150, 60, 70, 55, 180),
            {"Result": "center", "Rank": "center", "Score": "center"}, height=5))
        f3.grid(row=4, column=0, columnspan=2, sticky="nsew", pady=(6, 0))
        self.recent_tree.tag_configure("win", foreground=GREEN)
        self.recent_tree.tag_configure("loss", foreground=RED)

    def _lp_meter(self, parent, label, color):
        f = tk.Frame(parent, bg=PANEL)
        f.pack(side="left", fill="x", expand=True, padx=(0, 16))
        top = tk.Frame(f, bg=PANEL)
        top.pack(fill="x")
        tk.Label(top, text=label, bg=PANEL, fg=MUTED, font=(UI_FONT, 9, "bold")).pack(side="left")
        val = tk.Label(top, text="—", bg=PANEL, fg=TEXT, font=(UI_FONT, 18, "bold"))
        val.pack(side="right")
        bar = tk.Canvas(f, height=10, bg=PANEL2, highlightthickness=0)
        bar.pack(fill="x", pady=(2, 0))
        return {"val": val, "bar": bar, "color": color}

    # ---- card overview tab
    def _build_cards_tab(self):
        tab = ttk.Frame(self.nb, style="Panel.TFrame", padding=12)
        self.nb.add(tab, text="  Card Overview  ")
        tab.columnconfigure(0, weight=1)
        tab.columnconfigure(1, weight=0)
        tab.columnconfigure(2, weight=1)
        tab.rowconfigure(2, weight=1)

        ttk.Label(tab, text="All cards", style="Head.TLabel").grid(row=0, column=0, sticky="w")
        self.card_search = tk.StringVar()
        e = ttk.Entry(tab, textvariable=self.card_search)
        e.grid(row=1, column=0, sticky="ew", pady=6)
        e.insert(0, "")
        self.card_search.trace_add("write", lambda *_: self._fill_all_cards())
        f, self.all_tree = scrolled(tab, lambda m: SortableTree(
            m, ("ID", "Name", "Type", "ATK", "DEF", "Owned"), (40, 170, 95, 50, 50, 58),
            {"ID": "center", "ATK": "center", "DEF": "center", "Owned": "center"}))
        f.grid(row=2, column=0, sticky="nsew")
        self.all_tree.bind("<Double-1>", lambda _e: self.add_tracked())
        ttk.Label(tab, text="Search by name, type or ID. Double-click to track.", style="Muted.TLabel").grid(
            row=3, column=0, sticky="w", pady=(4, 0))

        mid = ttk.Frame(tab, style="Panel.TFrame", padding=(10, 0))
        mid.grid(row=2, column=1)
        ttk.Button(mid, text="Track  ▶", style="Gold.TButton", command=self.add_tracked).pack(fill="x", pady=3)
        ttk.Button(mid, text="◀  Untrack", command=self.remove_tracked).pack(fill="x", pady=3)
        ttk.Button(mid, text="Track my deck", command=self.track_deck).pack(fill="x", pady=(18, 3))
        ttk.Button(mid, text="Track Exodia", command=lambda: self.track_ids([17, 18, 19, 20, 21])).pack(fill="x", pady=3)
        ttk.Button(mid, text="Clear all", command=self.clear_tracked).pack(fill="x", pady=(18, 3))

        hdr = ttk.Frame(tab, style="Panel.TFrame")
        hdr.grid(row=0, column=2, sticky="ew")
        ttk.Label(hdr, text="Tracked cards", style="Head.TLabel").pack(side="left")
        self.tracked_summary = ttk.Label(hdr, text="", style="Muted.TLabel")
        self.tracked_summary.pack(side="right")
        ttk.Label(tab, text="Counts update live from your deck and trunk. Changes flash for 15 s.",
                  style="Muted.TLabel").grid(row=1, column=2, sticky="w")
        f2, self.track_tree = scrolled(tab, lambda m: SortableTree(
            m, ("ID", "Name", "In deck", "In trunk", "Total", "Change", "ATK/DEF"),
            (44, 190, 72, 76, 56, 70, 96),
            {"ID": "center", "In deck": "center", "In trunk": "center", "Total": "center", "Change": "center",
             "ATK/DEF": "center"}))
        f2.grid(row=2, column=2, sticky="nsew")
        self.track_tree.bind("<Delete>", lambda _e: self.remove_tracked())
        self.track_tree.bind("<Double-1>", lambda _e: self.remove_tracked())
        self.track_tree.tag_configure("up", background="#1e4630", foreground="#c9ffd9")
        self.track_tree.tag_configure("down", background="#4a2026", foreground="#ffd2d2")
        self.track_tree.tag_configure("none", foreground=MUTED)

        ttk.Label(tab, text="Last cards dropped", style="Head.TLabel").grid(row=4, column=2, sticky="w", pady=(10, 0))
        self.drops_label = ttk.Label(tab, text="—", style="Panel.TLabel", wraplength=560, justify="left")
        self.drops_label.grid(row=5, column=2, sticky="w")
        self._fill_all_cards()
        self._fill_tracked(force=True)

    # ---- duelists tab
    def _build_duelists_tab(self):
        tab = ttk.Frame(self.nb, style="Panel.TFrame", padding=12)
        self.nb.add(tab, text="  Duelists  ")
        tab.columnconfigure(0, weight=3)
        tab.columnconfigure(1, weight=2)
        tab.rowconfigure(1, weight=1)
        ttk.Label(tab, text="Chance to beat each duelist", style="Head.TLabel").grid(row=0, column=0, sticky="w")
        f, self.duelist_tree = scrolled(tab, lambda m: SortableTree(
            m, ("#", "Duelist", "Free W", "Free L", "Seen W", "Seen L", "Win rate", "Chance to beat", "Best rank"),
            (36, 160, 64, 64, 66, 66, 76, 120, 84),
            {c: "center" for c in ("#", "Free W", "Free L", "Seen W", "Seen L", "Win rate", "Chance to beat",
                                   "Best rank")}))
        f.grid(row=1, column=0, sticky="nsew", pady=(6, 0))
        self.duelist_tree.tag_configure("current", background="#3a3220", foreground="#ffe6a8")
        self.duelist_tree.bind("<<TreeviewSelect>>", lambda _e: self._fill_duelist_detail())
        for d in range(1, game.N_DUELISTS + 1):
            self.duelist_tree.insert("", "end", iid=str(d), values=(d, game.DUELISTS[d - 1], "", "", "", "", "", "", ""))
        ttk.Label(tab, text=("Free W/L = the game's own Free Duel record.  Seen = duels this tracker watched "
                             "(story included).  Chance to beat blends your record, a difficulty guess and your "
                             "deck's strength vs. the CPU's deck; during a duel it also follows life points."),
                  style="Muted.TLabel", wraplength=640, justify="left").grid(row=2, column=0, sticky="w", pady=(6, 0))

        side = ttk.Frame(tab, style="Panel.TFrame")
        side.grid(row=0, column=1, rowspan=3, sticky="nsew", padx=(12, 0))
        side.rowconfigure(2, weight=1)
        side.columnconfigure(0, weight=1)
        self.detail_title = ttk.Label(side, text="Select a duelist", style="Head.TLabel")
        self.detail_title.grid(row=0, column=0, sticky="w")
        self.detail_tier = tk.StringVar(value=game.DROP_TIER_NAMES[0])
        cb = ttk.Combobox(side, textvariable=self.detail_tier, values=list(game.DROP_TIER_NAMES) + ["Deck pool"],
                          state="readonly", width=14)
        cb.grid(row=1, column=0, sticky="w", pady=6)
        cb.bind("<<ComboboxSelected>>", lambda _e: self._fill_duelist_detail())
        f2, self.detail_tree = scrolled(side, lambda m: SortableTree(
            m, ("Card", "Chance", "Owned"), (180, 64, 64), {"Chance": "center", "Owned": "center"}))
        f2.grid(row=2, column=0, sticky="nsew")
        self.detail_tree.tag_configure("new", foreground=GOLD)
        self.detail_note = ttk.Label(side, text="", style="Muted.TLabel", wraplength=330, justify="left")
        self.detail_note.grid(row=3, column=0, sticky="w", pady=(4, 0))

    # ---- passwords tab
    def _build_passwords_tab(self):
        tab = ttk.Frame(self.nb, style="Panel.TFrame", padding=12)
        self.nb.add(tab, text="  Passwords  ")
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(2, weight=1)
        bar = ttk.Frame(tab, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew")
        ttk.Label(bar, text="All card passwords", style="Head.TLabel").pack(side="left")
        self.pw_search = tk.StringVar()
        ttk.Entry(bar, textvariable=self.pw_search, width=32).pack(side="left", padx=(16, 8))
        self.pw_search.trace_add("write", lambda *_: self._fill_passwords())
        self.pw_unowned = tk.BooleanVar()
        self.pw_afford = tk.BooleanVar()
        ttk.Checkbutton(bar, text="Not owned", variable=self.pw_unowned, command=self._fill_passwords).pack(side="left", padx=6)
        ttk.Checkbutton(bar, text="Affordable now", variable=self.pw_afford, command=self._fill_passwords).pack(side="left", padx=6)
        self.pw_count = ttk.Label(bar, text="", style="Muted.TLabel")
        self.pw_count.pack(side="right")
        ttk.Label(tab, text="Enter these at the Password screen (cost in starchips; — = can't be bought). Double-click a "
                            "row to copy the password.", style="Muted.TLabel").grid(row=1, column=0, sticky="w", pady=(4, 6))
        f, self.pw_tree = scrolled(tab, lambda m: SortableTree(
            m, ("ID", "Name", "Password", "Cost", "Type", "Attr", "Lv", "ATK", "DEF", "Owned"),
            (44, 230, 100, 70, 110, 70, 40, 60, 60, 66),
            {c: "center" for c in ("ID", "Password", "Cost", "Lv", "ATK", "DEF", "Owned", "Attr")}))
        f.grid(row=2, column=0, sticky="nsew")
        self.pw_tree.configure(style="Treeview")
        self.pw_tree.tag_configure("afford", foreground=GREEN)
        self.pw_tree.tag_configure("nobuy", foreground=MUTED)
        self.pw_tree.bind("<Double-1>", self._copy_password)
        self._fill_passwords()

    # ---- log tab
    def _build_log_tab(self):
        tab = ttk.Frame(self.nb, style="Panel.TFrame", padding=12)
        self.nb.add(tab, text="  Log  ")
        self.log_text = tk.Text(tab, bg=PANEL, fg=TEXT, insertbackground=TEXT, relief="flat",
                                font=(MONO_FONT, 10), wrap="word", height=10)
        self.log_text.pack(fill="both", expand=True)
        self.log_text.configure(state="disabled")
        self.log("FM Tracker ready. Start your emulator, load your save, then press Attach (or Demo to try it).")

    # ------------------------------------------------------------------ logging
    def log(self, msg):
        def do():
            self.log_text.configure(state="normal")
            self.log_text.insert("end", time.strftime("[%H:%M:%S] ") + msg + "\n")
            self.log_text.see("end")
            self.log_text.configure(state="disabled")
        if threading.current_thread() is threading.main_thread():
            do()
        else:
            self.root.after(0, do)

    # ------------------------------------------------------------------ attach
    def refresh_emulators(self):
        self.emulators = emulator.find_emulators()
        labels = [f"{name}  (pid {pid})" for pid, name, _ in self.emulators]
        if not emulator.IS_WINDOWS:
            labels = ["(attaching needs Windows - use Demo)"]
        elif not labels:
            labels = ["No emulator found - start one, then ↻"]
        self.emu_box["values"] = labels
        self.emu_box.current(0)

    def auto_attach(self):
        if self.source is None and self.emulators:
            self.attach_selected(quiet=True)

    def attach_selected(self, quiet=False):
        if self.attaching:
            return
        if not self.emulators:
            self.refresh_emulators()
            if not self.emulators:
                if not quiet:
                    messagebox.showinfo(APP_NAME, "No supported emulator is running.\n\nSupported: DuckStation, "
                                        "ePSXe, RetroArch (SwanStation / Beetle PSX / PCSX ReARMed), Mednafen, "
                                        "pSX, PCSX-Redux, Xebra, BizHawk (memory scan).")
                return
        idx = max(0, self.emu_box.current())
        pid, name, kind = self.emulators[min(idx, len(self.emulators) - 1)]
        self.detach()
        self.attaching = True
        self.attach_btn.configure(state="disabled")
        self._set_status(f"Attaching to {name}…", "attach")
        self.log(f"Attaching to {name} (pid {pid}, {kind})…")
        src = LiveSource(self.log)

        def progress(frac):
            self.root.after(0, lambda: self._set_status(f"Scanning {name} memory… {frac:.0%}", "attach"))

        def work():
            try:
                src.attach(pid, name, kind, progress=progress)
                self.root.after(0, lambda: self._start_source(src))
            except Exception as exc:
                msg = str(exc)
                self.root.after(0, lambda: self._attach_failed(msg, quiet))
        threading.Thread(target=work, daemon=True).start()

    def _attach_failed(self, msg, quiet):
        self.attaching = False
        self.attach_btn.configure(state="normal")
        self._set_status("Not attached", "off")
        self.log(msg)
        if not quiet:
            messagebox.showwarning(APP_NAME, msg)

    def start_demo(self):
        self.detach()
        self.log("Demo mode: a simulated game is playing endless duels.")
        self._start_source(DemoSource())

    def _start_source(self, src):
        self.attaching = False
        self.attach_btn.configure(state="normal")
        self.source = src
        self.tracker = game.DuelTracker()
        self.prev_totals = {}
        self.poller = Poller(src, self._on_snapshot)
        self.poller.start()

    def detach(self):
        if self.poller:
            self.poller.stop()
        if self.source:
            self.source.detach()
        self.poller = self.source = None
        with self.lock:
            self.latest = None
        self._set_status("Not attached", "off")

    def _on_snapshot(self, snap, status):
        with self.lock:
            self.latest = snap
            self.latest_status = status

    def _set_status(self, text, state):
        self.status_var.set(text)
        color = {"ok": GREEN, "wait": GOLD, "attach": BLUE, "off": RED}.get(state, MUTED)
        self.status_dot.delete("all")
        self.status_dot.create_oval(2, 2, 12, 12, fill=color, outline="")

    # ------------------------------------------------------------------ main loop
    def _tick(self):
        try:
            with self.lock:
                snap, status = self.latest, self.latest_status
            if self.source is not None:
                if isinstance(self.source, LiveSource) and self.source.proc is None:
                    self.log("Detached (emulator closed).")
                    self.detach()
                elif snap is None:
                    self._set_status(status, "wait")
                elif not snap.save_loaded:
                    self._set_status(f"{status} — waiting for a save to load", "wait")
                else:
                    self._set_status(status, "ok")
                    self._process(snap)
        except Exception as exc:
            self.log(f"UI update error: {exc}")
        self.root.after(250, self._tick)

    def _process(self, s: game.Snapshot):
        for kind, info in self.tracker.update(s):
            self._on_duel_event(kind, info, s)
        self._follow_pending(s)
        if self.tracker.active and 1 <= s.opponent <= game.N_DUELISTS:
            self._cache_pools(s)
        self._update_header(s)
        self._update_cards(s)
        self._update_duel(s)
        self._update_duelists(s)
        self._update_pw_owned(s)
        self._update_mini(s)

    # ------------------------------------------------------------------ duel events
    def _on_duel_event(self, kind, info, s):
        opp = info["opponent"]
        name = game.DUELISTS[opp - 1] if 1 <= opp <= game.N_DUELISTS else f"#{opp}"
        if kind == "start":
            self.drops_at_start = list(s.last_drops)
            self.log(f"Duel started vs {name}.")
            self.nb.select(0)
        elif kind == "abort":
            self.log(f"Duel vs {name} left without a result (reset or state load).")
        elif kind == "end":
            score, _ = game.rank_breakdown(info["you"], info["table"], assume_win_bonus=False)
            label = game.rank_label(score)[2] if info["result"] == "win" else "—"
            entry = {
                "time": time.strftime("%Y-%m-%d %H:%M"),
                "opponent": opp, "result": info["result"], "kind": info["kind"],
                "score": score if info["result"] == "win" else None, "rank": label,
                "turns": info["you"].counters.get(0, 0), "lp": info["you"].lp,
                "free_duel": False, "drop": None,
            }
            self.history.append(entry)
            _save_json("history.json", self.history)
            self.pending_end = {"entry": entry, "t": time.time(),
                                "drops": getattr(self, "drops_at_start", list(s.last_drops)),
                                "rec": self.tracker.start_records.get(opp) if self.tracker.start_records else None}
            self.log(f"Duel vs {name}: {info['result'].upper()}" + (f" — {label} ({score})" if label != "—" else ""))
            self._fill_recent()

    def _follow_pending(self, s):
        """After a duel: notice the card drop and whether it was a Free Duel."""
        p = self.pending_end
        if not p:
            return
        e = p["entry"]
        if s.last_drops and s.last_drops != p["drops"] and e["result"] == "win" and not e["drop"]:
            e["drop"] = s.last_drops[0]
            self.log(f"Card drop: {card_name(e['drop'])}")
        rec = s.records.get(e["opponent"])
        if p["rec"] is not None and rec and tuple(rec) != tuple(p["rec"]) and not e["free_duel"]:
            e["free_duel"] = True
        if time.time() - p["t"] > 90 or (e["drop"] and e["free_duel"]):
            self.pending_end = None
        _save_json("history.json", self.history)
        self._fill_recent()

    def _cache_pools(self, s):
        key = str(s.opponent)
        if key in self.pool_cache:
            return
        if game.pool_valid(s.cpu_deck_pool) and all(game.pool_valid(p) for p in s.cpu_drop_pools):
            self.pool_cache[key] = {"deck": s.cpu_deck_pool, "drops": s.cpu_drop_pools}
            _save_json("cpu_pools.json", self.pool_cache)
            self.log(f"Learned {game.DUELISTS[s.opponent - 1]}'s deck and drop tables.")

    # ------------------------------------------------------------------ views
    def _update_header(self, s):
        self.stat_vars["Starchips"].set(f"{s.starchips:,}")
        self.stat_vars["Deck"].set(f"{len(s.deck)} cards")
        self.stat_vars["Trunk cards"].set(f"{sum(s.trunk):,}")
        dc = s.deck_counts
        owned = sum(1 for cid in range(1, game.N_CARDS + 1) if s.trunk[cid - 1] or dc.get(cid))
        self.stat_vars["Unique owned"].set(f"{owned} / {game.N_CARDS}")
        if self.tracker.active:
            self.stat_vars["Opponent"].set(game.DUELISTS[self.tracker.opponent - 1])
        else:
            self.stat_vars["Opponent"].set("—")

    # ---- cards
    def _owned(self, cid):
        s = self.latest
        if not s or not s.save_loaded:
            return None
        return s.trunk[cid - 1] + s.deck_counts.get(cid, 0)

    def _fill_all_cards(self):
        q = self.card_search.get().strip().lower()
        t = self.all_tree
        t.delete(*t.get_children())
        for cid, c in game.CARDS.items():
            if q and not (q in c["name"].lower() or q == str(cid) or q in c["type"].lower()):
                continue
            own = self._owned(cid)
            t.insert("", "end", iid=str(cid), values=(cid, c["name"], c["type"], c["atk"], c["def_"],
                                                       "" if own is None else own))

    def add_tracked(self):
        ids = [int(i) for i in self.all_tree.selection()]
        self.track_ids(ids)

    def track_ids(self, ids):
        changed = False
        for cid in ids:
            if cid not in self.tracked:
                self.tracked.append(cid)
                changed = True
        if changed:
            self._save_settings()
            self._fill_tracked(force=True)

    def track_deck(self):
        s = self.latest
        if not s or not s.save_loaded:
            messagebox.showinfo(APP_NAME, "Attach to the game with a save loaded first.")
            return
        self.track_ids(sorted(set(s.deck)))

    def remove_tracked(self):
        for iid in self.track_tree.selection():
            cid = int(iid)
            if cid in self.tracked:
                self.tracked.remove(cid)
        self._save_settings()
        self._fill_tracked(force=True)

    def clear_tracked(self):
        if self.tracked and messagebox.askyesno(APP_NAME, "Stop tracking all cards?"):
            self.tracked = []
            self._save_settings()
            self._fill_tracked(force=True)

    def _fill_tracked(self, force=False):
        s = self.latest if (self.latest and self.latest.save_loaded) else None
        t = self.track_tree
        existing = set(t.get_children())
        wanted = [str(c) for c in self.tracked]
        if force or set(wanted) != existing:
            t.delete(*t.get_children())
            for cid in self.tracked:
                c = game.CARDS[cid]
                t.insert("", "end", iid=str(cid), values=(cid, c["name"], "", "", "", "", f"{c['atk']}/{c['def_']}"))
        now = time.time()
        total_cards = 0
        dc = s.deck_counts if s else {}
        for cid in self.tracked:
            c = game.CARDS[cid]
            if s:
                d, tr = dc.get(cid, 0), s.trunk[cid - 1]
                tot = d + tr
                total_cards += tot
                change = self.changes.get(cid)
                ctext, tag = "", ("none",) if tot == 0 else ()
                if change and now - change[1] < 15:
                    ctext = f"{change[0]:+d}"
                    tag = ("up",) if change[0] > 0 else ("down",)
                t.item(str(cid), values=(cid, c["name"], d, tr, tot, ctext, f"{c['atk']}/{c['def_']}"), tags=tag)
        self.tracked_summary.configure(
            text=f"{len(self.tracked)} tracked · {total_cards} copies owned" if s else f"{len(self.tracked)} tracked")

    def _update_cards(self, s):
        dc = s.deck_counts
        totals = {cid: s.trunk[cid - 1] + dc.get(cid, 0) for cid in range(1, game.N_CARDS + 1)}
        if self.prev_totals:
            now = time.time()
            for cid, tot in totals.items():
                prev = self.prev_totals.get(cid, tot)
                if tot != prev:
                    self.changes[cid] = (tot - prev, now)
                    if cid in self.tracked:
                        self.log(f"{card_name(cid)}: {prev} → {tot}")
                    if self.all_tree.exists(str(cid)):
                        self.all_tree.set(str(cid), "Owned", tot)
        else:
            for cid, tot in totals.items():
                if self.all_tree.exists(str(cid)):
                    self.all_tree.set(str(cid), "Owned", tot)
        self.prev_totals = totals
        self._fill_tracked()
        self.drops_label.configure(text=", ".join(card_name(c) for c in s.last_drops) or "—")

    # ---- duel
    def _draw_lp(self, meter, lp, start):
        meter["val"].configure(text=f"{max(lp, 0):,}" if start else "—")
        bar = meter["bar"]
        bar.delete("all")
        w = bar.winfo_width() or 300
        frac = max(0.0, min(1.0, lp / start)) if start else 0
        bar.create_rectangle(0, 0, int(w * frac), 10, fill=meter["color"], outline="")

    def _draw_rank_bar(self, score):
        c = self.rank_bar
        c.delete("all")
        w = 230
        for i in range(10):
            x0 = i * w / 10
            letter = "SABCD"[i] if i < 5 else "DCBAS"[i - 5]
            col = TEC_COLOR if i < 5 else POW_COLOR
            c.create_rectangle(x0 + 1, 6, x0 + w / 10 - 1, 20, fill=PANEL, outline=col)
            c.create_text(x0 + w / 20, 13, text=letter, fill=col, font=(UI_FONT, 7, "bold"))
        if score is not None:
            x = max(0, min(99, score)) / 99 * (w - 4) + 2
            c.create_polygon(x - 5, 0, x + 5, 0, x, 7, fill=GOLD)
            c.create_line(x, 6, x, 22, fill=GOLD, width=2)

    def _update_duel(self, s):
        active = self.tracker.active
        side = s.you
        score, rows = game.rank_breakdown(side, s.table, assume_win_bonus=self.assume_win.get())
        if active:
            name = game.DUELISTS[self.tracker.opponent - 1]
            self.duel_title.configure(text=f"Dueling {name}")
            letter, style, _ = game.rank_label(score)
            self.rank_letter.configure(text=letter, fg=LETTER_COLORS[letter])
            self.rank_style.configure(text=style, fg=POW_COLOR if style == "POW" else TEC_COLOR)
            self.rank_score.configure(text=f"score {score}" + ("" if s.table_live else " (est.)"))
            self._draw_rank_bar(score)
            self._draw_lp(self.lp_you, s.you.lp, s.you.start_lp)
            self._draw_lp(self.lp_cpu, s.cpu.lp, s.cpu.start_lp or s.you.start_lp)
            p, note = self._chance_for(self.tracker.opponent, s, live=True)
            self.win_pct.configure(text=f"{p:.0%}", fg=GREEN if p >= 0.6 else (GOLD if p >= 0.35 else RED))
            self.win_note.configure(text=note)
        elif self.history and self.history[-1].get("score") is not None and self.history[-1]["result"] == "win":
            e = self.history[-1]
            letter, style, _ = game.rank_label(e["score"])
            self.duel_title.configure(text=f"Last duel: beat {game.DUELISTS[e['opponent'] - 1]} — waiting for the next one")
            self.rank_letter.configure(text=letter, fg=LETTER_COLORS[letter])
            self.rank_style.configure(text=style, fg=POW_COLOR if style == "POW" else TEC_COLOR)
            self.rank_score.configure(text=f"final score {e['score']}")
            self._draw_rank_bar(e["score"])
            self._draw_lp(self.lp_you, 0, 0)
            self._draw_lp(self.lp_cpu, 0, 0)
            self.win_pct.configure(text="—", fg=GOLD)
            self.win_note.configure(text="")
        else:
            self.duel_title.configure(text="No duel in progress — waiting for the next one")
            self.rank_letter.configure(text="–", fg=MUTED)
            self.rank_style.configure(text="")
            self.rank_score.configure(text="start a duel to see your rank")
            self._draw_rank_bar(None)
            self._draw_lp(self.lp_you, 0, 0)
            self._draw_lp(self.lp_cpu, 0, 0)
            self.win_pct.configure(text="—", fg=GOLD)
            self.win_note.configure(text="")
        # breakdown
        t = self.rank_tree
        if not t.get_children():
            for name, *_ in rows:
                t.insert("", "end", iid=name, values=(name, "", "", ""))
        for name, v, chg, note in rows:
            tag = "pos" if chg > 0 else ("neg" if chg < 0 else "zero")
            t.item(name, values=(name, v if active else "—", f"{chg:+d}" if active else "—",
                                 note if active else ""), tags=(tag,) if active else ())
        # possible drops
        tier = game.drop_tier(score)
        pools = None
        if active and all(game.pool_valid(p) for p in s.cpu_drop_pools):
            pools = s.cpu_drop_pools
        sig = (active, tier, self.tracker.opponent, pools is not None)
        if getattr(self, "_drop_sig", None) != sig:
            self._drop_sig = sig
            self._fill_drop_tree(self.drop_tree, pools[tier] if pools else None)
            self.drop_caption.configure(
                text=f"{game.DROP_TIER_NAMES[tier]} table · gold = you don't own it" if pools else
                "Shown during a duel.")

    def _fill_drop_tree(self, tree, pool):
        tree.delete(*tree.get_children())
        if not pool:
            return
        items = sorted(((w, i + 1) for i, w in enumerate(pool) if w), reverse=True)
        for w, cid in items:
            own = self._owned(cid)
            tree.insert("", "end", values=(card_name(cid), f"{w / 2048:.1%}", "" if own is None else own),
                        tags=("new",) if own == 0 else ())

    def _fill_recent(self):
        t = self.recent_tree
        t.delete(*t.get_children())
        for e in reversed(self.history[-40:]):
            opp = e.get("opponent", 0)
            name = game.DUELISTS[opp - 1] if 1 <= opp <= game.N_DUELISTS else "?"
            drop = card_name(e["drop"]) if e.get("drop") else ""
            res = e.get("result", "?")
            t.insert("", "end", values=(e.get("time", ""), name, res.upper(), e.get("rank", ""),
                                        e.get("score") if e.get("score") is not None else "", drop),
                     tags=(res,))

    # ---- duelists
    def _record(self, d, s):
        fw, fl = s.records.get(d, (0, 0)) if s else (0, 0)
        sw = sum(1 for e in self.history if e.get("opponent") == d and e.get("result") == "win")
        sl = sum(1 for e in self.history if e.get("opponent") == d and e.get("result") == "loss")
        # Story duels the game's Free Duel record doesn't contain
        xw = sum(1 for e in self.history if e.get("opponent") == d and e.get("result") == "win" and not e.get("free_duel"))
        xl = sum(1 for e in self.history if e.get("opponent") == d and e.get("result") == "loss" and not e.get("free_duel"))
        return fw, fl, sw, sl, fw + xw, fl + xl

    def _chance_for(self, d, s, live=False):
        fw, fl, sw, sl, w, l = self._record(d, s)
        my_power = game.deck_power(s.deck) if s and s.deck else None
        cpu_power = None
        pool = None
        if live and game.pool_valid(s.cpu_deck_pool):
            pool = s.cpu_deck_pool
        elif str(d) in self.pool_cache:
            pool = self.pool_cache[str(d)]["deck"]
        if pool:
            cpu_power = game.pool_power(pool)
        lv = (s.you.lp, s.cpu.lp, s.you.start_lp or 8000) if live else None
        p = game.win_chance(w, l, game.DIFFICULTY[d - 1], my_power, cpu_power, lv)
        parts = [f"record {w}-{l}"] if w + l else ["no record yet"]
        if cpu_power:
            parts.append(f"your top ATK {my_power:.0f} vs CPU avg {cpu_power:.0f}")
        if live:
            parts.append("following life points")
        return p, " · ".join(parts)

    def _update_duelists(self, s):
        now = time.time()
        if now - getattr(self, "_duelists_t", 0) < 1.0:
            return
        self._duelists_t = now
        best = {}
        for e in self.history:
            if e.get("result") == "win" and e.get("score") is not None:
                d = e["opponent"]
                cur = best.get(d)
                if cur is None or abs(e["score"] - 50) > abs(cur - 50):
                    best[d] = e["score"]
        for d in range(1, game.N_DUELISTS + 1):
            fw, fl, sw, sl, w, l = self._record(d, s)
            live = self.tracker.active and self.tracker.opponent == d
            p, _ = self._chance_for(d, s, live=live)
            rate = f"{w / (w + l):.0%}" if w + l else "—"
            br = game.rank_label(best[d])[2] if d in best else ""
            self.duelist_tree.item(str(d), values=(d, game.DUELISTS[d - 1], fw, fl, sw, sl, rate, f"{p:.0%}", br),
                                   tags=("current",) if live else ())

    def _fill_duelist_detail(self):
        sel = self.duelist_tree.selection()
        if not sel:
            return
        d = int(sel[0])
        self.detail_title.configure(text=game.DUELISTS[d - 1])
        cache = self.pool_cache.get(str(d))
        choice = self.detail_tier.get()
        if not cache:
            self._fill_drop_tree(self.detail_tree, None)
            self.detail_note.configure(text="Duel this opponent once while the tracker is attached and their "
                                            "deck and drop tables are learned automatically.")
            return
        pool = cache["deck"] if choice == "Deck pool" else cache["drops"][game.DROP_TIER_NAMES.index(choice)]
        self._fill_drop_tree(self.detail_tree, pool)
        self.detail_note.configure(text="Gold = a card you don't own yet." if choice != "Deck pool" else
                                   "Chance of each card in the CPU's 40-card deck draw.")

    # ---- passwords
    def _fill_passwords(self):
        q = self.pw_search.get().strip().lower()
        t = self.pw_tree
        t.delete(*t.get_children())
        s = self.latest if (self.latest and self.latest.save_loaded) else None
        n = 0
        for cid, c in game.CARDS.items():
            if q and not (q in c["name"].lower() or q == str(cid) or q in (c["password"] or "") or q in c["type"].lower()):
                continue
            own = self._owned(cid)
            if self.pw_unowned.get() and own:
                continue
            cost = c["cost"]
            buyable = c["password"] and cost is not None and cost < 999999
            if self.pw_afford.get() and not (s and buyable and cost <= s.starchips):
                continue
            tag = ()
            if not buyable:
                tag = ("nobuy",)
            elif s and cost <= s.starchips:
                tag = ("afford",)
            t.insert("", "end", iid=str(cid), values=(
                cid, c["name"], c["password"] or "—", f"{cost:,}" if buyable else "—",
                c["type"], c["attr"] or "", c["level"] or "", c["atk"], c["def_"], "" if own is None else own),
                tags=tag)
            n += 1
        self.pw_count.configure(text=f"{n} cards · green = affordable with your starchips")

    def _update_pw_owned(self, s):
        sig = (s.starchips, tuple(self.prev_totals.get(i, 0) for i in range(1, game.N_CARDS + 1)))
        if getattr(self, "_pw_sig", None) == sig:
            return
        first = getattr(self, "_pw_sig", None) is None
        self._pw_sig = sig
        if first or self.pw_unowned.get() or self.pw_afford.get():
            self._fill_passwords()
            return
        for cid in self.pw_tree.get_children():
            c = game.CARDS[int(cid)]
            self.pw_tree.set(cid, "Owned", self.prev_totals.get(int(cid), 0))
            buyable = c["password"] and c["cost"] is not None and c["cost"] < 999999
            self.pw_tree.item(cid, tags=("afford",) if buyable and c["cost"] <= s.starchips else
                              (("nobuy",) if not buyable else ()))

    def _copy_password(self, _e):
        sel = self.pw_tree.selection()
        if not sel:
            return
        c = game.CARDS[int(sel[0])]
        if c["password"]:
            self.root.clipboard_clear()
            self.root.clipboard_append(c["password"])
            self.log(f"Copied password for {c['name']}: {c['password']}")

    # ------------------------------------------------------------------ mini overlay
    def toggle_mini(self):
        if self.mini:
            self.mini.destroy()
            self.mini = None
            return
        m = tk.Toplevel(self.root)
        m.title("FM Rank")
        m.configure(bg=PANEL2)
        m.attributes("-topmost", True)
        m.resizable(False, False)
        m.protocol("WM_DELETE_WINDOW", self.toggle_mini)
        self.mini = m
        row = tk.Frame(m, bg=PANEL2)
        row.pack(padx=12, pady=8)
        self.mini_rank = tk.Label(row, text="–", bg=PANEL2, fg=MUTED, font=(UI_FONT, 30, "bold"), width=6)
        self.mini_rank.pack(side="left")
        self.mini_win = tk.Label(row, text="—", bg=PANEL2, fg=GOLD, font=(UI_FONT, 30, "bold"), width=5)
        self.mini_win.pack(side="left", padx=(10, 0))
        self.mini_info = tk.Label(m, text="", bg=PANEL2, fg=MUTED, font=(UI_FONT, 9))
        self.mini_info.pack(pady=(0, 8))

    def _update_mini(self, s):
        if not self.mini:
            return
        if self.tracker.active:
            score, _ = game.rank_breakdown(s.you, s.table, assume_win_bonus=self.assume_win.get())
            letter, style, label = game.rank_label(score)
            self.mini_rank.configure(text=label, fg=LETTER_COLORS[letter])
            p, _ = self._chance_for(self.tracker.opponent, s, live=True)
            self.mini_win.configure(text=f"{p:.0%}")
            self.mini_info.configure(text=f"score {score} · vs {game.DUELISTS[self.tracker.opponent - 1]} · "
                                          f"LP {s.you.lp} / {s.cpu.lp}")
        else:
            self.mini_rank.configure(text="–", fg=MUTED)
            self.mini_win.configure(text="—")
            self.mini_info.configure(text="waiting for a duel")

    # ------------------------------------------------------------------ shutdown
    def _save_settings(self):
        self.settings["tracked"] = self.tracked
        self.settings["assume_win"] = bool(self.assume_win.get())
        try:
            self.settings["geometry"] = self.root.geometry()
        except tk.TclError:
            pass
        _save_json("settings.json", self.settings)

    def _on_close(self):
        self._save_settings()
        _save_json("history.json", self.history)
        self.detach()
        self.root.destroy()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if sys.platform == "win32":
        try:
            ctypes = __import__("ctypes")
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    root = tk.Tk()
    app = TrackerApp(root, demo="--demo" in argv)
    app._fill_recent()
    root.mainloop()


if __name__ == "__main__":
    main()
