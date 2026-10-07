#!/usr/bin/env python3
"""
rigctl_panel.py - a small standalone control panel for maxibitx.

Talks the same plain-text rigctld protocol WSJT-X/Thetis/etc. already use
against maxibitx's hamlib.c server (default TCP 4532 - see
docs/04_remot#!/usr/bin/env python3
"""
rigctl_panel.py - a touch-friendly control panel for maxibitx, laid out
for the sBitx's 7-inch 800x480 screen and just as usable in a window on
a laptop.

It is a client, separate from the maxibitx binary. Control goes over
maxibitx's rigctld server (hamlib.c, TCP 4532), the spectrum and
waterfall over its I/Q telemetry stream (iq_stream.c, UDP 4536).
docs/06_api.md describes both.

The screen, top to bottom:

    band menu, frequency, mode (CW CWR USB LSB DIGI), RX/TX, volume
    RIT and its steps          |  FILTER: on, width, centre, APF, NR
    span                                   status              S-meter
    spectrum, frequency scale, waterfall
    CW: ten macros  |  USB/LSB: mic gain and ALC  |  DIGI: a note
    key type, WPM, CW text, Send, Stop, power, Tune

The frequency is tuned digit by digit: tap a digit to make it the tuning
step, then turn the mouse wheel, drag up or down, or use the arrow keys.
Double-click (or double-tap) the frequency to type one in. The RX/TX
button opens Settings: the connection, the CW filter type, paddle
reversal, the TX test tones, your call and park for the macros, and the
macro texts.

APF, NR and XIT are on the screen but disabled: maxibitx has none of
them yet. TUNE keys the transmitter with the 1 kHz test tone (u TONE 1,
then T 1), so the carrier is 1 kHz from the dial; maxibitx drops it after
30 s.

rigctld commands used:

    f / F <hz>              frequency (F clears RIT)
    j / J <hz>              RIT, receive only, +/-9999 Hz
    m / M <mode> <pb>       mode; DIGITAL reads back as PKTUSB
    t / T <0|1>             PTT
    l AF / L AF             volume, 0.0-1.0
    l STRENGTH              S-meter, dB relative to S9
    l RFPOWER / L RFPOWER   TX power, 0.0-1.0 of max_power
    l ALC                   limiter gain reduction, dB
    l MICGAIN / L MICGAIN   mic gain multiplier, USB/LSB
    u NARROW / U NARROW     the CW filter in or out (DIGITAL holds it out)
    u FFTFILT / U FFTFILT   which CW filter: 1 FFT, 0 elliptic bank
    l CWPITCH / L CWPITCH   the filter's centre and the CW pitch, Hz
    l CWWIDTH / L CWWIDTH   the filter's width, Hz
    u KEYER / U KEYER       0 straight, 1 bug, 2 ultimatic, 3 iambic A, 4 B
    l KEYSPD / L KEYSPD     keyer speed, WPM
    u PADREV / U PADREV     paddle reversal
    u TONE / U TONE         test tone: 0 off, 1 1 kHz, 2 two-tone
    b <text>, \\stop_morse   send CW text, stop it (CW/CWR only)
    u MORSE                 1 while text is queued or going out

Requires Python 3 with tkinter, and numpy. On Raspberry Pi OS:
`sudo apt install python3-tk python3-numpy fonts-ibm-plex` (the fonts
are optional; without them the panel uses DejaVu).

Run it:

    python3 tools/rigctl_panel.py                    # a window, last host
    python3 tools/rigctl_panel.py --fullscreen       # the sBitx's screen
    python3 tools/rigctl_panel.py --host sbitx.local
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

try:
    import numpy as np
except ImportError:
    print("rigctl_panel.py needs numpy for the spectrum display "
          "(pip install numpy, or sudo apt install python3-numpy).",
          file=sys.stderr)
    sys.exit(1)

CONFIG_PATH = os.path.expanduser("~/.maxibitx_panel.json")
# Read when CONFIG_PATH doesn't exist yet, so a saved host/port carries
# over from the minibitx panel's file.
OLD_CONFIG_PATH = os.path.expanduser("~/.minibitx_panel.json")
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4532
POLL_INTERVAL_S = 1.0
RECONNECT_INTERVAL_MS = 5000
SOCKET_TIMEOUT_S = 2.0
WIN_W, WIN_H = 800, 480

# The keyer's modes in u/U KEYER's numbering (src/keyer.h's enum keyer_mode).
KEYER_MODES = ("Straight", "Bug", "Ultimatic", "Iambic A", "Iambic B")
MODES = ("CW", "CWR", "USB", "LSB", "DIGI")   # DIGI is rigctld's DIGITAL
CW_PITCHES = ("500", "600", "700", "800", "900", "1000")
CW_WIDTHS = ("150", "300", "450", "600")
# Full scale for the ALC bar. Correctly set mic gain reads a couple of dB
# on speech peaks, so the useful part of the scale is its bottom third.
ALC_METER_MAX_DB = 15.0

# Band menu: name, edges (Hz), and where the band starts if it has no
# remembered frequency yet. The panel remembers the last frequency used
# on each band in its config file.
BANDS = (
    ("80 m", 3500000, 4000000, 3530000),
    ("60 m", 5330500, 5406500, 5354000),
    ("40 m", 7000000, 7300000, 7030000),
    ("30 m", 10100000, 10150000, 10110000),
    ("20 m", 14000000, 14350000, 14030000),
    ("17 m", 18068000, 18168000, 18080000),
    ("15 m", 21000000, 21450000, 21030000),
    ("12 m", 24890000, 24990000, 24900000),
    ("10 m", 28000000, 29700000, 28030000),
)
FREQ_MIN_HZ, FREQ_MAX_HZ = 100000, 30000000   # what maxibitx tunes

# Macro texts may use {MYCALL} and {PARK}, filled from Settings.
DEFAULT_MACROS = (
    ("CQ POTA", "CQ POTA DE {MYCALL} {MYCALL} K"),
    ("QRZ?", "QRZ?"),
    ("5NN", "TU 5NN {PARK}"),
    ("TU 73", "TU 73 EE"),
    ("AGN?", "AGN?"),
    ("RR", "RR"),
    ("PARK", "{PARK} {PARK}"),
    ("CALL", "{MYCALL}"),
    ("QRL?", "QRL?"),
    ("EE", "EE"),
)

# --- Spectrum (iq_stream.c) ---
IQ_STREAM_PORT = 4536          # src/interfaces/iq_stream.h's IQ_STREAM_PORT
IQ_STREAM_MAGIC = b"IQS1"
SUBSCRIBE_INTERVAL_S = 1.0     # comfortably under iq_stream.c's 5 s subscriber timeout
FFT_SIZE = 2048                # 96000/2048 = 46.875 Hz/bin across the full +-48 kHz
SPECTRUM_REDRAW_MS = 66        # about 15 frames a second
SPECTRUM_DB_FLOOR = -100.0     # dBFS-style: 0 dB is one full-scale tone
# Not 0 dBFS: nothing on this receiver's raw I/Q comes near full scale,
# and -37.5 puts the strongest signal measured on an sBitx (-50 dBFS) at
# 80% of the height. A stronger signal flat-tops, and the trace says
# "clipping" when it does.
SPECTRUM_DB_CEILING = -37.5
# Displayed half-spans. The FFT always covers the full +-48 kHz at
# 46.875 Hz/bin; these crop it, so a narrow span spreads the same bins
# wider rather than resolving finer.
SPECTRUM_SPAN_CHOICES_HZ = (2500, 5000, 15000)
SPECTRUM_DEFAULT_HALF_SPAN_HZ = 15000
# Frequency scale under the trace: (major, minor) tick spacing in Hz for
# each half-span. Majors carry a label.
SCALE_TICKS_HZ = {2500: (1000, 500), 5000: (2000, 500), 15000: (5000, 1000)}
TRACE_H = 70                   # spectrum trace height, px
SCALE_H = 18                   # frequency scale strip height, px
WATERFALL_STOPS = ((0.00, (8, 19, 28)), (0.30, (0, 0, 150)), (0.50, (0, 170, 230)),
                   (0.70, (240, 230, 0)), (0.85, (250, 60, 0)), (1.00, (255, 255, 255)))

# --- S-meter (l STRENGTH) ---
# rx_audio.c's RX_STRENGTH_MIN_DB/MAX_DB: 0 dB is S9, 6 dB per S-unit.
STRENGTH_DB_PER_S_UNIT = 6.0
SMETER_SEGMENTS_DB = tuple(range(-48, 1, 6)) + (10, 20, 30, 40, 50, 60)

# Mirrors radio.h's RIT_MAX_HZ.
RIT_MAX_HZ = 9999

# --- colours ---
GROUND = "#0E1315"
PANEL = "#172024"
TILE = "#1F2A30"
TILE_HI = "#2A373E"
LINE = "#33434A"
TEXT = "#E8EEF0"
MUTED = "#8FA1A9"
DIM = "#55656C"
AMBER = "#F4A93B"
INK = "#1A1206"
TRACE = "#8FD3F4"
TX_RED = "#E5484D"


def s_unit_label(db):
    """A dB-relative-to-S9 reading as an S-meter reads: S1..S9, S9+N."""
    if db <= 0:
        s = round(9 + db / STRENGTH_DB_PER_S_UNIT)
        return f"S{max(0, min(9, s))}"
    return f"S9+{db}"


def waterfall_palette():
    """WATERFALL_STOPS as a 256-entry RGB lookup table."""
    x = np.linspace(0.0, 1.0, 256)
    pos = [stop for stop, _ in WATERFALL_STOPS]
    return np.stack([np.interp(x, pos, [rgb[i] for _, rgb in WATERFALL_STOPS])
                     for i in range(3)], axis=1).astype(np.uint8)


def band_of(hz):
    """The BANDS entry hz is in, or None."""
    for band in BANDS:
        if band[1] <= hz <= band[2]:
            return band
    return None


def parse_frequency(text):
    """A typed frequency in Hz, or None. "14058.2" and "14.0582" are
    read as kHz and MHz by size; a whole number of Hz is taken as is,
    and a smaller whole number as kHz."""
    t = text.strip().replace(",", "")
    try:
        v = float(t)
    except ValueError:
        return None
    if "." in t:
        hz = v * 1e6 if v < 100 else v * 1e3
    else:
        hz = v if v >= 100000 else v * 1e3
    hz = int(round(hz))
    return hz if FREQ_MIN_HZ <= hz <= FREQ_MAX_HZ else None


def load_config():
    for path in (CONFIG_PATH, OLD_CONFIG_PATH):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except (OSError, ValueError):
            continue
    return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f, indent=1)
    except OSError:
        pass  # best effort: losing the saved settings isn't worth failing over


def pick_family(candidates, fallback):
    available = set(tkfont.families())
    for name in candidates:
        if name in available:
            return name
    return fallback


class RigctlClient:
    """One TCP connection to maxibitx's rigctld server.

    rigctld is a plain line-request/line-reply protocol with no request
    IDs, so two commands must never be in flight on the same socket at
    once. self.lock keeps the panel's own actions and the background
    poll to one command at a time.
    """

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()

    def connect(self, host, port):
        s = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_S)
        s.settimeout(SOCKET_TIMEOUT_S)
        with self.lock:
            self.sock = s

    def disconnect(self):
        with self.lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None

    def connected(self):
        with self.lock:
            return self.sock is not None

    def query(self, command):
        """Send one command line, return the reply (stripped), or None if
        not connected or on any socket error, which also drops the
        connection. Never retries on its own."""
        with self.lock:
            if self.sock is None:
                return None
            try:
                self.sock.sendall((command + "\n").encode())
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = self.sock.recv(256)
                    if not chunk:
                        raise OSError("connection closed by remote")
                    buf += chunk
                return buf.decode(errors="replace").strip()
            except OSError:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
                return None


class SpectrumClient:
    """Subscribes to iq_stream.c's UDP stream and keeps an FFT of the most
    recent FFT_SIZE samples ready for the display.

    Independent of the rigctld connection: its own socket, its own
    keepalive (iq_stream.c drops a subscriber silent for 5 s) and its own
    receive thread. If UDP 4536 isn't reachable the spectrum stays empty
    and the controls keep working.

    dB: iq_stream.c scales a full-scale tone to int16 full scale, and the
    FFT is normalised by FFT_SIZE * mean(window), so 0 dB is one
    full-scale tone, as an uncalibrated SDR display reads.
    """

    def __init__(self):
        self.sock = None
        self.host = None
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        self.latest_db = None  # fftshifted, low to high frequency, or None
        self.window = np.hanning(FFT_SIZE)
        self.window_mean = float(np.mean(self.window)) or 1.0

    def start(self, host):
        self.stop()
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.5)
        self.stop_event.clear()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        with self.lock:
            self.latest_db = None
        threading.Thread(target=self._recv_loop, daemon=True).start()
        threading.Thread(target=self._keepalive_loop, daemon=True).start()

    def stop(self):
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _keepalive_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is not None:
                try:
                    sock.sendto(b"S", (self.host, IQ_STREAM_PORT))
                except OSError:
                    pass
            time.sleep(SUBSCRIBE_INTERVAL_S)

    def _recv_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is None:
                break
            try:
                data, _addr = sock.recvfrom(2048)
            except OSError:
                continue
            if len(data) < 12 or data[0:4] != IQ_STREAM_MAGIC:
                continue
            n_samples = struct.unpack(">I", data[8:12])[0]
            if len(data) != 12 + n_samples * 4:
                continue  # torn or short packet
            raw = np.frombuffer(data, dtype=">i2", count=n_samples * 2, offset=12)
            iq = raw.astype(np.float64).reshape(-1, 2)
            # Conjugated: the raw stream has a station at dial+d at -d
            # (sound.c's mixer), and the display wants it at +d, on the
            # right, as SDR displays draw it.
            samples = (iq[:, 0] - 1j * iq[:, 1]) / 32767.0
            self.sample_buf = np.concatenate((self.sample_buf, samples))
            if len(self.sample_buf) > FFT_SIZE * 2:
                self.sample_buf = self.sample_buf[-FFT_SIZE * 2:]
            if len(self.sample_buf) >= FFT_SIZE:
                block = self.sample_buf[-FFT_SIZE:]
                spectrum = np.fft.fftshift(np.fft.fft(block * self.window))
                mag = np.abs(spectrum) / (FFT_SIZE * self.window_mean)
                db = 20.0 * np.log10(mag + 1e-12)
                with self.lock:
                    self.latest_db = db

    def get_latest(self):
        with self.lock:
            return None if self.latest_db is None else self.latest_db.copy()


def setup_style(root):
    """The ttk 'clam' theme, dark, with controls tall enough for a finger
    (44-48 px). Returns the UI and monospace font families in use."""
    ui = pick_family(("IBM Plex Sans Condensed", "Barlow Semi Condensed",
                      "DejaVu Sans Condensed"), "TkDefaultFont")
    mono = pick_family(("IBM Plex Mono", "DejaVu Sans Mono"), "TkFixedFont")
    s = ttk.Style(root)
    s.theme_use("clam")
    root.option_add("*TCombobox*Listbox.font", (ui, 14))
    root.option_add("*TCombobox*Listbox.background", TILE)
    root.option_add("*TCombobox*Listbox.foreground", TEXT)
    root.option_add("*TCombobox*Listbox.selectBackground", AMBER)
    root.option_add("*TCombobox*Listbox.selectForeground", INK)
    s.configure(".", background=GROUND, foreground=TEXT, font=(ui, 12),
                bordercolor=LINE, lightcolor=TILE, darkcolor=TILE,
                troughcolor=PANEL, fieldbackground=TILE, insertcolor=TEXT,
                selectbackground=AMBER, selectforeground=INK)
    s.map(".", foreground=[("disabled", DIM)])
    s.configure("TFrame", background=GROUND)
    s.configure("Panel.TFrame", background=PANEL)
    s.configure("TLabel", background=GROUND, foreground=TEXT)
    s.configure("Muted.TLabel", foreground=MUTED, font=(ui, 10))
    s.configure("Status.TLabel", foreground=MUTED, font=(ui, 10))
    s.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED, font=(ui, 10))
    s.configure("Mono.TLabel", font=(mono, 13, "bold"))
    s.configure("TLabelframe", background=PANEL, bordercolor=LINE, relief="solid", borderwidth=1)
    s.configure("TLabelframe.Label", background=PANEL, foreground=MUTED, font=(ui, 10))
    s.configure("TButton", background=TILE, foreground=TEXT, padding=(6, 10),
                font=(ui, 12, "bold"), borderwidth=1, focusthickness=0)
    s.map("TButton", background=[("disabled", PANEL), ("pressed", AMBER), ("active", TILE_HI)],
          foreground=[("disabled", DIM), ("pressed", INK)])
    s.configure("Accent.TButton", background=AMBER, foreground=INK)
    s.map("Accent.TButton", background=[("disabled", PANEL), ("active", "#FFC060")])
    s.configure("Step.TButton", font=(mono, 13, "bold"), padding=(2, 10))
    s.configure("Value.TButton", font=(mono, 15, "bold"), background=PANEL, padding=(2, 9))
    s.configure("RX.TButton", font=(ui, 12, "bold"), foreground=MUTED, padding=(6, 8))
    s.configure("TX.TButton", font=(ui, 12, "bold"), background=TX_RED, foreground="#FFFFFF",
                padding=(6, 8))
    s.map("TX.TButton", background=[("active", TX_RED)])
    s.configure("Off.TButton", font=(ui, 12, "bold"), foreground=AMBER, padding=(6, 8))
    s.configure("Toolbutton", background=TILE, foreground=TEXT, padding=(4, 10),
                font=(ui, 12, "bold"), anchor="center", borderwidth=1)
    s.map("Toolbutton", background=[("disabled", PANEL), ("selected", AMBER), ("active", TILE_HI)],
          foreground=[("disabled", DIM), ("selected", INK)])
    s.configure("TCombobox", padding=(6, 9), arrowsize=18, foreground=TEXT,
                background=TILE, selectbackground=TILE, selectforeground=TEXT)
    s.map("TCombobox", fieldbackground=[("readonly", TILE), ("disabled", PANEL)],
          foreground=[("readonly", TEXT), ("disabled", DIM)],
          background=[("readonly", TILE), ("disabled", PANEL)])
    s.configure("TSpinbox", padding=(6, 9), arrowsize=16, foreground=TEXT, background=TILE)
    s.configure("TEntry", padding=(6, 10), foreground=TEXT)
    s.map("TEntry", fieldbackground=[("disabled", PANEL)])
    s.configure("Horizontal.TScale", background=TILE_HI, troughcolor=PANEL, borderwidth=1)
    s.configure("Horizontal.TProgressbar", background=AMBER, troughcolor=PANEL,
                bordercolor=LINE, lightcolor=AMBER, darkcolor=AMBER)
    s.configure("TCheckbutton", background=GROUND)
    s.configure("TRadiobutton", background=GROUND)
    s.configure("TNotebook", background=GROUND, borderwidth=0)
    s.configure("TNotebook.Tab", background=TILE, foreground=TEXT, padding=(14, 8),
                font=(ui, 12, "bold"))
    s.map("TNotebook.Tab", background=[("selected", AMBER)], foreground=[("selected", INK)])
    return ui, mono


