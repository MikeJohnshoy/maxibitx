#!/usr/bin/env python3
"""
rigctl_panel.py - a touch-friendly control panel for maxibitx, laid out
for the sBitx's 7-inch 800x480 screen and just as usable in a window on
a laptop.

It is a client, separate from the maxibitx binary. Control goes over
maxibitx's rigctld server (hamlib.c, TCP 4532), the spectrum and
waterfall over its I/Q telemetry stream (iq_stream.c, UDP 4536).
docs/06_api.md describes both.

The screen, top to bottom:

    band menu, frequency, mode (CW CWR USB LSB DIGI), RX/TX, volume, gear
    RIT and its steps          |  FILTER: on, width, centre, APF, NR
    span                                   status              S-meter
    spectrum, frequency scale, waterfall
    CW: ten macros  |  USB/LSB: mic gain and ALC  |  DIGI: a note
    key type, WPM, CW text, Send, Stop, power, Tune

The frequency is tuned digit by digit: tap a digit to make it the tuning
step, then turn the mouse wheel, drag up or down, or use the arrow keys.
Double-click (or double-tap) the frequency to type one in.

The gear (and the RX/TX button) opens Settings: the connection, the CW
filter type, paddle reversal, the TX test tones, the macro texts, and
the macro fields. A field is a name and a value, MYCALL and PARK to
start with, up to eight; a macro's {NAME} is replaced by the value, and
a macro naming a field with no value isn't sent.

The trace is the average of the FFTs since the last frame, about six,
so the noise floor holds steady without signals widening; a dimmer peak
line behind it falls back 15 dB a second (off in Settings if unwanted).

While RIT is set, a green marker on the spectrum, scale and waterfall
shows where the receiver is listening. The display stays centred on the
dial (the transmit frequency), so a signal doesn't move when RIT does;
an arrow at the edge shows an offset beyond the span. APF, NR and XIT
are on the screen but disabled: maxibitx has none of them yet.

TUNE keys the transmitter with the 1 kHz test tone (u TONE 1, then
T 1), so the carrier is 1 kHz from the dial; maxibitx drops it after
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
import math
import os
import re
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
# The frequency alone is read this often, so a turn of the sBitx's tuning
# knob shows at once.
FREQ_POLL_INTERVAL_S = 0.25
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

# Macro texts may use {NAME} for any field defined in Settings (MYCALL and
# PARK to start with); MAX_FIELDS rows are offered there.
MAX_FIELDS = 8
DEFAULT_FIELDS = (("MYCALL", ""), ("PARK", ""))
FIELD_RE = re.compile(r"\{([A-Za-z0-9_]+)\}")
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
FFT_HOP = FFT_SIZE // 2        # half-overlapped FFTs: about 94 a second
SPECTRUM_REDRAW_MS = 66        # about 15 frames a second, each the average of ~6 FFTs
# The peak line: each bin holds its highest level and falls back at this
# rate, so a CW station's dits leave a steady outline between them.
PEAK_DECAY_DB_PER_S = 15.0
PEAK_COLOR = "#4D7A8F"         # TRACE, dimmed
SPECTRUM_DB_FLOOR = -100.0     # dBFS-style: 0 dB is one full-scale tone
# Not 0 dBFS: nothing on this receiver's raw I/Q comes near full scale.
# -37.5 was set from the strongest signal first measured on an sBitx
# (-50 dBFS); strong FT8 went past it, so -30. A signal above the
# ceiling flat-tops and the trace says "over scale": the display's
# limit, not the radio's - the codec's is 0 dBFS, 30 dB higher.
SPECTRUM_DB_CEILING = -30.0
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
RIT_GREEN = "#7FE0A0"            # the RIT marker; XIT will want its own colour


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


def field_name(text):
    """A field name as macros spell it: upper case letters, digits and _,
    without the braces."""
    return re.sub(r"[^A-Z0-9_]", "", text.upper())


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
    """Subscribes to iq_stream.c's UDP stream and FFTs it, FFT_SIZE samples
    at a time, every FFT_HOP. get_latest() returns the average power of the
    FFTs since the last call: averaged over time, never across bins, so the
    noise floor steadies and signals stay as narrow as one FFT shows them.

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
        self.power_sum = None  # FFT power summed since the last get_latest()
        self.power_count = 0
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
            self.power_sum = None
            self.power_count = 0
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
            if len(self.sample_buf) > FFT_SIZE * 4:   # behind: drop the oldest
                self.sample_buf = self.sample_buf[-FFT_SIZE:]
            while len(self.sample_buf) >= FFT_SIZE:
                block = self.sample_buf[:FFT_SIZE]
                self.sample_buf = self.sample_buf[FFT_HOP:]
                spectrum = np.fft.fftshift(np.fft.fft(block * self.window))
                power = (np.abs(spectrum) / (FFT_SIZE * self.window_mean)) ** 2
                with self.lock:
                    if self.power_sum is None:
                        self.power_sum = power
                    else:
                        self.power_sum += power
                    self.power_count += 1

    def get_latest(self):
        """The average since the last call, in dB; the previous one again
        if no FFT has finished since."""
        with self.lock:
            if self.power_count:
                self.latest_db = 10.0 * np.log10(self.power_sum / self.power_count + 1e-24)
                self.power_sum = None
                self.power_count = 0
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
    s.configure("MuteOn.TLabel", foreground=AMBER, font=(ui, 10, "bold"))
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
    called with every new frequency, on_step(hz) when the operator picks a
    new step, on_type() for the double-click.
    """

    DIGIT_W = 20
    SEP_W = 10
    DRAG_STEP_PX = 14

    def __init__(self, parent, mono, on_tune, on_type, on_step=None, **kw):
        super().__init__(parent, width=200, height=44, bg=GROUND, highlightthickness=0,
                         takefocus=1, **kw)
        self.font = (mono, 24, "bold")
        self.on_tune = on_tune
        self.on_type = on_type
        self.on_step = on_step
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

    def set_step(self, step):
        """The radio's step, which its tuning knob can change too."""
        if step != self.step and 1 <= step <= 10000000:
            self.step = step
            self.draw()

    def _pick_step(self, step):
        self.step = step
        self.draw()
        if self.on_step:
            self.on_step(step)

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
            self._pick_step(step)

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
                self._pick_step(place)
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