class FreqDisplay(tk.Canvas):
    """The frequency, tunable digit by digit.

    Tap a digit to make it the tuning step (amber, underlined). The mouse
    wheel over a digit, a vertical drag on it, or the Up and Down keys
    change the frequency by that digit's place; Left and Right move the
    step. Double-click opens a box to type a frequency. on_tune(hz) is
    called with every new frequency; on_type() for the double-click.
    """

    DIGIT_W = 20
    SEP_W = 10
    DRAG_STEP_PX = 14

    def __init__(self, parent, mono, on_tune, on_type, **kw):
        super().__init__(parent, width=200, height=44, bg=GROUND, highlightthickness=0,
                         takefocus=1, **kw)
        self.font = (mono, 24, "bold")
        self.on_tune = on_tune
        self.on_type = on_type
        self.hz = None
        self.step = 10
        self.slots = []     # (x0, x1, place) for each drawn digit
        self.drag_y = None
        self.dragged = False
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._motion)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Double-Button-1>", lambda e: self.on_type())
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.bind(seq, self._wheel)
        self.bind("<Up>", lambda e: self._bump(self.step))
        self.bind("<Down>", lambda e: self._bump(-self.step))
        self.bind("<Left>", lambda e: self._move_step(10))
        self.bind("<Right>", lambda e: self._move_step(0.1))
        self.draw()

    def set_hz(self, hz):
        if hz != self.hz:
            self.hz = hz
            self.draw()

    def draw(self):
        self.delete("all")
        self.slots = []
        if self.hz is None:
            self.create_text(4, 38, text="--.---.---", fill=DIM, font=self.font, anchor="sw")
            self.configure(width=200)
            return
        text = str(self.hz)
        x = 2
        for i, ch in enumerate(text):
            place = 10 ** (len(text) - 1 - i)
            if i > 0 and (len(text) - i) % 3 == 0:
                self.create_text(x + self.SEP_W / 2, 38, text=".", fill=MUTED,
                                 font=self.font, anchor="s")
                x += self.SEP_W
            sel = place == self.step
            self.create_text(x + self.DIGIT_W / 2, 38, text=ch, fill=AMBER if sel else TEXT,
                             font=self.font, anchor="s")
            if sel:
                self.create_rectangle(x + 2, 40, x + self.DIGIT_W - 2, 43, fill=AMBER, outline="")
            self.slots.append((x, x + self.DIGIT_W, place))
            x += self.DIGIT_W
        self.configure(width=x + 4)

    def _place_at(self, x):
        for x0, x1, place in self.slots:
            if x0 - 2 <= x < x1 + 2:
                return place
        return None

    def _bump(self, delta):
        if self.hz is None:
            return
        hz = max(FREQ_MIN_HZ, min(FREQ_MAX_HZ, self.hz + delta))
        if hz != self.hz:
            self.set_hz(hz)
            self.on_tune(hz)

    def _move_step(self, factor):
        step = int(self.step * factor)
        if 1 <= step <= 10000000:
            self.step = step
            self.draw()

    def _press(self, event):
        self.focus_set()
        self.drag_y = event.y
        self.dragged = False

    def _motion(self, event):
        place = self._place_at(event.x) or self.step
        if self.drag_y is None:
            return
        dy = self.drag_y - event.y
        if abs(dy) >= self.DRAG_STEP_PX:
            self.dragged = True
            self.drag_y = event.y
            self._bump(place if dy > 0 else -place)

    def _release(self, event):
        if not self.dragged:
            place = self._place_at(event.x)
            if place is not None:
                self.step = place
                self.draw()
        self.drag_y = None

    def _wheel(self, event):
        place = self._place_at(event.x)
        if place is None:
            return "break"
        if getattr(event, "num", None) == 4 or getattr(event, "delta", 0) > 0:
            self._bump(place)
        else:
            self._bump(-place)
        return "break"


class SettingsDialog(tk.Toplevel):
    """Everything set once and rarely changed: the connection, the CW
    filter type, paddle reversal, the TX test tones, the station details
    the macros use, and the macros themselves."""

    def __init__(self, panel):
        super().__init__(panel)
        self.panel = panel
        self.title("maxibitx panel - settings")
        self.configure(bg=GROUND)
        self.geometry(f"{WIN_W - 20}x{WIN_H - 20}+{panel.winfo_rootx() + 10}+{panel.winfo_rooty() + 10}")
        self.transient(panel)
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=6, pady=6)
        nb.add(self.build_radio_tab(nb), text="Radio")
        nb.add(self.build_macro_tab(nb), text="Macros")
        ttk.Button(self, text="Close", command=self.close).place(relx=1.0, x=-8, y=6, anchor="ne")
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh_connection()

    def build_radio_tab(self, nb):
        p = self.panel
        f = ttk.Frame(nb, padding=8)
        conn = ttk.LabelFrame(f, text="Connection", padding=8)
        conn.grid(row=0, column=0, columnspan=2, sticky="ew")
        ttk.Label(conn, text="Host", style="PanelMuted.TLabel").grid(row=0, column=0)
        ttk.Entry(conn, textvariable=p.host_var, width=18).grid(row=0, column=1, padx=6)
        ttk.Label(conn, text="Port", style="PanelMuted.TLabel").grid(row=0, column=2)
        ttk.Entry(conn, textvariable=p.port_var, width=6).grid(row=0, column=3, padx=6)
        self.conn_btn = ttk.Button(conn, text="Connect", command=self.toggle_connection)
        self.conn_btn.grid(row=0, column=4, padx=6)
        self.conn_status = ttk.Label(conn, style="PanelMuted.TLabel", text="")
        self.conn_status.grid(row=1, column=0, columnspan=5, sticky="w", pady=(6, 0))

        cw = ttk.LabelFrame(f, text="CW", padding=8)
        cw.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        ttk.Checkbutton(cw, text="Reverse paddles", variable=p.padrev_var, style="Toolbutton",
                        command=p.on_padrev_toggled).grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(cw, text="Filter type", style="PanelMuted.TLabel").grid(row=1, column=0, sticky="w",
                                                                          pady=(10, 2))
        ttk.Radiobutton(cw, text="FFT", variable=p.fftfilt_var, value=True, style="Toolbutton",
                        command=p.on_fftfilt_toggled, width=8).grid(row=2, column=0, padx=(0, 2))
        ttk.Radiobutton(cw, text="Elliptic", variable=p.fftfilt_var, value=False,
                        style="Toolbutton", command=p.on_fftfilt_toggled,
                        width=8).grid(row=2, column=1)

        tx = ttk.LabelFrame(f, text="TX test - dummy load or low power", padding=8)
        tx.grid(row=1, column=1, sticky="nsew", pady=(8, 0), padx=(8, 0))
        for i, (label, val) in enumerate((("Off", 0), ("1 kHz", 1), ("Two-tone", 2))):
            ttk.Radiobutton(tx, text=label, value=val, variable=p.tone_var, style="Toolbutton",
                            command=p.on_tone_changed, width=8).grid(row=0, column=i, padx=1)
        ttk.Checkbutton(tx, text="Transmit", variable=p.ptt_var, style="Toolbutton",
                        command=p.on_ptt_toggled).grid(row=1, column=0, columnspan=3,
                                                       sticky="ew", pady=(8, 0))
        ttk.Label(tx, text="off after 30 s; two-tone 700 + 1900 Hz",
                  style="PanelMuted.TLabel").grid(row=2, column=0, columnspan=3, sticky="w",
                                                  pady=(6, 0))

        st = ttk.LabelFrame(f, text="Station, for the macros", padding=8)
        st.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        ttk.Label(st, text="My call {MYCALL}", style="PanelMuted.TLabel").grid(row=0, column=0)
        ttk.Entry(st, textvariable=p.mycall_var, width=12).grid(row=0, column=1, padx=6)
        ttk.Label(st, text="Park {PARK}", style="PanelMuted.TLabel").grid(row=0, column=2)
        ttk.Entry(st, textvariable=p.park_var, width=12).grid(row=0, column=3, padx=6)
        f.columnconfigure(0, weight=1)
        f.columnconfigure(1, weight=1)
        return f

    def build_macro_tab(self, nb):
        p = self.panel
        f = ttk.Frame(nb, padding=8)
        ttk.Label(f, text="Button", style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(f, text="Text sent ({MYCALL} and {PARK} are filled in)",
                  style="Muted.TLabel").grid(row=0, column=1, sticky="w", padx=(6, 0))
        for i, (lab_var, text_var) in enumerate(p.macro_vars):
            ttk.Entry(f, textvariable=lab_var, width=9).grid(row=i + 1, column=0, pady=1)
            ttk.Entry(f, textvariable=text_var).grid(row=i + 1, column=1, sticky="ew",
                                                    padx=(6, 0), pady=1)
        f.columnconfigure(1, weight=1)
        return f

    def toggle_connection(self):
        p = self.panel
        if p.client.connected():
            p.want_connection = False
            p.disconnect()
        else:
            p.connect_from_fields()
        self.refresh_connection()

    def refresh_connection(self):
        p = self.panel
        if p.client.connected():
            self.conn_btn.configure(text="Disconnect")
            self.conn_status.configure(text=f"connected to {p.connected_to}")
        else:
            self.conn_btn.configure(text="Connect")
            self.conn_status.configure(text=p.connect_error or "not connected")

    def close(self):
        self.panel.save_settings()
        self.panel.refresh_macro_labels()
        self.panel.settings = None
        self.destroy()


class Panel(tk.Tk):
    def __init__(self, args):
        super().__init__()
        self.title("maxibitx")
        self.configure(bg=GROUND)
        self.ui, self.mono = setup_style(self)
        if args.fullscreen:
            self.attributes("-fullscreen", True)
        else:
            self.geometry(f"{WIN_W}x{WIN_H}")
        self.minsize(WIN_W, WIN_H)

        self.client = RigctlClient()
        self.spectrum = SpectrumClient()
        self.poll_stop = threading.Event()
        self.spectrum_running = False
        self.settings = None
        self.connected_to = None
        self.connect_error = None
        self.want_connection = True
        self.connecting = False

        self.current_freq_hz = None
        self.freq_hold_until = 0.0      # ignore polled frequency until then
        self.freq_target = None         # latest frequency to send
        self.freq_sending = False
        self.rit_hz = 0
        self.rit_hold_until = 0.0
        self.mode = None
        self.status_message = None
        self.status_until = 0.0

        # Set while a poll reply is being written into a control, so the
        # control's own handler doesn't send it back to the radio.
        self.syncing = False

        cfg = load_config()
        self.cfg = cfg
        self.host_var = tk.StringVar(value=args.host or cfg.get("host", DEFAULT_HOST))
        self.port_var = tk.StringVar(value=str(args.port or cfg.get("port", DEFAULT_PORT)))
        self.mycall_var = tk.StringVar(value=cfg.get("mycall", ""))
        self.park_var = tk.StringVar(value=cfg.get("park", ""))
        macros = cfg.get("macros") or [list(m) for m in DEFAULT_MACROS]
        macros = (list(macros) + [list(m) for m in DEFAULT_MACROS])[:len(DEFAULT_MACROS)]
        self.macro_vars = [(tk.StringVar(value=lab), tk.StringVar(value=txt)) for lab, txt in macros]
        self.band_memory = dict(cfg.get("band_memory", {}))

        self.mode_var = tk.StringVar(value="CW")
        self.ritmode_var = tk.StringVar(value="RIT")
        self.vol_var = tk.DoubleVar(value=50)
        self.narrow_var = tk.BooleanVar(value=True)
        self.fftfilt_var = tk.BooleanVar(value=True)
        self.pitch_var = tk.StringVar(value="700")
        self.width_var = tk.StringVar(value="300")
        self.apf_var = tk.BooleanVar(value=False)
        self.nr_var = tk.BooleanVar(value=False)
        self.span_var = tk.StringVar(value=self.span_label(SPECTRUM_DEFAULT_HALF_SPAN_HZ))
        self.micgain_var = tk.DoubleVar(value=1.0)
        self.keyer_var = tk.StringVar(value=KEYER_MODES[4])
        self.wpm_var = tk.StringVar(value="20")
        self.wpm_focused = False
        self.cw_text_var = tk.StringVar(value="")
        self.power_var = tk.DoubleVar(value=100.0)
        self.tune_var = tk.BooleanVar(value=False)
        self.tone_var = tk.IntVar(value=0)
        self.ptt_var = tk.BooleanVar(value=False)
        self.padrev_var = tk.BooleanVar(value=False)
        self.band_var = tk.StringVar(value="")

        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)
        self.build_top()
        self.build_controls()
        self.build_spectrum()
        self.build_mode_row()
        self.build_bottom()
        self.block_wheel_on_values()
        self.show_mode_row("CW")

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(100, self.connect_from_fields)
        self.after(RECONNECT_INTERVAL_MS, self.reconnect_tick)

    # ---- layout ----

    def build_top(self):
        top = ttk.Frame(self, padding=(6, 6, 6, 0))
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)
        self.band_combo = ttk.Combobox(top, textvariable=self.band_var, width=5, state="readonly",
                                       values=[b[0] for b in BANDS], font=(self.ui, 13, "bold"))
        self.band_combo.grid(row=0, column=0, padx=(0, 8))
        self.band_combo.bind("<<ComboboxSelected>>", self.on_band_selected)
        f = ttk.Frame(top)
        f.grid(row=0, column=1, sticky="w")
        self.freq_display = FreqDisplay(f, self.mono, self.on_freq_tuned, self.on_freq_type)
        self.freq_display.grid(row=0, column=0)
        ttk.Label(f, text="MHz", style="Muted.TLabel").grid(row=0, column=1, sticky="s", pady=(0, 6))
        modes = ttk.Frame(top)
        modes.grid(row=0, column=2, padx=6)
        self.mode_buttons = []
        for i, m in enumerate(MODES):
            b = ttk.Radiobutton(modes, text=m, value=m, variable=self.mode_var, style="Toolbutton",
                                width=4, command=self.on_mode_changed)
            b.grid(row=0, column=i)
            self.mode_buttons.append(b)
        self.rx_button = ttk.Button(top, text="OFF", style="Off.TButton", width=4,
                                    command=self.open_settings)
        self.rx_button.grid(row=0, column=3, padx=6)
        vol = ttk.Frame(top)
        vol.grid(row=0, column=4)
        ttk.Label(vol, text="VOL", style="Muted.TLabel").grid(row=0, column=0, padx=(0, 4))
        self.vol_scale = ttk.Scale(vol, from_=0, to=100, variable=self.vol_var, length=64,
                                   command=lambda v: self.vol_label.configure(
                                       text=f"{round(float(v))}"))
        self.vol_scale.grid(row=0, column=1)
        self.vol_scale.bind("<ButtonRelease-1>", self.on_volume_released)
        self.vol_label = ttk.Label(vol, text="--", style="Mono.TLabel", width=3)
        self.vol_label.grid(row=0, column=2, padx=(4, 0))

    def build_controls(self):
        row = ttk.Frame(self, padding=(6, 4, 6, 0))
        row.grid(row=1, column=0, sticky="ew")
        row.columnconfigure(1, weight=1)
        rit = ttk.LabelFrame(row, text="RIT", padding=(3, 0, 3, 4))
        rit.grid(row=0, column=0, sticky="nsw")
        self.rit_frame = rit
        ttk.Radiobutton(rit, text="RIT", value="RIT", variable=self.ritmode_var,
                        style="Toolbutton", width=3).grid(row=0, column=0, padx=1)
        xit = ttk.Radiobutton(rit, text="XIT", value="XIT", variable=self.ritmode_var,
                              style="Toolbutton", width=3)
        xit.grid(row=0, column=1, padx=1)
        xit.state(["disabled"])    # maxibitx has no XIT yet
        for i, (t, d) in enumerate((("−100", -100), ("−10", -10))):
            ttk.Button(rit, text=t, style="Step.TButton", width=4,
                       command=lambda d=d: self.on_rit_step(d)).grid(row=0, column=2 + i, padx=1)
        self.rit_value = ttk.Button(rit, text="0 Hz", style="Value.TButton", width=7,
                                    command=lambda: self.send_rit(0))
        self.rit_value.grid(row=0, column=4, padx=2, sticky="ns")
        for i, (t, d) in enumerate((("+10", 10), ("+100", 100))):
            ttk.Button(rit, text=t, style="Step.TButton", width=4,
                       command=lambda d=d: self.on_rit_step(d)).grid(row=0, column=5 + i, padx=1)

        flt = ttk.LabelFrame(row, text="FILTER", padding=(6, 0, 6, 4))
        flt.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        self.filter_frame = flt
        self.narrow_check = ttk.Checkbutton(flt, text="ON", variable=self.narrow_var,
                                            style="Toolbutton", width=3,
                                            command=self.on_narrow_toggled)
        self.narrow_check.grid(row=0, column=0, padx=(0, 6))
        ttk.Label(flt, text="WIDTH", style="PanelMuted.TLabel").grid(row=0, column=1)
        self.width_combo = ttk.Combobox(flt, textvariable=self.width_var, values=CW_WIDTHS,
                                        width=3, state="readonly", font=(self.mono, 13))
        self.width_combo.grid(row=0, column=2, padx=(3, 6))
        self.width_combo.bind("<<ComboboxSelected>>", self.on_width_selected)
        ttk.Label(flt, text="CENTER", style="PanelMuted.TLabel").grid(row=0, column=3)
        self.pitch_combo = ttk.Combobox(flt, textvariable=self.pitch_var, values=CW_PITCHES,
                                        width=4, state="readonly", font=(self.mono, 13))
        self.pitch_combo.grid(row=0, column=4, padx=(3, 6))
        self.pitch_combo.bind("<<ComboboxSelected>>", self.on_pitch_selected)
        self.apf_check = ttk.Checkbutton(flt, text="APF", variable=self.apf_var,
                                         style="Toolbutton", width=3)
        self.apf_check.grid(row=0, column=5, padx=(0, 6))
        self.nr_check = ttk.Checkbutton(flt, text="NR", variable=self.nr_var,
                                        style="Toolbutton", width=3)
        self.nr_check.grid(row=0, column=6)
        # maxibitx has no APF or NR yet
        self.apf_check.state(["disabled"])
        self.nr_check.state(["disabled"])

    def build_spectrum(self):
        spec = ttk.Frame(self, padding=(6, 6, 6, 0))
        spec.grid(row=2, column=0, sticky="nsew")
        spec.columnconfigure(0, weight=1)
        spec.rowconfigure(1, weight=1)
        bar = ttk.Frame(spec)
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        bar.columnconfigure(2, weight=1)
        ttk.Label(bar, text="SPAN", style="Muted.TLabel").grid(row=0, column=0, padx=(0, 4))
        self.span_combo = ttk.Combobox(bar, textvariable=self.span_var, width=6, state="readonly",
                                       values=[self.span_label(h) for h in SPECTRUM_SPAN_CHOICES_HZ],
                                       font=(self.ui, 12, "bold"))
        self.span_combo.grid(row=0, column=1)
        self.span_combo.bind("<<ComboboxSelected>>", lambda e: self.clear_waterfall())
        self.status_label = ttk.Label(bar, text="", style="Status.TLabel", anchor="center",
                                      width=1)
        self.status_label.grid(row=0, column=2, sticky="ew", padx=8)
        ttk.Label(bar, text="SIGNAL", style="Muted.TLabel").grid(row=0, column=3)
        self.smeter = tk.Canvas(bar, width=152, height=14, bg=GROUND, highlightthickness=0)
        self.smeter.grid(row=0, column=4, padx=6)
        self.smeter_label = ttk.Label(bar, text="--", style="Mono.TLabel", width=5, anchor="e")
        self.smeter_label.grid(row=0, column=5)
        self.draw_smeter(None)

        self.canvas = tk.Canvas(spec, bg="#08131C", highlightthickness=0, height=120)
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.canvas_w, self.canvas_h = 0, 0
        self.waterfall_palette = waterfall_palette()
        self.waterfall_rows = np.zeros((1, 1, 3), np.uint8)
        self.waterfall_image = None
        self.canvas.bind("<Configure>", self.on_canvas_resized)

    def build_mode_row(self):
        holder = ttk.Frame(self, padding=(6, 6, 6, 0))
        holder.grid(row=3, column=0, sticky="ew")
        holder.columnconfigure(0, weight=1)
        self.mode_rows = {}

        cw = ttk.Frame(holder)
        self.macro_buttons = []
        for i in range(len(DEFAULT_MACROS)):
            cw.columnconfigure(i, weight=1, uniform="m")
            b = ttk.Button(cw, text="", command=lambda i=i: self.send_macro(i))
            b.grid(row=0, column=i, sticky="ew", padx=1)
            self.macro_buttons.append(b)
        self.refresh_macro_labels()
        self.mode_rows["cw"] = cw

        voice = ttk.Frame(holder)
        voice.columnconfigure(1, weight=1)
        voice.columnconfigure(4, weight=1)
        ttk.Label(voice, text="MIC", style="Muted.TLabel").grid(row=0, column=0, padx=(0, 4))
        self.micgain_scale = ttk.Scale(voice, from_=0.0, to=8.0, variable=self.micgain_var,
                                       command=lambda v: self.micgain_label.configure(
                                           text=f"{float(v):.1f}×"))
        self.micgain_scale.grid(row=0, column=1, sticky="ew")
        self.micgain_scale.bind("<ButtonRelease-1>", self.on_micgain_released)
        self.micgain_label = ttk.Label(voice, text="--", style="Mono.TLabel", width=5)
        self.micgain_label.grid(row=0, column=2, padx=(4, 12))
        ttk.Label(voice, text="ALC", style="Muted.TLabel").grid(row=0, column=3, padx=(0, 4))
        self.alc_bar = ttk.Progressbar(voice, orient="horizontal", mode="determinate",
                                       maximum=ALC_METER_MAX_DB)
        self.alc_bar.grid(row=0, column=4, sticky="ew", ipady=8)
        self.alc_label = ttk.Label(voice, text="--", style="Mono.TLabel", width=7, anchor="e")
        self.alc_label.grid(row=0, column=5, padx=(4, 0), ipady=10)
        self.mode_rows["voice"] = voice

        digi = ttk.Frame(holder)
        ttk.Label(digi, text="DIGI: transmit audio and PTT come from the digital-mode program "
                             "(WSJT-X, JTDX or a TCI client).",
                  style="Muted.TLabel").grid(row=0, column=0, sticky="w", ipady=12)
        self.mode_rows["digi"] = digi

    def build_bottom(self):
        b = ttk.Frame(self, padding=6)
        b.grid(row=4, column=0, sticky="ew")
        b.columnconfigure(3, weight=1)
        self.keyer_combo = ttk.Combobox(b, textvariable=self.keyer_var, values=KEYER_MODES,
                                        width=8, state="readonly", font=(self.ui, 12, "bold"))
        self.keyer_combo.grid(row=0, column=0, padx=(0, 6))
        self.keyer_combo.bind("<<ComboboxSelected>>", self.on_keyer_selected)
        ttk.Label(b, text="WPM", style="Muted.TLabel").grid(row=0, column=1)
        self.wpm_spin = ttk.Spinbox(b, from_=1, to=60, width=2, textvariable=self.wpm_var,
                                    font=(self.mono, 13, "bold"), command=self.on_wpm_changed)
        self.wpm_spin.grid(row=0, column=2, padx=(3, 6))
        self.wpm_spin.bind("<Return>", lambda e: self.on_wpm_changed())
        self.wpm_spin.bind("<FocusIn>", lambda e: setattr(self, "wpm_focused", True))
        self.wpm_spin.bind("<FocusOut>", self.on_wpm_focus_out)
        self.cw_entry = ttk.Entry(b, textvariable=self.cw_text_var, font=(self.mono, 13))
        self.cw_entry.grid(row=0, column=3, sticky="ew", padx=(0, 6))
        self.cw_entry.bind("<Return>", lambda e: self.on_cw_send())
        self.send_button = ttk.Button(b, text="SEND", style="Accent.TButton", width=5,
                                      command=self.on_cw_send)
        self.send_button.grid(row=0, column=4)
        self.stop_button = ttk.Button(b, text="STOP", width=5, command=self.on_cw_stop)
        self.stop_button.grid(row=0, column=5, padx=(4, 6))
        ttk.Label(b, text="PWR", style="Muted.TLabel").grid(row=0, column=6)
        self.power_scale = ttk.Scale(b, from_=0, to=100, variable=self.power_var, length=64,
                                     command=lambda v: self.power_label.configure(
                                         text=f"{round(float(v))}"))
        self.power_scale.grid(row=0, column=7, padx=3)
        self.power_scale.bind("<ButtonRelease-1>", self.on_power_released)
        self.power_label = ttk.Label(b, text="--", style="Mono.TLabel", width=3)
        self.power_label.grid(row=0, column=8, padx=(0, 6))
        ttk.Checkbutton(b, text="TUNE", variable=self.tune_var, style="Toolbutton", width=5,
                        command=self.on_tune_toggled).grid(row=0, column=9)

    def block_wheel_on_values(self):
        """The wheel over a menu, slider or the WPM box does nothing, so
        a scroll meant for the frequency can't change a setting on the
        way past."""
        for w in (self.band_combo, self.width_combo, self.pitch_combo,
                  self.span_combo, self.keyer_combo, self.vol_scale, self.power_scale,
                  self.micgain_scale, self.wpm_spin):
            for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                w.bind(seq, lambda e: "break")

    def refresh_macro_labels(self):
        for b, (lab_var, _t) in zip(self.macro_buttons, self.macro_vars):
            b.configure(text=lab_var.get() or "-")

    def show_mode_row(self, mode_name):
        key = "cw" if mode_name in ("CW", "CWR") else ("voice" if mode_name in ("USB", "LSB")
                                                         else "digi")
        for k, frame in self.mode_rows.items():
            if k == key:
                frame.grid(row=0, column=0, sticky="ew")
            else:
                frame.grid_remove()
        cw_state = ["!disabled"] if key == "cw" else ["disabled"]
        for w in (self.cw_entry, self.send_button, self.stop_button):
            w.state(cw_state)
        # The radio holds the CW filter out of circuit in DIGITAL whatever
        # it is set to; the controls say so rather than look live.
        gated = key == "digi"
        self.narrow_check.state(["disabled"] if gated else ["!disabled"])
        for combo in (self.width_combo, self.pitch_combo):
            combo.configure(state="disabled" if gated else "readonly")
        self.filter_frame.configure(text="FILTER - out of circuit in DIGI" if gated else "FILTER")

    # ---- status line ----

    def say(self, text, seconds=4.0):
        """Show text in the status line for a few seconds, ahead of the
        spectrum's own readout."""
        self.status_message = text
        self.status_until = time.monotonic() + seconds
        self.status_label.configure(text=text)

    # ---- connection ----

    def connect_from_fields(self):
        host = self.host_var.get().strip() or DEFAULT_HOST
        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            self.connect_error = "the port must be a number"
            self.refresh_settings()
            return
        self.want_connection = True
        self.connect(host, port)

    def connect(self, host, port):
        """Connect on a background thread, so an unreachable host can't
        freeze the screen for the socket timeout."""
        if self.client.connected() or self.connecting:
            return
        self.connecting = True

        def run():
            try:
                self.client.connect(host, port)
                error = None
            except OSError as e:
                error = f"couldn't connect to {host}:{port} - {e}"
            self.after(0, lambda: self.on_connect_result(host, port, error))
        threading.Thread(target=run, daemon=True).start()

    def on_connect_result(self, host, port, error):
        self.connecting = False
        if error is not None:
            self.connect_error = error
            self.say("not connected - tap OFF for settings", 6)
            self.refresh_settings()
            return
        self.connect_error = None
        self.connected_to = f"{host}:{port}"
        self.cfg.update({"host": host, "port": port})
        save_config(self.cfg)
        self.say(f"connected to {host}:{port}")
        self.rx_button.configure(text="RX", style="RX.TButton")
        self.poll_stop.clear()
        threading.Thread(target=self.poll_loop, daemon=True).start()
        self.spectrum.start(host)
        if not self.spectrum_running:
            self.spectrum_running = True
            self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)
        self.refresh_settings()

    def disconnect(self):
        self.poll_stop.set()
        self.client.disconnect()
        self.spectrum.stop()
        self.spectrum_running = False
        self.current_freq_hz = None
        self.freq_display.set_hz(None)
        self.canvas.delete("trace", "scale")
        self.clear_waterfall()
        self.draw_smeter(None)
        self.rx_button.configure(text="OFF", style="Off.TButton")
        self.say("disconnected", 6)
        self.refresh_settings()

    def reconnect_tick(self):
        """While the panel wants a connection and hasn't one, try again
        every few seconds, so a restarted maxibitx comes back on its own."""
        if self.want_connection and not self.client.connected():
            self.connect_from_fields()
        self.after(RECONNECT_INTERVAL_MS, self.reconnect_tick)

    def on_close(self):
        self.save_settings()
        self.want_connection = False
        self.disconnect()
        self.destroy()

    # ---- settings ----

    def open_settings(self):
        if self.settings is None:
            self.settings = SettingsDialog(self)
        else:
            self.settings.lift()

    def refresh_settings(self):
        if self.settings is not None:
            self.settings.refresh_connection()

    def save_settings(self):
        self.cfg.update({
            "mycall": self.mycall_var.get().strip(),
            "park": self.park_var.get().strip(),
            "macros": [[lab.get(), txt.get()] for lab, txt in self.macro_vars],
            "band_memory": self.band_memory,
        })
        save_config(self.cfg)

    # ---- polling ----
    #
    # The poll runs on its own thread, so a stalled connection can't freeze
    # the screen; every control update it makes is handed to the Tk thread
    # with self.after().

    POLL_COMMANDS = ("f", "j", "l AF", "l MICGAIN", "m", "u NARROW", "u FFTFILT", "l CWPITCH",
                     "l CWWIDTH", "l STRENGTH", "u TONE", "t", "l RFPOWER", "l ALC", "u KEYER",
                     "l KEYSPD", "u PADREV", "u MORSE")

    def poll_loop(self):
        while not self.poll_stop.is_set():
            if not self.client.connected():
                self.after(0, self.disconnect)
                return
            replies = {}
            for cmd in self.POLL_COMMANDS:
                r = self.client.query(cmd)
                if r is None:
                    self.after(0, self.disconnect)
                    return
                replies[cmd] = r
            self.after(0, lambda r=replies: self.apply_poll(r))
            time.sleep(POLL_INTERVAL_S)

    def apply_poll(self, r):
        if not self.client.connected():
            return
        self.syncing = True
        try:
            self.apply_freq(r["f"])
            self.apply_rit(r["j"])
            self.apply_float(r["l AF"], lambda v: (self.vol_var.set(v * 100),
                                                   self.vol_label.configure(text=f"{round(v * 100)}")))
            self.apply_float(r["l MICGAIN"], lambda v: (self.micgain_var.set(v),
                                                        self.micgain_label.configure(
                                                            text=f"{v:.1f}×")))
            self.apply_mode(r["m"])
            self.apply_int(r["u NARROW"], lambda v: self.narrow_var.set(v != 0))
            self.apply_int(r["u FFTFILT"], lambda v: self.fftfilt_var.set(v != 0))
            self.apply_choice(r["l CWPITCH"], self.pitch_combo, self.pitch_var)
            self.apply_choice(r["l CWWIDTH"], self.width_combo, self.width_var)
            self.apply_int(r["l STRENGTH"], self.draw_smeter)
            tone = self.apply_int(r["u TONE"], self.tone_var.set)
            ptt = self.apply_int(r["t"], lambda v: self.ptt_var.set(v != 0))
            self.apply_tx(ptt, tone)
            self.apply_float(r["l RFPOWER"], lambda v: (self.power_var.set(v * 100),
                                                        self.power_label.configure(
                                                            text=f"{round(v * 100)}")))
            self.apply_float(r["l ALC"], lambda v: (self.alc_bar.configure(
                value=min(v, ALC_METER_MAX_DB)), self.alc_label.configure(text=f"{v:.1f} dB")))
            self.apply_int(r["u KEYER"], lambda v: self.keyer_var.set(KEYER_MODES[v])
                           if 0 <= v < len(KEYER_MODES) else None)
            if not self.wpm_focused:
                self.apply_int(r["l KEYSPD"], lambda v: self.wpm_var.set(str(v)))
            self.apply_int(r["u PADREV"], lambda v: self.padrev_var.set(v != 0))
            self.apply_int(r["u MORSE"], self.apply_morse)
        finally:
            self.syncing = False

    @staticmethod
    def apply_int(reply, setter):
        """Parse an integer reply and pass it on; a reply that isn't one
        (an older maxibitx answers RPRT -1 to what it lacks) is ignored.
        Returns the value, or None."""
        try:
            v = int(float(reply))
        except ValueError:
            return None
        setter(v)
        return v

    @staticmethod
    def apply_float(reply, setter):
        try:
            v = float(reply)
        except ValueError:
            return None
        setter(v)
        return v

    def apply_freq(self, reply):
        try:
            hz = int(reply)
        except ValueError:
            return
        if time.monotonic() < self.freq_hold_until or self.freq_sending:
            return   # a frequency the operator just set is still on its way
        self.current_freq_hz = hz
        self.freq_display.set_hz(hz)
        band = band_of(hz)
        self.band_var.set(band[0] if band else "")

    def apply_rit(self, reply):
        try:
            hz = int(reply)
        except ValueError:
            return
        if time.monotonic() < self.rit_hold_until:
            return
        self.rit_hz = hz
        self.show_rit()

    def show_rit(self):
        self.rit_value.configure(text=f"{self.rit_hz:+d} Hz" if self.rit_hz else "0 Hz")

    def apply_mode(self, reply):
        # "m" answers two lines, the mode and a passband; only the first
        # matters. DIGITAL reads back as PKTUSB, the Hamlib name.
        name = reply.split("\n")[0].strip()
        name = {"PKTUSB": "DIGI", "DIGITAL": "DIGI"}.get(name, name)
        if name not in MODES:
            return
        self.mode_var.set(name)
        if name != self.mode:
            self.mode = name
            self.show_mode_row(name)

    def apply_choice(self, reply, combo, var):
        """Pitch and width readback. The radio owns the list of valid
        values; one this menu lacks is added to it rather than dropped."""
        try:
            hz = str(int(float(reply)))
        except ValueError:
            return
        values = list(combo.cget("values"))
        if hz not in values:
            combo.configure(values=tuple(sorted(values + [hz], key=int)))
        var.set(hz)

    def apply_tx(self, ptt, tone):
        if ptt is None:
            return
        if ptt:
            self.rx_button.configure(text="TX", style="TX.TButton")
        else:
            self.rx_button.configure(text="RX", style="RX.TButton")
        self.tune_var.set(bool(ptt) and tone == 1)

    def apply_morse(self, busy):
        if busy:
            self.say("sending CW", 1.5)

    def draw_smeter(self, db):
        c = self.smeter
        c.delete("all")
        for i, threshold in enumerate(SMETER_SEGMENTS_DB):
            lit = db is not None and db >= threshold
            colour = (TRACE if threshold <= 0 else AMBER) if lit else "#23323A"
            c.create_rectangle(2 + i * 10, 2, 10 + i * 10, 12, fill=colour, outline="")
        self.smeter_label.configure(text=s_unit_label(db) if db is not None else "--")

    # ---- actions ----

    def query_async(self, command, then=None):
        """Send one command off the Tk thread; then(reply) runs back on it."""
        if not self.client.connected():
            return

        def run():
            reply = self.client.query(command)
            if then is not None:
                self.after(0, lambda: then(reply))
        threading.Thread(target=run, daemon=True).start()

    def on_freq_tuned(self, hz):
        """Frequency changes from the digits arrive faster than they can
        be sent; only the latest is sent, one at a time, in order."""
        self.current_freq_hz = hz
        self.freq_hold_until = time.monotonic() + 1.5
        band = band_of(hz)
        self.band_var.set(band[0] if band else "")
        self.freq_target = hz
        if self.freq_sending or not self.client.connected():
            return
        self.freq_sending = True

        def run():
            sent = None
            while self.freq_target != sent:
                sent = self.freq_target
                if self.client.query(f"F {sent}") is None:
                    break
            self.freq_sending = False
        threading.Thread(target=run, daemon=True).start()

    def on_freq_type(self):
        """Double-click on the frequency: type one in (Hz, kHz or MHz)."""
        top = tk.Toplevel(self)
        top.title("Frequency")
        top.configure(bg=GROUND)
        top.transient(self)
        var = tk.StringVar(value=str(self.current_freq_hz or ""))
        ttk.Label(top, text="Frequency: 14058.2 (kHz), 14.0582 (MHz) or 14058200 (Hz)",
                  style="Muted.TLabel").pack(padx=10, pady=(10, 4))
        entry = ttk.Entry(top, textvariable=var, font=(self.mono, 16))
        entry.pack(fill="x", padx=10)
        msg = ttk.Label(top, text="", style="Muted.TLabel")
        msg.pack(padx=10, pady=4)

        def go(_e=None):
            hz = parse_frequency(var.get())
            if hz is None:
                msg.configure(text="not a frequency between 0.1 and 30 MHz")
                return
            self.freq_display.set_hz(hz)
            self.on_freq_tuned(hz)
            top.destroy()
        entry.bind("<Return>", go)
        entry.bind("<Escape>", lambda e: top.destroy())
        bar = ttk.Frame(top, padding=10)
        bar.pack(fill="x")
        ttk.Button(bar, text="Tune", style="Accent.TButton", command=go).pack(side="right")
        ttk.Button(bar, text="Cancel", command=top.destroy).pack(side="right", padx=6)
        entry.focus_set()
        entry.select_range(0, "end")

    def on_band_selected(self, _event=None):
        name = self.band_var.get()
        band = next((b for b in BANDS if b[0] == name), None)
        if band is None:
            return
        old = band_of(self.current_freq_hz) if self.current_freq_hz else None
        if old is not None:
            self.band_memory[old[0]] = self.current_freq_hz
        hz = int(self.band_memory.get(name, band[3]))
        self.freq_display.set_hz(hz)
        self.on_freq_tuned(hz)
        self.save_settings()

    def on_mode_changed(self):
        name = self.mode_var.get()
        self.mode = name
        self.show_mode_row(name)
        self.query_async(f"M {'DIGITAL' if name == 'DIGI' else name} 2400")

    def on_rit_step(self, delta):
        self.send_rit(self.rit_hz + delta)

    def send_rit(self, hz):
        hz = max(-RIT_MAX_HZ, min(RIT_MAX_HZ, hz))
        self.rit_hz = hz
        self.rit_hold_until = time.monotonic() + 1.5
        self.show_rit()
        self.query_async(f"J {hz}")

    def on_volume_released(self, _event=None):
        self.query_async(f"L AF {self.vol_var.get() / 100.0:.3f}")

    def on_micgain_released(self, _event=None):
        self.query_async(f"L MICGAIN {self.micgain_var.get():.3f}")

    def on_power_released(self, _event=None):
        self.query_async(f"L RFPOWER {self.power_var.get() / 100.0:.3f}")

    def on_narrow_toggled(self):
        if not self.syncing:
            self.query_async(f"U NARROW {1 if self.narrow_var.get() else 0}")

    def on_fftfilt_toggled(self):
        if not self.syncing:
            self.query_async(f"U FFTFILT {1 if self.fftfilt_var.get() else 0}")

    def on_pitch_selected(self, _event=None):
        self.query_async(f"L CWPITCH {self.pitch_var.get()}")

    def on_width_selected(self, _event=None):
        self.query_async(f"L CWWIDTH {self.width_var.get()}")

    def on_keyer_selected(self, _event=None):
        self.query_async(f"U KEYER {KEYER_MODES.index(self.keyer_var.get())}")

    def on_wpm_changed(self):
        try:
            wpm = max(1, min(60, int(float(self.wpm_var.get()))))
        except ValueError:
            return   # the next poll puts the radio's value back
        self.query_async(f"L KEYSPD {wpm}")

    def on_wpm_focus_out(self, _event=None):
        self.wpm_focused = False
        self.on_wpm_changed()

    def on_padrev_toggled(self):
        if not self.syncing:
            self.query_async(f"U PADREV {1 if self.padrev_var.get() else 0}")

    def on_tone_changed(self):
        if not self.syncing:
            self.query_async(f"U TONE {self.tone_var.get()}")

    def on_ptt_toggled(self):
        if not self.syncing:
            self.query_async(f"T {1 if self.ptt_var.get() else 0}")

    def on_tune_toggled(self):
        """TUNE: the 1 kHz test tone, keyed. maxibitx drops it after 30 s."""
        if self.tune_var.get():
            self.query_async("U TONE 1", then=lambda r: self.query_async("T 1"))
        else:
            self.query_async("T 0", then=lambda r: self.query_async("U TONE 0"))

    def macro_text(self, i):
        text = self.macro_vars[i][1].get()
        return (text.replace("{MYCALL}", self.mycall_var.get().strip())
                    .replace("{PARK}", self.park_var.get().strip()))

    def send_macro(self, i):
        raw = self.macro_vars[i][1].get()
        for field, var, name in (("{MYCALL}", self.mycall_var, "My call"),
                                 ("{PARK}", self.park_var, "Park")):
            if field in raw and not var.get().strip():
                self.say(f"set {name} in Settings first (tap RX)")
                return
        self.send_cw(self.macro_text(i))

    def on_cw_send(self):
        self.send_cw(self.cw_text_var.get())

    def send_cw(self, text):
        text = " ".join(text.split())
        if not text:
            return

        def done(reply):
            if reply is not None and reply.strip() != "RPRT 0":
                self.say("CW text refused - CW or CWR only, or the queue is full")
            elif reply is not None:
                self.say("sending CW", 1.5)
        self.query_async(f"b {text}", then=done)

    def on_cw_stop(self):
        self.query_async("\\stop_morse")

    # ---- spectrum and waterfall ----

    @staticmethod
    def span_label(half_hz):
        """A half-span as the menu shows it: the whole span, in kHz."""
        whole = 2 * half_hz / 1000.0
        return f"{whole:g} kHz"

    def half_span(self):
        for h in SPECTRUM_SPAN_CHOICES_HZ:
            if self.span_label(h) == self.span_var.get():
                return h
        return SPECTRUM_DEFAULT_HALF_SPAN_HZ

    def on_canvas_resized(self, event):
        w, h = event.width, event.height
        if (w, h) == (self.canvas_w, self.canvas_h):
            return
        self.canvas_w, self.canvas_h = w, h
        rows = max(1, h - TRACE_H - SCALE_H)
        self.waterfall_rows = np.zeros((rows, max(1, w), 3), np.uint8)
        self.waterfall_rows[:] = WATERFALL_STOPS[0][1]
        self.canvas.delete("all")
        self.waterfall_image = tk.PhotoImage(width=max(1, w), height=rows)
        self.canvas.create_image(0, TRACE_H + SCALE_H, image=self.waterfall_image, anchor="nw",
                                 tags="waterfall")
        self.canvas.create_rectangle(0, 0, w, TRACE_H, fill="#0B1720", outline="", tags="bg")
        self.canvas.create_rectangle(0, TRACE_H, w, TRACE_H + SCALE_H, fill="#101B22",
                                     outline="", tags="bg")
        self.canvas.create_line(w / 2, TRACE_H + SCALE_H, w / 2, h, fill=AMBER, dash=(2, 6),
                                tags="dialwf")
        self.show_waterfall()

    def clear_waterfall(self):
        self.waterfall_rows[:] = WATERFALL_STOPS[0][1]
        self.show_waterfall()

    def show_waterfall(self):
        if self.waterfall_image is None:
            return
        rows = self.waterfall_rows
        header = b"P6 %d %d 255\n" % (rows.shape[1], rows.shape[0])
        self.waterfall_image.configure(data=header + rows.tobytes(), format="PPM")

    def add_waterfall_row(self, frac):
        """frac: the displayed bins, 0 (SPECTRUM_DB_FLOOR) to 1 (ceiling).
        Each pixel shows the strongest bin under it, so a narrow CW signal
        isn't averaged away at the wide span."""
        w = self.waterfall_rows.shape[1]
        edges = (np.arange(w) * len(frac)) // w
        row = np.maximum.reduceat(frac, edges)
        self.waterfall_rows[1:] = self.waterfall_rows[:-1]
        self.waterfall_rows[0] = self.waterfall_palette[(row * 255.0).astype(np.intp)]
        self.show_waterfall()

    def draw_scale(self, w, span_hz):
        """The frequency scale between the trace and the waterfall: a tick
        every minor step, a label at every major one, the dial marked."""
        c = self.canvas
        c.delete("scale")
        y0 = TRACE_H
        dial = self.current_freq_hz
        if dial is None:
            return
        major, minor = SCALE_TICKS_HZ.get(int(round(span_hz / 500.0)) * 500,
                                          SCALE_TICKS_HZ[SPECTRUM_DEFAULT_HALF_SPAN_HZ])
        lo, hi = dial - span_hz, dial + span_hz
        k = -(-lo // minor) * minor
        while k <= hi:
            x = (k - lo) / (hi - lo) * w
            is_major = k % major == 0
            c.create_line(x, y0, x, y0 + (6 if is_major else 3),
                          fill=MUTED if is_major else "#3A4A51", tags="scale")
            if is_major and 24 < x < w - 24:
                c.create_text(x, y0 + SCALE_H - 1, text=f"{k / 1e6:.3f}", fill=MUTED,
                              font=(self.mono, 9), anchor="s", tags="scale")
            k += minor
        c.create_polygon(w / 2 - 4, y0, w / 2 + 4, y0, w / 2, y0 + 5, fill=AMBER, outline="",
                         tags="scale")

    def redraw_spectrum(self):
        if not self.spectrum_running:
            return
        if not self.client.connected():
            self.spectrum_running = False
            return
        c = self.canvas
        w = self.canvas_w
        db = self.spectrum.get_latest()
        c.delete("trace")
        readout = ""
        if db is None or w < 2:
            readout = f"waiting for I/Q on UDP {IQ_STREAM_PORT}"
        else:
            # db covers the full +-48 kHz (bin FFT_SIZE/2 is the dial);
            # the display shows the middle +-span.
            bin_hz = 96000.0 / FFT_SIZE
            center = len(db) // 2
            half_bins = min(max(int(round(self.half_span() / bin_hz)), 2), center)
            db = db[center - half_bins:center + half_bins]
            n = len(db)
            peak_db = float(np.max(db))
            frac = (np.clip(db, SPECTRUM_DB_FLOOR, SPECTRUM_DB_CEILING) - SPECTRUM_DB_FLOOR) \
                / (SPECTRUM_DB_CEILING - SPECTRUM_DB_FLOOR)
            self.add_waterfall_row(frac)
            coords = np.empty(n * 2)
            coords[0::2] = np.arange(n) * (w / n)
            coords[1::2] = TRACE_H - 2 - frac * (TRACE_H - 6)
            c.create_line(*coords.tolist(), fill=TRACE, width=1.5, tags="trace")
            c.create_line(w / 2, 0, w / 2, TRACE_H, fill=AMBER, width=1.5, tags="trace")
            if peak_db >= SPECTRUM_DB_CEILING:
                c.create_text(w - 4, 8, text="clipping", anchor="e", fill=TX_RED,
                              font=(self.mono, 8), tags="trace")
            self.draw_scale(w, half_bins * bin_hz)
            readout = f"peak {peak_db:.0f} dB"
        if self.status_message and time.monotonic() < self.status_until:
            self.status_label.configure(text=self.status_message)
        else:
            self.status_message = None
            self.status_label.configure(text=readout)
        self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)