class GearButton(tk.Canvas):
    """The settings button, top right: a gear drawn on a canvas rather
    than a font glyph, which not every font on the Pi has."""

    SIZE = 44

    def __init__(self, parent, command):
        n = self.SIZE
        super().__init__(parent, width=n, height=n, bg=GROUND, highlightthickness=0,
                         cursor="hand2")
        self.command = command
        self.box = self.create_rectangle(0, 0, n - 1, n - 1, fill=TILE, outline=LINE)
        # Eight teeth, each narrowing from 56% of its 45-degree pitch at
        # radius 9.5 to 36% at radius 13.
        c, pts, pitch = n / 2, [], 2 * math.pi / 8
        for i in range(8):
            a = i * pitch
            for frac, r in ((-0.28, 9.5), (-0.18, 13), (0.18, 13), (0.28, 9.5)):
                pts += [c + r * math.cos(a + frac * pitch), c + r * math.sin(a + frac * pitch)]
        self.gear = self.create_polygon(pts, fill=MUTED, outline="")
        self.hole = self.create_oval(c - 4.5, c - 4.5, c + 4.5, c + 4.5, fill=TILE, outline="")
        self.bind("<Enter>", lambda e: self.paint(TILE_HI, TEXT))
        self.bind("<Leave>", lambda e: self.paint(TILE, MUTED))
        self.bind("<ButtonPress-1>", lambda e: self.paint(AMBER, INK))
        self.bind("<ButtonRelease-1>", self.on_release)

    def paint(self, ground, ink):
        self.itemconfigure(self.box, fill=ground)
        self.itemconfigure(self.hole, fill=ground)
        self.itemconfigure(self.gear, fill=ink)

    def on_release(self, event):
        self.paint(TILE, MUTED)
        if 0 <= event.x < self.SIZE and 0 <= event.y < self.SIZE:
            self.command()


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
        nb.add(self.build_fields_tab(nb), text="Fields")
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

        disp = ttk.LabelFrame(f, text="Spectrum", padding=8)
        disp.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        ttk.Checkbutton(disp, text="Peak line", variable=p.peak_var, style="Toolbutton",
                        command=p.save_settings).grid(row=0, column=0, sticky="w")
        ttk.Label(disp, text="each frequency's highest level, falling back "
                  f"{PEAK_DECAY_DB_PER_S:g} dB a second", style="PanelMuted.TLabel").grid(
            row=0, column=1, sticky="w", padx=(10, 0))

        f.columnconfigure(0, weight=1)
        f.columnconfigure(1, weight=1)
        return f

    def build_macro_tab(self, nb):
        p = self.panel
        f = ttk.Frame(nb, padding=8)
        ttk.Label(f, text="Button", style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(f, text="Text sent: {NAME} is filled in from the Fields tab",
                  style="Muted.TLabel").grid(row=0, column=1, sticky="w", padx=(6, 0))
        for i, (lab_var, text_var) in enumerate(p.macro_vars):
            ttk.Entry(f, textvariable=lab_var, width=9).grid(row=i + 1, column=0, pady=1)
            ttk.Entry(f, textvariable=text_var).grid(row=i + 1, column=1, sticky="ew",
                                                    padx=(6, 0), pady=1)
        f.columnconfigure(1, weight=1)
        return f

    def build_fields_tab(self, nb):
        p = self.panel
        f = ttk.Frame(nb, padding=8)
        ttk.Label(f, text="Field", style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(f, text="Value: a macro's {FIELD} becomes this", style="Muted.TLabel").grid(
            row=0, column=1, sticky="w", padx=(6, 0))
        for i, (name_var, value_var) in enumerate(p.field_vars):
            e = ttk.Entry(f, textvariable=name_var, width=10)
            e.grid(row=i + 1, column=0, pady=1)
            # Show the name as it will be stored: "park" becomes PARK.
            e.bind("<FocusOut>", lambda _e, v=name_var: v.set(field_name(v.get())))
            ttk.Entry(f, textvariable=value_var).grid(row=i + 1, column=1, sticky="ew",
                                                     padx=(6, 0), pady=1)
        ttk.Label(f, text="Names are upper case letters, digits and _. An empty name is unused.",
                  style="Muted.TLabel").grid(row=MAX_FIELDS + 1, column=0, columnspan=2,
                                             sticky="w", pady=(6, 0))
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
        self.step_hold_until = 0.0
        self.mute_hold_until = 0.0
        self.muted = False
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
        fields = cfg.get("fields")
        if fields is None:   # a config from before fields: its call and park
            fields = [["MYCALL", cfg.get("mycall", "")], ["PARK", cfg.get("park", "")]]
        fields = (list(fields) + [["", ""]] * MAX_FIELDS)[:MAX_FIELDS]
        self.field_vars = [(tk.StringVar(value=n), tk.StringVar(value=v)) for n, v in fields]
        macros = cfg.get("macros") or [list(m) for m in DEFAULT_MACROS]
        macros = (list(macros) + [list(m) for m in DEFAULT_MACROS])[:len(DEFAULT_MACROS)]
        self.macro_vars = [(tk.StringVar(value=lab), tk.StringVar(value=txt)) for lab, txt in macros]
        self.band_memory = dict(cfg.get("band_memory", {}))
        self.peak_var = tk.BooleanVar(value=cfg.get("peak_line", True))
        self.peak_db = None        # the peak line's levels, display bins
        self.peak_key = None       # (dial, bins) the peak line was built for
        self.peak_time = 0.0

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
        self.freq_display = FreqDisplay(f, self.mono, self.on_freq_tuned, self.on_freq_type,
                                        self.on_step_picked)
        self.freq_display.grid(row=0, column=0)
        ttk.Label(f, text="MHz", style="Muted.TLabel").grid(row=0, column=1, sticky="s", pady=(0, 6))
        modes = ttk.Frame(top)
        modes.grid(row=0, column=2, padx=4)
        self.mode_buttons = []
        for i, m in enumerate(MODES):
            b = ttk.Radiobutton(modes, text=m, value=m, variable=self.mode_var, style="Toolbutton",
                                width=-4, command=self.on_mode_changed)
            b.grid(row=0, column=i)
            self.mode_buttons.append(b)
        self.rx_button = ttk.Button(top, text="OFF", style="Off.TButton", width=4,
                                    command=self.open_settings)
        self.rx_button.grid(row=0, column=3, padx=4)
        vol = ttk.Frame(top)
        vol.grid(row=0, column=4)
        # Tap the caption to mute; it reads MUTE while muted.
        self.vol_caption = ttk.Label(vol, text="VOL", style="Muted.TLabel", width=5, anchor="e",
                                     cursor="hand2")
        self.vol_caption.grid(row=0, column=0, padx=(0, 4))
        self.vol_caption.bind("<Button-1>", lambda e: self.send_mute(not self.muted))
        self.vol_scale = ttk.Scale(vol, from_=0, to=100, variable=self.vol_var, length=50,
                                   command=lambda v: self.vol_label.configure(
                                       text=f"{round(float(v))}"))
        self.vol_scale.grid(row=0, column=1)
        self.vol_scale.bind("<ButtonRelease-1>", self.on_volume_released)
        self.vol_label = ttk.Label(vol, text="--", style="Mono.TLabel", width=3)
        self.vol_label.grid(row=0, column=2, padx=(4, 0))
        GearButton(top, self.open_settings).grid(row=0, column=5, padx=(4, 0))

    def build_controls(self):
        row = ttk.Frame(self, padding=(6, 4, 6, 0))
        row.grid(row=1, column=0, sticky="ew")
        row.columnconfigure(1, weight=1)
        rit = ttk.LabelFrame(row, text="RIT", padding=(3, 0, 3, 4))
        rit.grid(row=0, column=0, sticky="nsw")
        self.rit_frame = rit
        ttk.Radiobutton(rit, text="RIT", value="RIT", variable=self.ritmode_var,
                        style="Toolbutton", width=-3).grid(row=0, column=0, padx=1)
        xit = ttk.Radiobutton(rit, text="XIT", value="XIT", variable=self.ritmode_var,
                              style="Toolbutton", width=-3)
        xit.grid(row=0, column=1, padx=1)
        xit.state(["disabled"])    # maxibitx has no XIT yet
        for i, (t, d) in enumerate((("−100", -100), ("−10", -10))):
            ttk.Button(rit, text=t, style="Step.TButton", width=4,
                       command=lambda d=d: self.on_rit_step(d)).grid(row=0, column=2 + i, padx=1)
        self.rit_value = ttk.Button(rit, text="0 Hz", style="Value.TButton", width=-7,
                                    command=lambda: self.send_rit(0))
        self.rit_value.grid(row=0, column=4, padx=1, sticky="ns")
        for i, (t, d) in enumerate((("+10", 10), ("+100", 100))):
            ttk.Button(rit, text=t, style="Step.TButton", width=4,
                       command=lambda d=d: self.on_rit_step(d)).grid(row=0, column=5 + i, padx=1)

        flt = ttk.LabelFrame(row, text="FILTER", padding=(4, 0, 4, 4))
        flt.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        self.filter_frame = flt
        self.narrow_check = ttk.Checkbutton(flt, text="ON", variable=self.narrow_var,
                                            style="Toolbutton", width=-3,
                                            command=self.on_narrow_toggled)
        self.narrow_check.grid(row=0, column=0, padx=(0, 4))
        ttk.Label(flt, text="WIDTH", style="PanelMuted.TLabel").grid(row=0, column=1)
        self.width_combo = ttk.Combobox(flt, textvariable=self.width_var, values=CW_WIDTHS,
                                        width=3, state="readonly", font=(self.mono, 13))
        self.width_combo.grid(row=0, column=2, padx=(3, 4))
        self.width_combo.bind("<<ComboboxSelected>>", self.on_width_selected)
        ttk.Label(flt, text="CENTER", style="PanelMuted.TLabel").grid(row=0, column=3)
        self.pitch_combo = ttk.Combobox(flt, textvariable=self.pitch_var, values=CW_PITCHES,
                                        width=4, state="readonly", font=(self.mono, 13))
        self.pitch_combo.grid(row=0, column=4, padx=(3, 4))
        self.pitch_combo.bind("<<ComboboxSelected>>", self.on_pitch_selected)
        self.apf_check = ttk.Checkbutton(flt, text="APF", variable=self.apf_var,
                                         style="Toolbutton", width=-3)
        self.apf_check.grid(row=0, column=5, padx=(0, 4))
        self.nr_check = ttk.Checkbutton(flt, text="NR", variable=self.nr_var,
                                        style="Toolbutton", width=-3)
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
        ttk.Checkbutton(b, text="TUNE", variable=self.tune_var, style="Toolbutton", width=-5,
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
            self.say("not connected - tap the gear for Settings", 6)
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
            "fields": [[field_name(n.get()), v.get().strip()] for n, v in self.field_vars
                       if field_name(n.get())],
            "macros": [[lab.get(), txt.get()] for lab, txt in self.macro_vars],
            "band_memory": self.band_memory,
            "peak_line": self.peak_var.get(),
        })
        save_config(self.cfg)

    # ---- polling ----
    #
    # The poll runs on its own thread, so a stalled connection can't freeze
    # the screen; every control update it makes is handed to the Tk thread
    # with self.after().

    POLL_COMMANDS = ("f", "j", "l AF", "l MICGAIN", "m", "u NARROW", "u FFTFILT", "l CWPITCH",
                     "l CWWIDTH", "l STRENGTH", "u TONE", "t", "l RFPOWER", "l ALC", "u KEYER",
                     "l KEYSPD", "u PADREV", "u MORSE", "n", "u MUTE")

    def poll_loop(self):
        every = max(1, round(POLL_INTERVAL_S / FREQ_POLL_INTERVAL_S))
        n = 0
        while not self.poll_stop.is_set():
            if not self.client.connected():
                self.after(0, self.disconnect)
                return
            full = n % every == 0
            replies = {}
            for cmd in self.POLL_COMMANDS if full else ("f",):
                r = self.client.query(cmd)
                if r is None:
                    self.after(0, self.disconnect)
                    return
                replies[cmd] = r
            if full:
                self.after(0, lambda r=replies: self.apply_poll(r))
            else:
                self.after(0, lambda r=replies: self.apply_freq(r["f"]))
            n += 1
            time.sleep(FREQ_POLL_INTERVAL_S)

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
            if time.monotonic() >= self.step_hold_until:
                self.apply_int(r["n"], self.freq_display.set_step)
            if time.monotonic() >= self.mute_hold_until:
                self.apply_int(r["u MUTE"], lambda v: self.show_mute(v != 0))
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
        if self.muted:
            self.send_mute(False)   # as turning the radio's volume knob unmutes
        self.query_async(f"L AF {self.vol_var.get() / 100.0:.3f}")

    def send_mute(self, on):
        self.show_mute(on)
        self.mute_hold_until = time.monotonic() + 1.5
        self.query_async(f"U MUTE {1 if on else 0}")

    def show_mute(self, on):
        self.muted = on
        self.vol_caption.configure(text="MUTE" if on else "VOL",
                                   style="MuteOn.TLabel" if on else "Muted.TLabel")

    def on_step_picked(self, step):
        """A digit tapped: it becomes the radio's tuning step too, for its
        tuning knob."""
        self.step_hold_until = time.monotonic() + 1.5
        self.query_async(f"N {step}")

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

    def field_values(self):
        return {field_name(n.get()): v.get().strip() for n, v in self.field_vars
                if field_name(n.get())}

    def send_macro(self, i):
        """Send macro i with its {FIELD}s filled in. A field that isn't
        defined, or has no value, stops it: a CQ without the call in it is
        worse than none."""
        values = self.field_values()
        missing = []

        def fill(m):
            v = values.get(m.group(1).upper(), "")
            if not v:
                missing.append(m.group(1).upper())
            return v
        text = FIELD_RE.sub(fill, self.macro_vars[i][1].get())
        if missing:
            self.say(f"{{{missing[0]}}} has no value - set it in Settings, Fields")
            return
        self.send_cw(text)

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

    def draw_offset_marker(self, w, span_hz, hz, label, color):
        """Where an offset from the dial (RIT now, XIT later) lands: a
        dashed line through the trace and waterfall, a triangle and label
        on the scale. Off the display, an arrow at the edge it's past."""
        c = self.canvas
        c.delete("offset")
        if not hz:
            return
        y0 = TRACE_H
        x = w / 2 + hz / (2.0 * span_hz) * w
        if 0 <= x <= w:
            # Sparse dashes, like the dial's, so the line doesn't hide the
            # signal it's been put on.
            c.create_line(x, 0, x, y0, fill=color, dash=(4, 3), width=1.5, tags="offset")
            c.create_line(x, y0 + SCALE_H, x, self.canvas_h, fill=color, dash=(2, 6),
                          tags="offset")
            c.create_polygon(x - 5, y0 + SCALE_H, x + 5, y0 + SCALE_H, x, y0 + SCALE_H - 7,
                             fill=color, outline="", tags="offset")
            right = x < w - 40
            c.create_text(x + (7 if right else -7), 2, text=label, fill=color,
                          font=(self.mono, 9, "bold"), anchor="nw" if right else "ne",
                          tags="offset")
        else:
            edge = w - 2 if x > w else 2
            point = 1 if x > w else -1
            ym = y0 + SCALE_H / 2
            c.create_polygon(edge, ym, edge - point * 9, ym - 6, edge - point * 9, ym + 6,
                             fill=color, outline="", tags="offset")
            t = c.create_text(edge - point * 12, ym, text=f"{label} {hz:+d}", fill=color,
                              font=(self.mono, 9, "bold"), anchor="e" if x > w else "w",
                              tags="offset")
            x1, y1, x2, y2 = c.bbox(t)
            c.tag_lower(c.create_rectangle(x1 - 3, y0, x2 + 3, y0 + SCALE_H, fill="#101B22",
                                           outline="", tags="offset"), t)

    def update_peak(self, db):
        """The peak line: each bin's highest level, falling back at
        PEAK_DECAY_DB_PER_S. Starts again on a retune or a span change,
        when the old bins no longer line up with the new ones."""
        now = time.monotonic()
        key = (self.current_freq_hz, len(db))
        if self.peak_db is None or key != self.peak_key:
            self.peak_db = db.copy()
            self.peak_key = key
        else:
            fall = PEAK_DECAY_DB_PER_S * min(now - self.peak_time, 1.0)
            self.peak_db = np.maximum(db, self.peak_db - fall)
        self.peak_time = now
        return self.peak_db

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
            # db covers the full +-48 kHz around the receive frequency (bin
            # FFT_SIZE/2), which RIT moves off the dial. The display shows
            # +-span around the dial, so a signal stays put when RIT moves
            # and the RIT marker moves onto it. (rigctld's "j" reports the
            # offset even when CAT or TCI has switched RIT off; the panel
            # then shifts by an offset the radio isn't applying.)
            bin_hz = 96000.0 / FFT_SIZE
            half = len(db) // 2
            half_bins = min(max(int(round(self.half_span() / bin_hz)), 2), half // 2)
            center = half - int(round(self.rit_hz / bin_hz))
            center = min(max(center, half_bins), len(db) - half_bins)
            db = db[center - half_bins:center + half_bins]
            n = len(db)
            peak_db = float(np.max(db))

            def to_frac(levels):
                return (np.clip(levels, SPECTRUM_DB_FLOOR, SPECTRUM_DB_CEILING)
                        - SPECTRUM_DB_FLOOR) / (SPECTRUM_DB_CEILING - SPECTRUM_DB_FLOOR)

            def trace(levels, color, width):
                coords = np.empty(n * 2)
                coords[0::2] = np.arange(n) * (w / n)
                coords[1::2] = TRACE_H - 2 - to_frac(levels) * (TRACE_H - 6)
                c.create_line(*coords.tolist(), fill=color, width=width, tags="trace")

            frac = to_frac(db)
            self.add_waterfall_row(frac)
            if self.peak_var.get():
                trace(self.update_peak(db), PEAK_COLOR, 1)
            trace(db, TRACE, 1.5)
            c.create_line(w / 2, 0, w / 2, TRACE_H, fill=AMBER, width=1.5, tags="trace")
            if peak_db >= SPECTRUM_DB_CEILING:
                c.create_text(w - 4, 8, text="over scale", anchor="e", fill=AMBER,
                              font=(self.mono, 8), tags="trace")
            self.draw_scale(w, half_bins * bin_hz)
            self.draw_offset_marker(w, half_bins * bin_hz, self.rit_hz, "RIT", RIT_GREEN)
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
    main()