def main():
    ap = argparse.ArgumentParser(description="maxibitx control panel")
    ap.add_argument("--host", help="maxibitx's host (default: the last one used)")
    ap.add_argument("--port", type=int, help=f"rigctld port (default {DEFAULT_PORT})")
    ap.add_argument("--fullscreen", action="store_true", help="fill the screen (the sBitx's display)")
    Panel(ap.parse_args()).mainloop()


if __name__ == "__main__":
    main()e_control_and_iq_output.md) - nothing here is maxibitx-specific
beyond the commands it actually exercises:

    f  / F <hz>            get / set frequency (F also clears RIT, below)
    j  / J <hz>            get / set RIT - a receive-only tuning offset,
                            +/-9999 Hz (radio.h's RIT_MAX_HZ); never
                            applied to TX (radio.c's radio_set_rit()),
                            and auto-cleared by the next F
    l AF / L AF <0.0-1.0>  get / set the local CW monitor's volume
    l STRENGTH             get the S-meter reading (rx_audio.c's narrowband
                            meter envelope - post image-rejection and
                            narrow-filter-or-bypass, so it reflects what's
                            actually audible, not the AGC's own wideband
                            envelope - dB relative to a placeholder S9, see
                            rx_audio_get_strength_db()'s comment; read-only,
                            same as a real rig's S-meter)
    u NARROW / U NARROW <0|1>  get / set the post-demod CW filter
                                (rx_audio.c stage 3) in or out of circuit.
                                Its width is selectable - see CWWIDTH.
                                DIGITAL holds it out whatever is set, since
                                WSJT-X reads its audio downstream of it; the
                                get reports the EFFECTIVE state, so it reads
                                0 there, and the setting comes back on the
                                way out of DIGITAL
    t  / T <0|1>           get / set PTT (the TX Test section)
    u TONE / U TONE <0|1|2>  get / set the TX test-tone generator: off,
                              1 kHz, or two-tone 700 + 1900 Hz
    u FFTFILT / U FFTFILT <0|1>  get / set WHICH filter NARROW's "on"
                                  state uses - 1 = the shared FFT filter
                                  (rx_filter.c, the default, always
                                  minimum phase), 0 = an elliptic IIR from
                                  the pre-designed bank. Pitch and width
                                  below apply to either one
    l CWPITCH / L CWPITCH <hz>   get / set stage 3's center pitch - 500 to
                                  1000 Hz in 100 Hz steps. Moves the RX BFO
                                  and the TX sidetone with it, so the tone
                                  you hear moves and your carrier stays on
                                  the dial. A real Hamlib level
    l CWWIDTH / L CWWIDTH <hz>   get / set stage 3's width - 150, 300, 450
                                  or 600 Hz. Both settings snap to the
                                  nearest value the filter bank carries
                                  (src/narrow_filter_bank.h) and the reply
                                  says which one was selected
    u KEYER / U KEYER <0-4>      get / set the CW keyer: 0 straight, 1 bug,
                                  2 ultimatic, 3 iambic A, 4 iambic B. Takes
                                  effect once the keyer is idle
    l KEYSPD / L KEYSPD <wpm>    get / set the keyer's speed, 1-60 WPM (a
                                  real Hamlib level). Takes effect at the
                                  next element
    u PADREV / U PADREV <0|1>    get / set paddle reversal: 0 tip = dot,
                                  1 tip = dash
    b <text> / \\stop_morse      send text as CW at the keyer's speed
                                  (prosigns as <AR>, <SK>, <BT>, <KN>, ...),
                                  and stop it - the element being sent
                                  completes. CW/CWR only
    u MORSE                      1 while text is queued or being sent

It also shows a live spectrum, fed by a second, independent UDP
connection to src/interfaces/iq_stream.c's lightweight I/Q telemetry stream (UDP
port 4536, no relation to rigctld's TCP port above, and no relation to
the HPSDR Protocol 1 link WSJT-X/Thetis use for their own I/Q - see
iq_stream.h/.c's file headers for why this is its own third, minimal
path rather than reusing either). That stream carries the same native
96kHz baseband I/Q docs/02_rx_processing_pipeline.md describes, so the
FFT covers the full ±48kHz around dial center - the same range
docs/dsp_design_notes/antialias_filter_design.md's crystal-filter
analysis and rx_audio_demod_design.md's AGC-placement work were both
reasoning about. The display draws a selectable slice of it (±2.5, ±5 or
±15kHz), which is a crop of the same bins rather than a finer FFT - see
SPECTRUM_SPAN_CHOICES_HZ - with a waterfall of the same slice under it
(about 8 s of history, WATERFALL_ROWS).

The window scrolls, so it can be made shorter than its ~1300px of
content and still reach every control - a short laptop screen or the
Pi's own small touchscreen. The mouse wheel scrolls it, and does so even
over a slider or a dropdown, so scrolling past a control can never
change what it is set to.

Meant to run on a laptop, or on the Pi's own desktop if it has one - this
is a *client*, completely separate from the maxibitx binary itself. Point
it at the Pi's hostname/IP and the rigctld port and it just needs a TCP
route to it - same as any other rigctld client (WSJT-X, Thetis, rigctl) -
plus, for the spectrum, a UDP route to the same host's port 4536 (most
LANs/home networks impose no extra firewalling here beyond what TCP
4532 already needed, but a locked-down network might).

Requires Python 3's standard library plus numpy (only numpy - the
spectrum is drawn on a plain Tkinter Canvas, no plotting library needed).
tkinter usually ships with Python on Windows/macOS; on Debian/Raspberry
Pi OS it's a separate package if missing: `sudo apt install python3-tk`.
numpy: `pip install numpy` (or `sudo apt install python3-numpy`).

Run it:

    python3 tools/rigctl_panel.py
"""

import json
import os
import socket
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import numpy as np
except ImportError:
    print("rigctl_panel.py needs numpy for the spectrum display "
          "(pip install numpy, or sudo apt install python3-numpy).",
          file=sys.stderr)
    sys.exit(1)

CONFIG_PATH = os.path.expanduser("~/.maxibitx_panel.json")
# Read if CONFIG_PATH doesn't exist yet, so the saved host/port carries
# over from the file name this panel used before the project rename.
OLD_CONFIG_PATH = os.path.expanduser("~/.minibitx_panel.json")
DEFAULT_PORT = 4532
POLL_INTERVAL_S = 1.0

# The keyer's modes in u/U KEYER's numbering (src/keyer.h's enum keyer_mode).
KEYER_MODES = ("Straight", "Bug", "Ultimatic", "Iambic A", "Iambic B")
SOCKET_TIMEOUT_S = 2.0
# Full scale for the ALC bar. Correctly-set mic gain reads a couple of dB
# on peaks, so the useful part of the scale is its bottom third; anything
# past this is deep limiting either way.
ALC_METER_MAX_DB = 15.0

# --- Spectrum (iq_stream.c) ---
IQ_STREAM_PORT = 4536          # src/interfaces/iq_stream.h's IQ_STREAM_PORT
IQ_STREAM_MAGIC = b"IQS1"
SUBSCRIBE_INTERVAL_S = 1.0     # comfortably under iq_stream.c's 5s subscriber timeout
FFT_SIZE = 2048                # -> 96000/2048 = 46.875 Hz/bin across the full +-48kHz span
SPECTRUM_REDRAW_MS = 66        # ~15 fps - the FFT itself runs far faster than this and
                                # just keeps get_latest() fresh; no need to redraw faster
                                # than a human eye or a Tkinter Canvas benefits from
SPECTRUM_DB_FLOOR = -100.0     # y-axis floor, dBFS-style (0dB ~= one full-scale tone -
                                # see SpectrumClient's docstring on how that's calibrated)
# Y-axis top. NOT 0dBFS: nothing on this receiver's raw I/Q gets anywhere
# near full scale, so a 0dB top left the strongest real signals at about
# half height and spent the upper half of the canvas on levels that never
# occur. -37.5 puts a -50dBFS signal at 80% of the height
# ((-50) - (-100)) / ((-37.5) - (-100)) = 0.8 - which was the measured peak
# on this board, and it also stretches the whole trace: the gap between a
# -85dB noise floor and a -50dB peak grows from 35% of the canvas to 56%,
# so it is more readable rather than just taller.
#
# The cost is headroom. The trace flat-tops above -37.5dBFS, which is only
# 12.5dB above that measured peak, so a much stronger signal will clip
# against the top - the status line says so when it happens. Both this and
# the floor are display-only; raise this number if clipping shows up
# regularly, and use the peak/floor readout in the status line to pick a
# window that matches your own band conditions rather than these.
SPECTRUM_DB_CEILING = -37.5
# The FFT itself always covers the full native +-48kHz (FFT_SIZE stays
# 2048 whatever is selected - resolution is unaffected), and only the
# middle slice gets drawn. The widest choice is +-15kHz because
# docs/dsp_design_notes/antialias_filter_design.md's crystal-filter
# analysis puts the genuinely flat passband at only +-17.4/17.5kHz, with
# an asymmetric, increasingly attenuated skirt past that - so displaying
# the full +-48kHz mostly just shows the filter's own rolloff shape
# rather than real signal content, which is what prompted cropping it.
#
# The two narrower choices are for placing one signal rather than
# surveying a band: at +-15kHz a 300Hz CW filter's passband is 11px wide
# on a 560px canvas, which is not enough to see where a signal sits
# inside it. Since the FFT is unchanged, zooming spreads the same bins
# wider instead of resolving finer - 46.875Hz/bin throughout, about 107
# bins on screen at +-2.5kHz against 640 at +-15kHz. redraw_spectrum()
# reports the count so a blocky trace reads as "bin-limited" rather than
# as a fault.
SPECTRUM_SPAN_CHOICES_HZ = (2500, 5000, 15000)
SPECTRUM_DISPLAY_HALF_SPAN_HZ = 15000  # the default; the operator picks from the tuple above

# --- Waterfall, under the spectrum ---
# One row per spectrum redraw (SPECTRUM_REDRAW_MS), newest at the top, so
# 120 rows is about 8 s of history. Same span and same dB window as the
# trace above it; colours run through these stops from SPECTRUM_DB_FLOOR
# (black) to SPECTRUM_DB_CEILING (white).
WATERFALL_ROWS = 120
WATERFALL_STOPS = ((0.00, (0, 0, 0)), (0.30, (0, 0, 150)), (0.50, (0, 170, 230)),
                   (0.70, (240, 230, 0)), (0.85, (250, 60, 0)), (1.00, (255, 255, 255)))

# --- Signal strength ("l STRENGTH") ---
# Mirrors rx_audio.c's RX_STRENGTH_MIN_DB/MAX_DB exactly, so the bar
# always spans the full range the server can ever report - no clipping
# against a range chosen independently on this end.
STRENGTH_MIN_DB = -54   # S0, bottom of the conventional 6dB/S-unit scale
STRENGTH_MAX_DB = 60    # generous "S9+60" ceiling
STRENGTH_DB_PER_S_UNIT = 6.0

# --- RIT ("j"/"J") ---
# Mirrors radio.h's RIT_MAX_HZ exactly - the panel should never be able to
# ask the server for more range than it actually accepts.
RIT_MAX_HZ = 9999


def s_unit_label(db):
    """Format a dB-relative-to-S9 reading the way an operator reads an
    S-meter: "S1".."S9" below/at S9, "S9+N" above it - the same
    convention real Hamlib clients (and rx_audio_get_strength_db()'s own
    comment) use."""
    if db <= 0:
        s = round(9 + db / STRENGTH_DB_PER_S_UNIT)
        s = max(0, min(9, s))
        return f"S{s}"
    return f"S9+{db}"


def waterfall_palette():
    """WATERFALL_STOPS as a 256-entry RGB lookup table."""
    x = np.linspace(0.0, 1.0, 256)
    pos = [stop for stop, _ in WATERFALL_STOPS]
    return np.stack([np.interp(x, pos, [rgb[i] for _, rgb in WATERFALL_STOPS])
                     for i in range(3)], axis=1).astype(np.uint8)


def span_khz_label(hz):
    """Format a span or offset in Hz as compact kHz: 15000 -> "15k",
    2500 -> "2.5k". One decimal at most, and a trailing ".0" dropped, so
    the spectrum's own span buttons and its frequency ticks read the same
    way. Written as one helper rather than an f-string at each site
    because a plain "{hz/1000:.0f}k" turns the +-2.5kHz span's ticks into
    "2k" and "1k", which are both wrong by more than the CW filter is
    wide."""
    txt = f"{hz / 1000.0:.1f}".rstrip("0").rstrip(".")
    return f"{txt}k"


def load_config():
    for path in (CONFIG_PATH, OLD_CONFIG_PATH):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except (OSError, ValueError):
            continue
    return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f)
    except OSError:
        pass  # best-effort - remembering the last host/port isn't worth
              # failing the app over


class RigctlClient:
    """One TCP connection to maxibitx's rigctld server.

    rigctld is a plain line-request/line-reply protocol with no request
    IDs, so two commands must never be in flight on the same socket at
    once - there would be no way to tell which reply answers which
    request. self.lock enforces "one command at a time" between the
    panel's own actions (Tune, drag the volume slider) and the
    background poll loop's periodic refresh.
    """

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()

    def connect(self, host, port):
        s = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_S)
        s.settimeout(SOCKET_TIMEOUT_S)
        with self.lock:
            self.sock = s

    def disconnect(self):
        with self.lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None

    def connected(self):
        with self.lock:
            return self.sock is not None

    def query(self, command):
        """Send one command line, return the first reply line (stripped),
        or None if not connected, or on any socket error - which also
        drops the connection. The caller finds out via connected()
        turning false; this never retries on its own."""
        with self.lock:
            if self.sock is None:
                return None
            try:
                self.sock.sendall((command + "\n").encode())
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = self.sock.recv(256)
                    if not chunk:
                        raise OSError("connection closed by remote")
                    buf += chunk
                return buf.decode(errors="replace").strip()
            except OSError:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
                return None


class SpectrumClient:
    """Subscribes to iq_stream.c's UDP telemetry stream and keeps a
    rolling FFT of the most recent FFT_SIZE baseband I/Q samples ready
    for the GUI to draw.

    Fully independent of RigctlClient's TCP connection - its own UDP
    socket, its own subscribe/keepalive traffic (iq_stream.c drops a
    subscriber that goes quiet for 5s, so this just re-sends a bare
    datagram every SUBSCRIBE_INTERVAL_S to stay subscribed), its own
    receive thread. If that UDP port isn't reachable (firewalled, or an
    older maxibitx build without iq_stream.c), the spectrum panel just
    never gets data - the frequency/volume controls over rigctld keep
    working regardless, since the two connections don't know about each
    other any more than iq_stream.c and hamlib.c do on the maxibitx side.

    dB calibration: iq_stream.c scales samples so a full-scale baseband
    tone reads close to int16 full-scale (32767 - see iq_stream.c's file
    header). For a Hann-windowed FFT of a single tone sitting exactly on
    a bin center, dividing that bin's magnitude by (FFT_SIZE * mean(window))
    recovers the input tone's amplitude - so 0dB here means "as loud as a
    single full-scale input tone would read," a normal dBFS-style
    reference, not an absolutely-calibrated S-meter (no different from
    what any SDR app's own spectrum display does without a signal
    generator to calibrate against).
    """

    def __init__(self):
        self.sock = None
        self.host = None
        self.stop_event = threading.Event()
        self.recv_thread = None
        self.keepalive_thread = None
        self.lock = threading.Lock()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        self.latest_db = None  # np.ndarray (FFT_SIZE,), fftshifted low->high freq, or None
        self.window = np.hanning(FFT_SIZE)
        self.window_mean = float(np.mean(self.window)) or 1.0

    def start(self, host):
        self.stop()  # tolerate start() called twice without an intervening stop()
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.5)
        self.stop_event.clear()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        with self.lock:
            self.latest_db = None
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.keepalive_thread = threading.Thread(target=self._keepalive_loop, daemon=True)
        self.recv_thread.start()
        self.keepalive_thread.start()

    def stop(self):
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _keepalive_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is not None:
                try:
                    sock.sendto(b"S", (self.host, IQ_STREAM_PORT))
                except OSError:
                    pass
            time.sleep(SUBSCRIBE_INTERVAL_S)

    def _recv_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is None:
                break
            try:
                data, _addr = sock.recvfrom(2048)
            except OSError:
                continue
            if len(data) < 12 or data[0:4] != IQ_STREAM_MAGIC:
                continue
            n_samples = struct.unpack(">I", data[8:12])[0]
            if len(data) != 12 + n_samples * 4:
                continue  # torn/short packet - drop it, next one will be fine

            raw = np.frombuffer(data, dtype=">i2", count=n_samples * 2, offset=12)
            iq = raw.astype(np.float64).reshape(-1, 2)
            # Conjugated (-Q, not +Q) to correct display orientation.
            # maxibitx's raw baseband I/Q (shared by hpsdr_p1.c and this
            # stream) is spectrally inverted: a station +d Hz above dial
            # lands at baseband -d, so tuning up would move stations right
            # instead of the conventional-SDR-display left. sound.c's mixer
            # is deliberately left as-is (hpsdr_p1.c clients work with it);
            # rx_audio.c compensates the same way for USB/LSB - see its
            # RX_IQ_SPECTRUM_INVERTED.
            samples = (iq[:, 0] - 1j * iq[:, 1]) / 32767.0

            self.sample_buf = np.concatenate((self.sample_buf, samples))
            if len(self.sample_buf) > FFT_SIZE * 2:
                self.sample_buf = self.sample_buf[-FFT_SIZE * 2:]

            if len(self.sample_buf) >= FFT_SIZE:
                block = self.sample_buf[-FFT_SIZE:]
                spectrum = np.fft.fftshift(np.fft.fft(block * self.window))
                mag = np.abs(spectrum) / (FFT_SIZE * self.window_mean)
                db = 20.0 * np.log10(mag + 1e-12)
                with self.lock:
                    self.latest_db = db

    def get_latest(self):
        with self.lock:
            return None if self.latest_db is None else self.latest_db.copy()


class Panel(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("maxibitx control panel")
        # Width stays fixed - every control group is sized by a canvas or an
        # entry, so there is nothing to gain from a wider window, and fixing it
        # keeps the one runtime-variable label (the spectrum status line) from
        # being able to drag the window wider. Height is now adjustable,
        # because the content is taller than a 1080p screen's usable area once
        # the title bar and taskbar are counted, and the whole point of the
        # scroll container below is to let the operator make the window as
        # short as their display requires.
        self.resizable(False, True)

        self.client = RigctlClient()
        self.poll_thread = None
        self.poll_stop = threading.Event()
        self.freq_entry_focused = False
        self.rit_entry_focused = False

        self.spectrum = SpectrumClient()
        self.spectrum_running = False
        self.current_freq_hz = None

        # Guards on_narrow_toggled() while apply_narrow() is syncing the
        # checkbox from a poll reply, so reading the state back from the
        # server never turns around and re-sends it as a fresh command -
        # same "don't let a poll-driven UI update loop back into a
        # network write" concern the freq entry's freq_entry_focused
        # guard exists for, just via a plain before/after flag instead
        # since there's no focus-in-progress signal for a checkbox.
        self._syncing_narrow = False
        # Same guard, same reasoning, for the FFT-filter selector below.
        self._syncing_fftfilt = False
        # Same guard, same reasoning, for the pitch/width selectors.
        self._syncing_pitch = False
        self._syncing_width = False
        # Same guard, same reasoning, for the mode selector below.
        self._syncing_mode = False
        # Same guard, same reasoning, for the TX test controls below.
        self._syncing_tone = False
        self._syncing_ptt = False
        # Same guard, same reasoning, for the keyer controls. The WPM box is a
        # text entry as well, so like the frequency entry it isn't overwritten
        # by a poll while it has focus.
        self._syncing_keyer = False
        self._syncing_padrev = False
        self.wpm_focused = False

        cfg = load_config()

        # --- scroll container ---
        # Everything below lives in self.body, a frame inside a Canvas, so the
        # whole panel scrolls vertically. Without it the window is ~1300px of
        # content, which does not fit a 1080p screen once the title bar and
        # taskbar are counted, and on a smaller display (a Pi's own 7"
        # touchscreen, a laptop at 768px) the TX Power group at the bottom was
        # simply unreachable - the window could not be shrunk and the content
        # could not be scrolled.
        #
        # A Canvas is the standard way to do this in Tk: a Frame has no
        # scrolling of its own, while a Canvas has a scrollregion. The frame
        # goes inside it as a canvas window item, keeps its natural size (so
        # nothing about the layout changes), and the <Configure> binding below
        # keeps the scrollregion matched to it as widgets appear.
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.body_canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0)
        self.body_canvas.grid(row=0, column=0, sticky="nsew")
        body_vsb = ttk.Scrollbar(self, orient="vertical", command=self.body_canvas.yview)
        body_vsb.grid(row=0, column=1, sticky="ns")
        self.body_canvas.configure(yscrollcommand=body_vsb.set)
        self.body = ttk.Frame(self.body_canvas)
        self.body_canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.body.bind("<Configure>", self.on_body_configure)

        # --- connection row ---
        conn = ttk.Frame(self.body, padding=8)
        conn.grid(row=0, column=0, sticky="ew")
        ttk.Label(conn, text="Host:").grid(row=0, column=0)
        self.host_var = tk.StringVar(value=cfg.get("host", ""))
        ttk.Entry(conn, textvariable=self.host_var, width=18).grid(row=0, column=1, padx=4)
        ttk.Label(conn, text="Port:").grid(row=0, column=2)
        self.port_var = tk.StringVar(value=str(cfg.get("port", DEFAULT_PORT)))
        ttk.Entry(conn, textvariable=self.port_var, width=6).grid(row=0, column=3, padx=4)
        self.connect_btn = ttk.Button(conn, text="Connect", command=self.on_connect_clicked)
        self.connect_btn.grid(row=0, column=4, padx=8)
        self.status_var = tk.StringVar(value="disconnected")
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="#a00")
        self.status_label.grid(row=0, column=5, padx=4)

        # --- frequency ---
        freq = ttk.LabelFrame(self.body, text="Frequency (Hz)", padding=8)
        freq.grid(row=1, column=0, sticky="ew", padx=8, pady=4)
        self.freq_display_var = tk.StringVar(value="—")
        ttk.Label(freq, textvariable=self.freq_display_var, font=("monospace", 20)).grid(
            row=0, column=0, columnspan=6, pady=(0, 6))

        self.freq_entry_var = tk.StringVar()
        entry = ttk.Entry(freq, textvariable=self.freq_entry_var, width=14, font=("monospace", 12))
        entry.grid(row=1, column=0, columnspan=3)
        entry.bind("<FocusIn>", lambda e: setattr(self, "freq_entry_focused", True))
        entry.bind("<FocusOut>", lambda e: setattr(self, "freq_entry_focused", False))
        entry.bind("<Return>", lambda e: self.on_tune_clicked())
        ttk.Button(freq, text="Tune", command=self.on_tune_clicked).grid(row=1, column=3, padx=4)

        steps = ttk.Frame(freq)
        steps.grid(row=2, column=0, columnspan=6, pady=(6, 0))
        for label, delta in [("-1k", -1000), ("-100", -100), ("-10", -10),
                              ("+10", 10), ("+100", 100), ("+1k", 1000)]:
            ttk.Button(steps, text=label, width=5,
                       command=lambda d=delta: self.on_step_clicked(d)).pack(side="left", padx=2)

        # --- mode ---
        # rigctld's m/M (radio.c's radio_get_mode()/radio_set_mode(),
        # docs/ARCHITECTURE.md build order step 3/8) - the same real,
        # single-owner mode value both this panel and the Kenwood CAT
        # surface (usb_gadget.c) now agree on. Matters a lot more than it
        # used to as of step 8: CW/USB/LSB now genuinely select a
        # different TX audio source and sideband in sound.c/cw.c (see
        # ARCHITECTURE.md §10 step 8), not just a cosmetic label - keying
        # the PTT/key line in the wrong mode is a real, easy way to key
        # TX with no audible result (e.g. still in CW while trying to
        # talk), which is exactly the kind of mixup this control exists
        # to make obvious and quick to fix. DIGITAL transmits the USB
        # gadget's audio (WSJT-X).
        mode = ttk.LabelFrame(self.body, text="Mode", padding=8)
        mode.grid(row=2, column=0, sticky="ew", padx=8, pady=4)
        self.mode_var = tk.StringVar(value="CW")
        # CWR is CW-reverse: the same key and the same transmitted
        # carrier as CW, listening on the other side of the BFO to move
        # away from an interfering signal (radio.h).
        for i, name in enumerate(("CW", "CWR", "USB", "LSB", "DIGITAL")):
            ttk.Radiobutton(mode, text=name, value=name, variable=self.mode_var,
                             command=self.on_mode_changed).grid(row=0, column=i, padx=(0 if i == 0 else 10, 0))

        # --- keyer ---
        # rigctld's u/U KEYER (src/keyer.h's mode), l/L KEYSPD (a real Hamlib
        # level, in WPM) and u/U PADREV (paddle reversal, src/key_input.h).
        # The server applies a mode change once the keyer is idle and a speed
        # change at the next element, so a change mid-character never cuts an
        # element short. Polled like everything else, so a speed set over CAT
        # (Kenwood KS) shows up here too. Reversal only matters to the paddle
        # modes; a straight key keys from either contact.
        kf = ttk.LabelFrame(self.body, text="Keyer (CW)", padding=8)
        kf.grid(row=3, column=0, sticky="ew", padx=8, pady=4)
        self.keyer_var = tk.StringVar(value=KEYER_MODES[0])
        self.keyer_combo = ttk.Combobox(kf, textvariable=self.keyer_var, width=10,
                                         state="readonly", values=KEYER_MODES)
        self.keyer_combo.grid(row=0, column=0)
        self.keyer_combo.bind("<<ComboboxSelected>>", self.on_keyer_selected)
        ttk.Label(kf, text="WPM").grid(row=0, column=1, padx=(12, 4))
        self.wpm_var = tk.StringVar(value="20")
        self.wpm_spin = ttk.Spinbox(kf, from_=1, to=60, width=4, textvariable=self.wpm_var,
                                    command=self.on_wpm_changed)
        self.wpm_spin.grid(row=0, column=2)
        self.wpm_spin.bind("<Return>", lambda e: self.on_wpm_changed())
        self.wpm_spin.bind("<FocusIn>", lambda e: setattr(self, "wpm_focused", True))
        self.wpm_spin.bind("<FocusOut>", self.on_wpm_focus_out)
        self.padrev_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(kf, text="Reverse paddles", variable=self.padrev_var,
                        command=self.on_padrev_toggled).grid(row=0, column=3, padx=(12, 0))

        # Text to send - rigctld's b, and \stop_morse. The text stays in the
        # box after Send, so a CQ can be sent again with one click. Any touch
        # of the key or paddle stops it too, server-side. The label shows
        # u MORSE: whether text is still queued or going out.
        trow = ttk.Frame(kf)
        trow.grid(row=1, column=0, columnspan=4, sticky="w", pady=(6, 0))
        self.cw_text_var = tk.StringVar(value="")
        self.cw_text_entry = ttk.Entry(trow, textvariable=self.cw_text_var, width=26)
        self.cw_text_entry.pack(side="left")
        self.cw_text_entry.bind("<Return>", lambda e: self.on_cw_send_clicked())
        ttk.Button(trow, text="Send", width=5, command=self.on_cw_send_clicked).pack(
            side="left", padx=(4, 0))
        ttk.Button(trow, text="Stop", width=5, command=self.on_cw_stop_clicked).pack(
            side="left", padx=(4, 0))
        self.cw_text_status_var = tk.StringVar(value="")
        ttk.Label(trow, textvariable=self.cw_text_status_var, width=8).pack(side="left",
                                                                        padx=(8, 0))

        # --- RIT ---
        # rigctld's j/J (radio.c's radio_set_rit()/radio_get_rit()) - a
        # receive-only offset, see this file's module docstring. Unlike
        # the narrow-filter checkbox below, there's no separate on/off
        # bit to sync from the server - "off" IS 0 Hz, same convention
        # real Hamlib rigs use - so Clear is just "J 0" spelled out as
        # its own button for a one-click reset mid-QSO.
        rit = ttk.LabelFrame(self.body, text="RIT - receive only (Hz)", padding=8)
        rit.grid(row=4, column=0, sticky="ew", padx=8, pady=4)
        self.rit_display_var = tk.StringVar(value="—")
        ttk.Label(rit, textvariable=self.rit_display_var, font=("monospace", 16)).grid(
            row=0, column=0, columnspan=6, pady=(0, 6))

        self.rit_entry_var = tk.StringVar(value="0")
        rit_entry = ttk.Entry(rit, textvariable=self.rit_entry_var, width=8, font=("monospace", 12))
        rit_entry.grid(row=1, column=0, columnspan=2)
        rit_entry.bind("<FocusIn>", lambda e: setattr(self, "rit_entry_focused", True))
        rit_entry.bind("<FocusOut>", lambda e: setattr(self, "rit_entry_focused", False))
        rit_entry.bind("<Return>", lambda e: self.on_rit_set_clicked())
        ttk.Button(rit, text="Set", command=self.on_rit_set_clicked).grid(row=1, column=2, padx=4)
        ttk.Button(rit, text="Clear", command=self.on_rit_clear_clicked).grid(row=1, column=3, padx=4)

        rit_steps = ttk.Frame(rit)
        rit_steps.grid(row=2, column=0, columnspan=6, pady=(6, 0))
        for label, delta in [("-100", -100), ("-10", -10), ("+10", 10), ("+100", 100)]:
            ttk.Button(rit_steps, text=label, width=5,
                       command=lambda d=delta: self.on_rit_step_clicked(d)).pack(side="left", padx=2)

        # --- volume ---
        vol = ttk.LabelFrame(self.body, text="Volume", padding=8)
        vol.grid(row=5, column=0, sticky="ew", padx=8, pady=(4, 8))
        self.vol_var = tk.IntVar(value=50)
        self.vol_scale = ttk.Scale(vol, from_=0, to=100, orient="horizontal",
                                    variable=self.vol_var, length=280,
                                    command=self.on_volume_dragged)
        self.vol_scale.grid(row=0, column=0, padx=(0, 8))
        # The actual L AF command only goes out on release, not on every
        # tick of the drag - see on_volume_released.
        self.vol_scale.bind("<ButtonRelease-1>", self.on_volume_released)
        self.vol_label = ttk.Label(vol, text="50%", width=5)
        self.vol_label.grid(row=0, column=1)

        # --- mic gain (TX) ---
        # rigctld's l/L MICGAIN (sound.c's mic_tx_gain, this server's own
        # extension - see hamlib.c's l/L comment) - live gain on real mic
        # audio feeding USB/LSB TX (docs/ARCHITECTURE.md §10 step 8), NOT
        # a 0-100% volume like the RX slider above: there's no natural
        # ceiling for "how much extra gain the mic needs," so this is a
        # raw multiplier, 1.0 = no extra gain beyond the fixed unit
        # conversion. Exists specifically for bisecting against a real
        # wattmeter reading while transmitting, without a rebuild/restart
        # between trials - the first on-air SSB test found real audio
        # reaching the local monitor speaker but no measurable power out,
        # consistent with this needing to go up from its 1.0 default.
        mg = ttk.LabelFrame(self.body, text="Mic Gain (TX, USB/LSB)", padding=8)
        mg.grid(row=6, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.micgain_var = tk.DoubleVar(value=1.0)
        self.micgain_scale = ttk.Scale(mg, from_=0.0, to=64.0, orient="horizontal",
                                         variable=self.micgain_var, length=280,
                                         command=self.on_micgain_dragged)
        self.micgain_scale.grid(row=0, column=0, padx=(0, 8))
        # Same "only send on release, not every drag tick" pattern as
        # Volume above.
        self.micgain_scale.bind("<ButtonRelease-1>", self.on_micgain_released)
        self.micgain_label = ttk.Label(mg, text="1.00x", width=6)
        self.micgain_label.grid(row=0, column=1)

        # --- narrow filter ---
        # rx_audio.c stage 3, the post-demod "single signal"
        # selectivity filter - a plain on/off toggle (not a runtime-
        # adjustable width, see rx_audio.h), reached over rigctld's
        # u/U NARROW (this server's own extension, not a real Hamlib
        # function - see hamlib.c's u/U comment). The whole group is
        # disabled in DIGITAL - see set_rx_filter_gate() below.
        nf = ttk.LabelFrame(self.body, text="RX Filter", padding=8)
        nf.grid(row=7, column=0, sticky="ew", padx=8, pady=(0, 8))
        # Kept for set_rx_filter_gate() below, which retitles this frame in
        # DIGITAL. The title carries the explanation rather than a separate
        # label: the panel is already ~1200px of content, and every row added
        # here is a row someone on a small display has to scroll past.
        self.narrow_frame = nf
        # One checkbox for whether the filter is in circuit, then one choice
        # of which filter, then its pitch and width. An earlier layout had
        # three checkboxes - on/off, elliptic-vs-FFT, and which realization of
        # the FFT one - which read as three independent options when it was
        # really one two-way choice with a modifier that only applied to one
        # side. An operator spent a long time believing the pitch and width
        # selectors belonged to the FFT filter alone; they never did. The
        # linear-phase realization is gone from the server too (rx_audio.h),
        # so the third box had nothing left to select.
        self.narrow_var = tk.BooleanVar(value=True)
        self.narrow_check = ttk.Checkbutton(nf, text="CW filter", variable=self.narrow_var,
                                             command=self.on_narrow_toggled)
        self.narrow_check.grid(row=0, column=0, sticky="w")

        # Which filter, as two radio buttons rather than a checkbox: the
        # choice is between two named things, and a checkbox labelled with one
        # of them leaves the other unnamed. rigctld's u/U FFTFILT underneath,
        # where 1 is the FFT filter. Indented under the checkbox above,
        # because it means nothing while the filter is out of circuit.
        #
        # The FFT filter is the server's default. The elliptic bank is not a
        # legacy option - it settles faster on a keyed element, and it is what
        # the server falls back to for an odd-sized block. It is NOT the way to
        # save CPU: the server runs both filters on every block whatever is
        # selected, so selecting this one frees nothing (rx_audio.h).
        self.fftfilt_var = tk.BooleanVar(value=True)
        frow = ttk.Frame(nf)
        frow.grid(row=1, column=0, sticky="w", padx=(16, 0))
        self.fftfilt_radios = (
            ttk.Radiobutton(frow, text="FFT (tunable)", variable=self.fftfilt_var, value=True,
                             command=self.on_fftfilt_toggled),
            ttk.Radiobutton(frow, text="Elliptic bank", variable=self.fftfilt_var, value=False,
                             command=self.on_fftfilt_toggled),
        )
        self.fftfilt_radios[0].pack(side="left")
        self.fftfilt_radios[1].pack(side="left", padx=(12, 0))

        # Stage 3's pitch and width - rigctld's l/L CWPITCH (a real Hamlib
        # level) and l/L CWWIDTH (this server's extension). Both apply to
        # whichever filter is selected above, which is the whole reason they
        # sit outside the choice rather than under one arm of it.
        #
        # Fixed lists rather than values read from the server: rigctld has no
        # "enumerate the choices" command, and hard-coding them here means
        # this panel can drift from src/narrow_filter_bank.h if that table is
        # ever regenerated at other frequencies. The server snaps whatever it
        # is sent to its nearest entry and the reply says what it chose, so
        # drift shows up as a selector that springs back rather than as
        # silently wrong behavior - apply_pitch()/apply_width() below add any
        # value the server reports that isn't already in the list.
        #
        # Both on one row. The panel scrolls now, so this is no longer the
        # difference between reachable and not, but the content is still
        # ~1200px against a 1080p screen's usable height and a row saved here
        # is a row nobody has to scroll past.
        pwrow = ttk.Frame(nf)
        pwrow.grid(row=2, column=0, sticky="w", padx=(16, 0), pady=(6, 0))
        ttk.Label(pwrow, text="Pitch").pack(side="left")
        self.pitch_var = tk.StringVar(value="700")
        self.pitch_combo = ttk.Combobox(pwrow, textvariable=self.pitch_var, width=5,
                                         state="readonly",
                                         values=("500", "600", "700", "800", "900", "1000"))
        self.pitch_combo.pack(side="left", padx=(4, 2))
        ttk.Label(pwrow, text="Hz    Width").pack(side="left")
        self.width_var = tk.StringVar(value="300")
        self.width_combo = ttk.Combobox(pwrow, textvariable=self.width_var, width=5,
                                         state="readonly", values=("150", "300", "450", "600"))
        self.width_combo.pack(side="left", padx=(4, 2))
        ttk.Label(pwrow, text="Hz").pack(side="left")
        self.pitch_combo.bind("<<ComboboxSelected>>", self.on_pitch_selected)
        self.width_combo.bind("<<ComboboxSelected>>", self.on_width_selected)

        # --- signal strength ("l STRENGTH") ---
        # Read-only, like a real rig's S-meter - no slider/checkbox to
        # drive a network write, just a bar + label kept current by
        # refresh_once()'s poll, same as the frequency readout above.
        sm = ttk.LabelFrame(self.body, text="Signal Strength (uncalibrated)", padding=8)
        sm.grid(row=8, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.smeter_canvas_w = 380
        self.smeter_canvas_h = 40
        self.smeter_canvas = tk.Canvas(sm, width=self.smeter_canvas_w,
                                        height=self.smeter_canvas_h,
                                        background="#111", highlightthickness=0)
        self.smeter_canvas.grid(row=0, column=0)
        self.smeter_label_var = tk.StringVar(value="—")
        ttk.Label(sm, textvariable=self.smeter_label_var, font=("monospace", 13),
                   width=15).grid(row=0, column=1, padx=(10, 0))
        self.draw_smeter(None)

        # --- spectrum ---
        spec = ttk.LabelFrame(self.body, text="Spectrum", padding=8)
        spec.grid(row=9, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.spectrum_canvas_w = 560
        self.spectrum_canvas_h = 180
        self.spectrum_canvas = tk.Canvas(spec, width=self.spectrum_canvas_w,
                                          height=self.spectrum_canvas_h,
                                          background="#111", highlightthickness=0)
        # Displayed span, as a row of radio buttons above the trace. Purely a
        # display crop - the FFT is unchanged (still FFT_SIZE points over the
        # full native +-48kHz at 46.875 Hz/bin), so a narrower span shows the
        # same bins spread wider, not finer resolution. At +-2.5kHz that is
        # about 107 bins across 560px, so the trace goes visibly blocky; the
        # status line reports the bin count for exactly that reason. Narrowing
        # it is still what makes a CW signal's position against a 300Hz filter
        # legible, which at +-15kHz occupies 11px.
        self.spectrum_span_var = tk.IntVar(value=SPECTRUM_DISPLAY_HALF_SPAN_HZ)
        spanrow = ttk.Frame(spec)
        spanrow.pack(anchor="w", pady=(0, 4))
        ttk.Label(spanrow, text="Span").pack(side="left", padx=(0, 6))
        for i, hz in enumerate(SPECTRUM_SPAN_CHOICES_HZ):
            ttk.Radiobutton(spanrow, text=f"±{span_khz_label(hz)}",
                             variable=self.spectrum_span_var, value=hz,
                             command=self.on_spectrum_span_changed
                             ).pack(side="left", padx=(0 if i == 0 else 10, 0))
        self.spectrum_canvas.pack()
        # The waterfall: a photo image rewritten whole each redraw from
        # waterfall_rows (rows x width x RGB), which Tk loads as a PPM - about
        # a millisecond for the whole image, where per-pixel puts would take
        # far longer.
        self.waterfall_rows = np.zeros((WATERFALL_ROWS, self.spectrum_canvas_w, 3), np.uint8)
        self.waterfall_palette = waterfall_palette()
        self.waterfall_image = tk.PhotoImage(width=self.spectrum_canvas_w, height=WATERFALL_ROWS)
        self.waterfall_canvas = tk.Canvas(spec, width=self.spectrum_canvas_w,
                                           height=WATERFALL_ROWS, background="#000",
                                           highlightthickness=0)
        self.waterfall_canvas.create_image(0, 0, image=self.waterfall_image, anchor="nw")
        # The dial centre, as on the trace; sparse dashes so a signal sitting
        # on it still shows.
        self.waterfall_canvas.create_line(self.spectrum_canvas_w / 2, 0,
                                           self.spectrum_canvas_w / 2, WATERFALL_ROWS,
                                           fill="#fb3", dash=(2, 6))
        self.waterfall_canvas.pack(pady=(2, 0))
        self.spectrum_status_var = tk.StringVar(value="no spectrum data yet")
        # wraplength pinned to the canvas width: this label's text is the only
        # thing in the window whose length varies at runtime, and the window's
        # width is fixed by resizable(False, True). Without the cap, a long
        # status line silently widens the whole panel and there is no way for
        # the operator to drag it back - measured at 891px against the normal
        # 596px before this was added.
        ttk.Label(spec, textvariable=self.spectrum_status_var,
                   wraplength=self.spectrum_canvas_w).pack(anchor="w", pady=(4, 0))

        # --- TX test ---
        # rigctld's u/U TONE (the test-tone generator, tone_gen.h) and t/T
        # (PTT). With a tone selected, Transmit keys the radio and sends it
        # in any mode; the sideband follows the mode. maxibitx turns the
        # tone off and drops PTT after TONE_GEN_TIMEOUT_S (30 s).
        # docs/dsp_design_notes/tx_test_tones_and_alc.md.
        txt = ttk.LabelFrame(self.body, text="TX Test - dummy load or low power", padding=8)
        txt.grid(row=10, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.tone_var = tk.IntVar(value=0)
        for i, (label, val) in enumerate((("Off", 0), ("1 kHz tone", 1),
                                          ("Two-tone 700 + 1900 Hz", 2))):
            ttk.Radiobutton(txt, text=label, value=val, variable=self.tone_var,
                             command=self.on_tone_changed).grid(row=0, column=i, padx=(0 if i == 0 else 10, 0))
        self.ptt_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(txt, text="Transmit", variable=self.ptt_var,
                         command=self.on_ptt_toggled).grid(row=1, column=0, sticky="w", pady=(6, 0))
        ttk.Label(txt, text="auto-off after 30 s").grid(row=1, column=1, columnspan=2,
                                                        sticky="w", pady=(6, 0))

        # --- TX power and ALC ---
        # Power is rigctld's l/L RFPOWER: a percentage of hw_settings.ini's
        # max_power, which moves tx_pipeline.c's limiter ceiling rather
        # than adding a gain after it, so max_power stays unreachable from
        # here. ALC is the limiter's gain reduction in dB (this server's
        # extension, not Hamlib's 0.0-1.0), and it's the instrument for
        # setting Mic Gain above: advance mic gain until speech peaks read
        # a couple of dB and no further. Turning Power down lowers output;
        # turning Mic Gain up drives harder into the ceiling and shows up
        # here instead. docs/03_tx_processing_pipeline.md, "Setting power".
        pw = ttk.LabelFrame(self.body, text="TX Power", padding=8)
        pw.grid(row=11, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.power_var = tk.DoubleVar(value=100.0)
        self.power_scale = ttk.Scale(pw, from_=0, to=100, orient="horizontal",
                                      variable=self.power_var, length=280,
                                      command=self.on_power_dragged)
        self.power_scale.grid(row=0, column=0, padx=(0, 8))
        # Same "only send on release" pattern as Volume and Mic Gain.
        self.power_scale.bind("<ButtonRelease-1>", self.on_power_released)
        self.power_label = ttk.Label(pw, text="100%", width=6)
        self.power_label.grid(row=0, column=1)

        ttk.Label(pw, text="ALC (gain reduction):").grid(row=1, column=0, sticky="w",
                                                          pady=(6, 0))
        self.alc_bar = ttk.Progressbar(pw, orient="horizontal", length=280,
                                        mode="determinate", maximum=ALC_METER_MAX_DB)
        self.alc_bar.grid(row=2, column=0, padx=(0, 8))
        self.alc_label = ttk.Label(pw, text="0.0 dB", width=8)
        self.alc_label.grid(row=2, column=1)

        self.bind_scroll_wheel()
        self.size_to_screen()

        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---- scrolling ----

    def on_body_configure(self, _event):
        # The scrollregion has to follow the content, not be set once: the
        # body's requested height changes as widgets are built and again
        # whenever the spectrum status line wraps to a second line.
        self.body_canvas.configure(scrollregion=self.body_canvas.bbox("all"))

    def size_to_screen(self):
        """Open at the content's natural size, or as tall as the screen
        usefully allows, whichever is smaller - so a short display gets a
        window that fits and scrolls rather than one whose bottom controls
        are off-screen and unreachable."""
        self.update_idletasks()
        body_w = self.body.winfo_reqwidth()
        body_h = self.body.winfo_reqheight()
        # The canvas is the viewport; it must be the body's full width or the
        # right-hand edge of every group is clipped (there is no horizontal
        # scrollbar, by choice - see resizable() in __init__).
        self.body_canvas.configure(width=body_w, height=body_h)
        # 120px covers a title bar plus a taskbar/panel with room to spare.
        # Erring large costs a little unused screen; erring small puts a
        # control back off the bottom edge, which is the bug being fixed.
        usable_h = max(400, self.winfo_screenheight() - 120)
        self.geometry(f"{body_w + 18}x{min(body_h, usable_h)}")

    def scroll_body(self, event):
        """One wheel notch. Windows and macOS send <MouseWheel> with a
        signed delta (120 per notch on Windows, small values on macOS); X11
        sends Button-4 (up) and Button-5 (down) with no delta, which is what
        the Pi itself uses."""
        if getattr(event, "num", None) == 4:
            step = -1
        elif getattr(event, "num", None) == 5:
            step = 1
        else:
            delta = getattr(event, "delta", 0)
            if not delta:
                return
            step = -1 if delta > 0 else 1
        self.body_canvas.yview_scroll(step, "units")

    def scroll_body_only(self, event):
        """The wheel over a combobox or a slider scrolls the page and does
        NOT change the control's value. Deliberate: ttk gives a readonly
        combobox its own wheel binding on some platforms, so without this a
        scroll down the panel could silently retune the CW filter's pitch or
        move TX power on the way past. A control surface that changes a
        setting the operator did not mean to touch has cost this project real
        time before (docs/09_faq.md, on what the filter A/B comparison
        taught). Returning "break" stops the widget's class binding, so this
        is the only handler that runs."""
        self.scroll_body(event)
        return "break"

    def bind_scroll_wheel(self):
        # bind_all so the wheel works wherever the pointer happens to be,
        # then the value-carrying widgets get their own binding that wins
        # over both their class binding and this one.
        self.bind_all("<MouseWheel>", self.scroll_body)
        self.bind_all("<Button-4>", self.scroll_body)
        self.bind_all("<Button-5>", self.scroll_body)
        for w in (self.pitch_combo, self.width_combo, self.vol_scale,
                  self.micgain_scale, self.power_scale):
            for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                w.bind(seq, self.scroll_body_only)

    # ---- connection handling ----

    def on_connect_clicked(self):
        if self.client.connected():
            self.disconnect()
            return
        host = self.host_var.get().strip()
        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            messagebox.showerror("maxibitx panel", "Port must be a number.")
            return
        if not host:
            messagebox.showerror("maxibitx panel", "Enter the Pi's hostname or IP address.")
            return
        try:
            self.client.connect(host, port)
        except OSError as e:
            messagebox.showerror("maxibitx panel", f"Couldn't connect to {host}:{port}\n{e}")
            return

        save_config({"host": host, "port": port})
        self.status_var.set("connected")
        self.status_label.configure(foreground="#0a0")
        self.connect_btn.configure(text="Disconnect")

        # One immediate refresh so the panel isn't blank while it waits
        # for the first poll tick.
        self.refresh_once()

        self.poll_stop.clear()
        self.poll_thread = threading.Thread(target=self.poll_loop, daemon=True)
        self.poll_thread.start()

        # Independent of the rigctld connection above - see SpectrumClient's
        # docstring. Started/stopped alongside it purely because "connect"
        # is the one button this panel has; a failure here never affects
        # rigctld's own connection.
        self.spectrum.start(host)
        if not self.spectrum_running:
            self.spectrum_running = True
            self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)

    def disconnect(self):
        self.poll_stop.set()
        self.client.disconnect()
        self.spectrum.stop()
        self.spectrum_running = False
        self.current_freq_hz = None
        self.spectrum_status_var.set("no spectrum data yet")
        self.spectrum_canvas.delete("all")
        self.clear_waterfall()
        self.smeter_label_var.set("—")
        self.draw_smeter(None)
        self.rit_display_var.set("—")
        self.rit_entry_var.set("0")
        self.status_var.set("disconnected")
        self.status_label.configure(foreground="#a00")
        self.connect_btn.configure(text="Connect")

    def on_close(self):
        self.disconnect()
        self.destroy()

    # ---- background polling ----
    #
    # Runs on its own thread so a slow/stalled connection can never
    # freeze the GUI's own event loop. All widget updates it triggers go
    # through self.after(...) to hand them back to the main thread rather
    # than touching Tk widgets directly from here.

    def poll_loop(self):
        while not self.poll_stop.is_set():
            if not self.client.connected():
                self.after(0, self.disconnect)
                return
            self.refresh_once()
            time.sleep(POLL_INTERVAL_S)

    def refresh_once(self):
        freq_reply = self.client.query("f")
        rit_reply = self.client.query("j")
        vol_reply = self.client.query("l AF")
        micgain_reply = self.client.query("l MICGAIN")
        mode_reply = self.client.query("m")
        narrow_reply = self.client.query("u NARROW")
        fftfilt_reply = self.client.query("u FFTFILT")
        pitch_reply = self.client.query("l CWPITCH")
        width_reply = self.client.query("l CWWIDTH")
        strength_reply = self.client.query("l STRENGTH")
        tone_reply = self.client.query("u TONE")
        ptt_reply = self.client.query("t")
        power_reply = self.client.query("l RFPOWER")
        alc_reply = self.client.query("l ALC")
        keyer_reply = self.client.query("u KEYER")
        wpm_reply = self.client.query("l KEYSPD")
        padrev_reply = self.client.query("u PADREV")
        morse_reply = self.client.query("u MORSE")
        if freq_reply is None or rit_reply is None or vol_reply is None \
                or micgain_reply is None or mode_reply is None or narrow_reply is None \
                or fftfilt_reply is None \
                or pitch_reply is None or width_reply is None \
                or strength_reply is None \
                or tone_reply is None or ptt_reply is None \
                or power_reply is None or alc_reply is None \
                or keyer_reply is None or wpm_reply is None or padrev_reply is None \
                or morse_reply is None:
            self.after(0, self.disconnect)
            return
        self.after(0, lambda: self.apply_freq(freq_reply))
        self.after(0, lambda: self.apply_rit(rit_reply))
        self.after(0, lambda: self.apply_volume(vol_reply))
        self.after(0, lambda: self.apply_micgain(micgain_reply))
        self.after(0, lambda: self.apply_mode(mode_reply))
        self.after(0, lambda: self.apply_narrow(narrow_reply))
        self.after(0, lambda: self.apply_fftfilt(fftfilt_reply))
        self.after(0, lambda: self.apply_pitch(pitch_reply))
        self.after(0, lambda: self.apply_width(width_reply))
        self.after(0, lambda: self.apply_strength(strength_reply))
        self.after(0, lambda: self.apply_tone(tone_reply))
        self.after(0, lambda: self.apply_ptt(ptt_reply))
        self.after(0, lambda: self.apply_power(power_reply))
        self.after(0, lambda: self.apply_alc(alc_reply))
        self.after(0, lambda: self.apply_keyer(keyer_reply))
        self.after(0, lambda: self.apply_wpm(wpm_reply))
        self.after(0, lambda: self.apply_padrev(padrev_reply))
        self.after(0, lambda: self.apply_morse(morse_reply))

    def apply_freq(self, reply):
        try:
            hz = int(reply)
        except ValueError:
            return
        self.current_freq_hz = hz
        self.freq_display_var.set(f"{hz:,}".replace(",", "."))
        # Don't clobber text the operator is mid-way through typing.
        if not self.freq_entry_focused:
            self.freq_entry_var.set(str(hz))

    def apply_rit(self, reply):
        try:
            hz = int(reply)
        except ValueError:
            return
        self.rit_display_var.set(f"{hz:+d} Hz" if hz != 0 else "0 Hz (off)")
        # Same "don't clobber an in-progress edit" guard as the frequency
        # entry above - matters here in particular right after an F,
        # since radio_tune_to() auto-clears RIT server-side and the next
        # poll tick will otherwise stomp whatever the operator just typed.
        if not self.rit_entry_focused:
            self.rit_entry_var.set(str(hz))

    def apply_volume(self, reply):
        try:
            pct = round(float(reply) * 100)
        except ValueError:
            return
        self.vol_var.set(pct)
        self.vol_label.configure(text=f"{pct}%")

    def apply_micgain(self, reply):
        # No _syncing guard needed, same reasoning as apply_volume above -
        # this Scale only ever sends on a real <ButtonRelease-1> click,
        # not a variable-write trace, so a poll-driven .set() here can't
        # loop back into on_micgain_released().
        try:
            gain = float(reply)
        except ValueError:
            return
        self.micgain_var.set(gain)
        self.micgain_label.configure(text=f"{gain:.2f}x")

    def apply_power(self, reply):
        # rigctld reports RFPOWER as a 0.0-1.0 fraction of max_power; the
        # slider is the same thing as a percentage. No _syncing guard, for
        # the same reason as apply_micgain above.
        try:
            fraction = float(reply)
        except ValueError:
            return
        self.power_var.set(fraction * 100.0)
        self.power_label.configure(text=f"{fraction * 100.0:.0f}%")

    def apply_alc(self, reply):
        # Read-only meter: dB of gain reduction, peak-held by the daemon
        # so it's legible at this poll rate (sound.h's sound_get_alc_db()).
        try:
            db = float(reply)
        except ValueError:
            return
        self.alc_bar.configure(value=min(db, ALC_METER_MAX_DB))
        self.alc_label.configure(text=f"{db:.1f} dB")

    def apply_strength(self, reply):
        try:
            db = int(reply)
        except ValueError:
            return
        self.smeter_label_var.set(f"{s_unit_label(db)} ({db:+d}dB)")
        self.draw_smeter(db)

    def draw_smeter(self, db):
        """Draws the S-meter bar. db=None (not connected / no reading yet)
        just shows the empty scale. Range and tick points match
        rx_audio.c's RX_STRENGTH_MIN_DB/MAX_DB and the l STRENGTH
        convention (0 = S9, 6dB/S-unit below it, "S9+NdB" above) - see
        this file's module docstring and rx_audio_get_strength_db()'s own
        comment for why this reading is relative/uncalibrated, not a
        wattmeter-grade dBm number."""
        canvas = self.smeter_canvas
        w, h = self.smeter_canvas_w, self.smeter_canvas_h
        canvas.delete("all")

        span = STRENGTH_MAX_DB - STRENGTH_MIN_DB

        def x_of(value_db):
            frac = (value_db - STRENGTH_MIN_DB) / span
            return max(0.0, min(1.0, frac)) * w

        # S9 boundary: green (S-units) to the left, amber ("+dB") to the
        # right - the same two-tone convention a real analog S-meter face
        # uses.
        s9_x = x_of(0)
        canvas.create_rectangle(0, 10, s9_x, h - 10, fill="#1a4", outline="")
        canvas.create_rectangle(s9_x, 10, w, h - 10, fill="#a71", outline="")

        # Tick marks + labels across the S-unit and "+dB" zones.
        for tick_db, label in ((-48, "S1"), (-36, "S3"), (-24, "S5"),
                                 (-12, "S7"), (0, "S9"), (20, "+20"), (40, "+40")):
            x = x_of(tick_db)
            canvas.create_line(x, 10, x, h - 10, fill="#000", width=1)
            canvas.create_text(x, h - 2, text=label, fill="#ccc",
                                 font=("monospace", 7), anchor="s")

        # Current reading - a bright vertical needle/cursor, matching the
        # spectrum canvas's own dial-center marker style.
        if db is not None:
            x = x_of(db)
            canvas.create_line(x, 4, x, h - 4, fill="#fff", width=2)

    def apply_mode(self, reply):
        # hamlib.c's "m" replies with TWO lines - mode name, then a
        # cosmetic passband number (see its own comment) - both written
        # in one send_line() call server-side, so RigctlClient.query()'s
        # generic "read until the buffer ends in a newline" loop almost
        # always gets both in one recv() and returns them joined by an
        # internal "\n" (e.g. "CW\n2400"). Only the first line matters
        # here. (In the unlikely event the two lines arrive in separate
        # TCP segments, query() would return just the first line early -
        # still fine for this parse, though the leftover second line
        # would then desync the NEXT command's reply; every command this
        # panel sends is short enough, on a local/LAN connection, that
        # this hasn't been observed in practice - flagged rather than
        # silently assumed away.)
        name = reply.split("\n")[0].strip()
        # hamlib.c's mode_to_name() reports RADIO_MODE_DIGITAL as
        # "PKTUSB" - the real Hamlib name, kept on the wire for any
        # genuine Hamlib client that might query over this surface -
        # never as this panel's own friendlier "DIGITAL" label. Map it
        # back here so the radio button actually reflects DIGITAL once
        # selected, instead of never matching anything in the tuple
        # below and silently freezing on whatever mode was last
        # displayed. (The other half of this fix, accepting "DIGITAL" as
        # an alias on the way in, is hamlib.c's name_to_mode().)
        if name == "PKTUSB":
            name = "DIGITAL"
        if name not in ("CW", "CWR", "USB", "LSB", "DIGITAL"):
            return
        self._syncing_mode = True
        self.mode_var.set(name)
        self._syncing_mode = False
        self.set_rx_filter_gate(name)

    # The server holds stage 3 out of circuit in DIGITAL and refuses to put it
    # back until the mode changes (rx_audio.h, radio.c's radio_set_mode()), so
    # these controls would be writing to a setting the server is overriding.
    # Disabled and retitled rather than hidden: an operator who came looking
    # for the CW filter should find out why it isn't available, not find the
    # group missing.
    #
    # This only reflects the server's decision - it does not enforce anything.
    # The enforcement is deliberately server-side, because WSJT-X sets the mode
    # over CAT itself and never opens this panel; a guard that lived here would
    # be bypassed by exactly the client the protection is for.
    #
    # The CW filter checkbox itself is left to the u NARROW poll, which reports
    # the EFFECTIVE state: it unchecks on its own in DIGITAL, and comes back
    # checked on the way out if that is what the operator had.
    def set_rx_filter_gate(self, mode_name):
        gated = (mode_name == "DIGITAL")
        state = "disabled" if gated else "!disabled"
        for w in (self.narrow_check,) + self.fftfilt_radios:
            w.state([state])
        # Comboboxes go back to "readonly", not plain enabled - that is what
        # keeps them a dropdown rather than a free-text entry.
        for combo in (self.pitch_combo, self.width_combo):
            combo.configure(state="disabled" if gated else "readonly")
        self.narrow_frame.configure(
            text="RX Filter - held out of circuit in DIGITAL" if gated else "RX Filter")

    def apply_narrow(self, reply):
        try:
            enabled = int(reply) != 0
        except ValueError:
            return
        # Guarded (see self._syncing_narrow's comment in __init__) so this
        # poll-driven readback never re-triggers on_narrow_toggled's own
        # network write.
        self._syncing_narrow = True
        self.narrow_var.set(enabled)
        self._syncing_narrow = False

    def apply_fftfilt(self, reply):
        try:
            use_fft = int(reply) != 0
        except ValueError:
            return
        # Same guard, same reasoning, as apply_narrow() above.
        self._syncing_fftfilt = True
        self.fftfilt_var.set(use_fft)
        self._syncing_fftfilt = False

    # Pitch/width readback. The server owns the list of valid values, so if
    # it reports one this panel's dropdown doesn't offer, widen the dropdown
    # rather than discard the reply - that keeps a regenerated
    # narrow_filter_bank.h visible here instead of invisible.
    def _apply_choice(self, reply, combo, var, flag_name):
        try:
            hz = str(int(float(reply)))
        except ValueError:
            return
        values = list(combo.cget("values"))
        if hz not in values:
            values = sorted(values + [hz], key=int)
            combo.configure(values=tuple(values))
        setattr(self, flag_name, True)
        var.set(hz)
        setattr(self, flag_name, False)

    def apply_pitch(self, reply):
        self._apply_choice(reply, self.pitch_combo, self.pitch_var, "_syncing_pitch")

    def apply_width(self, reply):
        self._apply_choice(reply, self.width_combo, self.width_var, "_syncing_width")

    def apply_tone(self, reply):
        try:
            val = int(reply)
        except ValueError:
            return  # an older maxibitx without u TONE replies RPRT -1
        self._syncing_tone = True
        self.tone_var.set(val)
        self._syncing_tone = False

    # Keyer readbacks. An older maxibitx without the keyer replies RPRT -1 to
    # these, which doesn't parse and is ignored.
    def apply_keyer(self, reply):
        try:
            idx = int(reply)
        except ValueError:
            return
        if 0 <= idx < len(KEYER_MODES):
            self._syncing_keyer = True
            self.keyer_var.set(KEYER_MODES[idx])
            self._syncing_keyer = False

    def apply_wpm(self, reply):
        if self.wpm_focused:
            return  # the operator is typing a speed
        try:
            wpm = int(float(reply))
        except ValueError:
            return
        self.wpm_var.set(str(wpm))

    def apply_padrev(self, reply):
        try:
            on = int(reply) != 0
        except ValueError:
            return
        self._syncing_padrev = True
        self.padrev_var.set(on)
        self._syncing_padrev = False

    def apply_morse(self, reply):
        try:
            busy = int(reply) != 0
        except ValueError:
            return
        if busy:
            self.cw_text_status_var.set("sending")
        elif self.cw_text_status_var.get() == "sending":
            self.cw_text_status_var.set("")  # a "refused" stays until the next Send

    def apply_ptt(self, reply):
        try:
            on = int(reply) != 0
        except ValueError:
            return
        self._syncing_ptt = True
        self.ptt_var.set(on)
        self._syncing_ptt = False

    # ---- user actions ----

    def on_tune_clicked(self):
        if not self.client.connected():
            return
        try:
            hz = int(self.freq_entry_var.get().strip())
        except ValueError:
            messagebox.showerror("maxibitx panel", "Frequency must be a whole number of Hz.")
            return
        threading.Thread(target=lambda: self.client.query(f"F {hz}"), daemon=True).start()

    def on_step_clicked(self, delta):
        if not self.client.connected():
            return
        try:
            hz = int(self.freq_entry_var.get().strip())
        except ValueError:
            return
        hz = max(0, hz + delta)
        self.freq_entry_var.set(str(hz))
        threading.Thread(target=lambda: self.client.query(f"F {hz}"), daemon=True).start()

    def on_rit_set_clicked(self):
        if not self.client.connected():
            return
        try:
            hz = int(self.rit_entry_var.get().strip())
        except ValueError:
            messagebox.showerror("maxibitx panel", "RIT must be a whole number of Hz.")
            return
        hz = max(-RIT_MAX_HZ, min(RIT_MAX_HZ, hz))
        self.rit_entry_var.set(str(hz))
        threading.Thread(target=lambda: self.client.query(f"J {hz}"), daemon=True).start()

    def on_rit_clear_clicked(self):
        if not self.client.connected():
            return
        self.rit_entry_var.set("0")
        threading.Thread(target=lambda: self.client.query("J 0"), daemon=True).start()

    def on_rit_step_clicked(self, delta):
        if not self.client.connected():
            return
        try:
            hz = int(self.rit_entry_var.get().strip())
        except ValueError:
            hz = 0
        hz = max(-RIT_MAX_HZ, min(RIT_MAX_HZ, hz + delta))
        self.rit_entry_var.set(str(hz))
        threading.Thread(target=lambda: self.client.query(f"J {hz}"), daemon=True).start()

    def on_volume_dragged(self, _value):
        # Live label update while dragging; see on_volume_released for
        # when the L AF command actually goes out.
        pct = int(round(self.vol_var.get()))
        self.vol_label.configure(text=f"{pct}%")

    def on_volume_released(self, _event):
        if not self.client.connected():
            return
        pct = int(round(self.vol_var.get()))
        val = pct / 100.0
        threading.Thread(target=lambda: self.client.query(f"L AF {val:.3f}"), daemon=True).start()

    def on_micgain_dragged(self, _value):
        # Live label update while dragging; see on_micgain_released for
        # when the L MICGAIN command actually goes out.
        gain = self.micgain_var.get()
        self.micgain_label.configure(text=f"{gain:.2f}x")

    def on_micgain_released(self, _event):
        if not self.client.connected():
            return
        gain = self.micgain_var.get()
        threading.Thread(target=lambda: self.client.query(f"L MICGAIN {gain:.3f}"),
                          daemon=True).start()

    def on_power_dragged(self, _value):
        # Live label update while dragging; on_power_released sends it.
        self.power_label.configure(text=f"{self.power_var.get():.0f}%")

    def on_power_released(self, _event):
        if not self.client.connected():
            return
        fraction = self.power_var.get() / 100.0
        threading.Thread(target=lambda: self.client.query(f"L RFPOWER {fraction:.3f}"),
                          daemon=True).start()

    def on_mode_changed(self):
        if self._syncing_mode:
            return  # apply_mode() is syncing from a poll reply, not an operator click
        # Gate the RX Filter group on this click rather than waiting for the
        # next poll to report the mode back: the operator is looking at those
        # controls now, and a second of them still appearing live in DIGITAL
        # invites exactly the wrong conclusion about what the server is doing.
        # Ahead of the connected() check below, because the radio button has
        # already moved whether or not there is a server to tell.
        self.set_rx_filter_gate(self.mode_var.get())
        if not self.client.connected():
            return
        name = self.mode_var.get()
        threading.Thread(target=lambda: self.client.query(f"M {name} 2400"),
                          daemon=True).start()

    def on_narrow_toggled(self):
        if self._syncing_narrow:
            return  # apply_narrow() is syncing from a poll reply, not an operator click
        if not self.client.connected():
            return
        enable = 1 if self.narrow_var.get() else 0
        threading.Thread(target=lambda: self.client.query(f"U NARROW {enable}"),
                          daemon=True).start()

    def on_tone_changed(self):
        if self._syncing_tone or not self.client.connected():
            return
        val = self.tone_var.get()
        threading.Thread(target=lambda: self.client.query(f"U TONE {val}"),
                          daemon=True).start()

    def on_ptt_toggled(self):
        if self._syncing_ptt or not self.client.connected():
            return
        on = 1 if self.ptt_var.get() else 0
        threading.Thread(target=lambda: self.client.query(f"T {on}"),
                          daemon=True).start()

    def on_fftfilt_toggled(self):
        if self._syncing_fftfilt:
            return  # apply_fftfilt() is syncing from a poll reply, not an operator click
        if not self.client.connected():
            return
        use_fft = 1 if self.fftfilt_var.get() else 0
        threading.Thread(target=lambda: self.client.query(f"U FFTFILT {use_fft}"),
                          daemon=True).start()

    def on_keyer_selected(self, _event=None):
        if self._syncing_keyer or not self.client.connected():
            return
        idx = KEYER_MODES.index(self.keyer_var.get())
        threading.Thread(target=lambda: self.client.query(f"U KEYER {idx}"),
                          daemon=True).start()

    def on_wpm_changed(self):
        if not self.client.connected():
            return
        try:
            wpm = int(float(self.wpm_var.get()))
        except ValueError:
            return  # not a number; the next poll puts the server's value back
        wpm = max(1, min(60, wpm))
        threading.Thread(target=lambda: self.client.query(f"L KEYSPD {wpm}"),
                          daemon=True).start()

    def on_wpm_focus_out(self, _event=None):
        self.wpm_focused = False
        self.on_wpm_changed()

    def on_cw_send_clicked(self):
        text = self.cw_text_var.get().strip()
        if not text or not self.client.connected():
            return

        def send():
            reply = self.client.query(f"b {text}")
            if reply is not None and reply.strip() != "RPRT 0":
                # Refused: not in CW/CWR, the queue is full, or the keyer was
                # built without text (the server's console says which).
                self.after(0, lambda: self.cw_text_status_var.set("refused"))
            else:
                self.after(0, lambda: self.cw_text_status_var.set("sending"))
        threading.Thread(target=send, daemon=True).start()

    def on_cw_stop_clicked(self):
        if not self.client.connected():
            return
        threading.Thread(target=lambda: self.client.query("\\stop_morse"),
                          daemon=True).start()

    def on_padrev_toggled(self):
        if self._syncing_padrev or not self.client.connected():
            return
        on = 1 if self.padrev_var.get() else 0
        threading.Thread(target=lambda: self.client.query(f"U PADREV {on}"),
                          daemon=True).start()

    def on_pitch_selected(self, _event=None):
        if self._syncing_pitch or not self.client.connected():
            return
        hz = self.pitch_var.get()
        threading.Thread(target=lambda: self.client.query(f"L CWPITCH {hz}"),
                          daemon=True).start()

    def on_width_selected(self, _event=None):
        if self._syncing_width or not self.client.connected():
            return
        hz = self.width_var.get()
        threading.Thread(target=lambda: self.client.query(f"L CWWIDTH {hz}"),
                          daemon=True).start()

    def on_spectrum_span_changed(self):
        # Nothing to send and nothing to redraw: the span is a display-side
        # crop, and the next redraw_spectrum() tick (at most
        # SPECTRUM_REDRAW_MS, ~66ms) reads the variable itself - faster than
        # anyone notices a button click.
        #
        # Deliberately does NOT call redraw_spectrum() to make it instant:
        # that method ends by scheduling its own self.after(), so calling it
        # from here would start a second redraw chain alongside the running
        # one and permanently double the frame rate. Kept as a named handler
        # rather than dropping the radio buttons' command= so that reason is
        # written down where someone would otherwise add the call.
        #
        # The waterfall's history is at the old span, so it starts again.
        self.clear_waterfall()

    def clear_waterfall(self):
        self.waterfall_rows[:] = 0
        self.show_waterfall()

    def show_waterfall(self):
        rows = self.waterfall_rows
        header = b"P6 %d %d 255\n" % (rows.shape[1], rows.shape[0])
        self.waterfall_image.configure(data=header + rows.tobytes(), format="PPM")

    def add_waterfall_row(self, frac):
        """frac: the displayed bins, 0 (SPECTRUM_DB_FLOOR) to 1 (ceiling)."""
        w = self.waterfall_rows.shape[1]
        # Each pixel shows the strongest bin under it: at +-15kHz there are
        # more bins than pixels, and averaging would fade a narrow CW signal.
        # At the narrow spans a bin covers several pixels and is repeated.
        edges = (np.arange(w) * len(frac)) // w
        row = np.maximum.reduceat(frac, edges)
        self.waterfall_rows[1:] = self.waterfall_rows[:-1]  # numpy copes with the overlap
        self.waterfall_rows[0] = self.waterfall_palette[(row * 255.0).astype(np.intp)]
        self.show_waterfall()

    # ---- spectrum ----
    #
    # Runs entirely on the main/Tk thread via self.after() - SpectrumClient
    # does its own socket work and FFT math on its own threads and just
    # hands back a finished numpy array through get_latest(); this method
    # only ever touches the Canvas, never blocks.

    def redraw_spectrum(self):
        if not self.spectrum_running:
            return  # disconnect() already cleared the canvas
        if not self.client.connected():
            self.spectrum_running = False
            return

        db = self.spectrum.get_latest()
        canvas = self.spectrum_canvas
        w, h = self.spectrum_canvas_w, self.spectrum_canvas_h
        canvas.delete("all")

        if db is None:
            self.spectrum_status_var.set(
                f"waiting for I/Q telemetry on UDP {IQ_STREAM_PORT} "
                "(older maxibitx builds without iq_stream.c won't send any)")
        else:
            # db spans the full native +-48kHz (fftshifted, bin 0 = -48kHz,
            # bin FFT_SIZE/2 = dial center) - crop to the middle
            # +-(the selected span) for display (see
            # SPECTRUM_SPAN_CHOICES_HZ's comment on why, and on why this is
            # a crop rather than a zoom).
            bin_hz = 96000.0 / FFT_SIZE
            center = len(db) // 2
            # At least 2 bins each side even if someone adds a tiny span to
            # the tuple: create_line needs two points, and a one-bin crop
            # would raise rather than just look odd.
            half_bins = min(max(int(round(self.spectrum_span_var.get() / bin_hz)), 2), center)
            db = db[center - half_bins:center + half_bins]

            n = len(db)
            xs = np.arange(n) * (w / n)
            peak_db, floor_db = float(np.max(db)), float(np.min(db))
            clipped = np.clip(db, SPECTRUM_DB_FLOOR, SPECTRUM_DB_CEILING)
            frac = (clipped - SPECTRUM_DB_FLOOR) / (SPECTRUM_DB_CEILING - SPECTRUM_DB_FLOOR)
            self.add_waterfall_row(frac)
            ys = h - frac * h
            coords = np.empty(n * 2)
            coords[0::2] = xs
            coords[1::2] = ys
            canvas.create_line(*coords.tolist(), fill="#3fa", width=1)

            # A trace against the top is flat-topped, not peaking - say so
            # where it is happening rather than only in the status line, since
            # a clipped peak and a genuinely strong one look identical.
            # SPECTRUM_DB_CEILING is the knob; its comment has the trade.
            if peak_db >= SPECTRUM_DB_CEILING:
                canvas.create_line(0, 1, w, 1, fill="#f55", width=1)
                canvas.create_text(w - 4, 8, text="clipping", anchor="e",
                                    fill="#f55", font=("monospace", 8))

            # Dial-center marker plus frequency ticks across the displayed
            # span.
            span = half_bins * bin_hz  # actual displayed half-span - the
                                        # selected value snapped to a whole
                                        # number of bins, so the tick labels
                                        # state what is really on screen
            # The centre marker is the one reference the whole display is read
            # against - where the dial is, and so where a zero-beat CW signal
            # or an SSB suppressed carrier belongs. It was a dim grey dash
            # that disappeared into the trace. Bright amber reads clearly
            # against both the green trace and the dark ground, and the
            # triangle at the bottom stays visible even where a strong signal
            # sits right on top of the line.
            #
            # Still dashed, deliberately: a solid line down the middle would
            # mask the very signal an operator is trying to centre under it.
            canvas.create_line(w / 2, 0, w / 2, h, fill="#fb3", dash=(4, 4))
            canvas.create_polygon(w / 2, h - 11, w / 2 - 6, h, w / 2 + 6, h,
                                   fill="#fb3", outline="")
            # No "dial" text at the centre any more - the triangle says it,
            # and the two would have overlapped at h-8.
            # The two end labels are anchored to the edge they sit against,
            # not centred on it. Centred-and-clamped loses half the text off
            # the canvas, which an earlier version did invisibly while the
            # widest label was only "15k" - "-2.5k" made it obvious.
            for offset_hz, anchor in ((-span, "w"), (-span / 2, "center"),
                                       (span / 2, "center"), (span, "e")):
                label = ("+" if offset_hz > 0 else "-") + span_khz_label(abs(offset_hz))
                x = (offset_hz + span) / (2 * span) * w
                x = 2 if anchor == "w" else (w - 2 if anchor == "e" else x)
                canvas.create_text(x, h - 8, text=label, anchor=anchor,
                                    fill="#999", font=("monospace", 8))

            freq_note = f" (dial {self.current_freq_hz:,} Hz)".replace(",", ".") \
                if self.current_freq_hz is not None else ""
            # Peak and floor of what is actually on screen (pre-clip), so the
            # dB window above can be chosen from measurements rather than
            # guesses. The bin count is here because it is the honest measure
            # of how much detail a narrow span is really showing - the FFT
            # does not get finer when the display is cropped. The span itself
            # is NOT repeated here: the buttons and the tick labels both
            # already state it, and this line has to stay inside one
            # wraplength - a second line pushes every group below it down.
            self.spectrum_status_var.set(
                f"live - {n} bins, {bin_hz:.1f} Hz/bin, "
                f"peak {peak_db:.1f}, floor {floor_db:.1f} dB{freq_note}")

        self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)


if __name__ == "__main__":
    Panel().mainloop()
