#!/usr/bin/env python3
"""
rigctl_panel.py - a small standalone control panel for maxibitx.

Talks the same plain-text rigctld protocol WSJT-X/Thetis/etc. already use
against maxibitx's hamlib.c server (default TCP 4532 - see
docs/04_remote_control_and_iq_output.md) - nothing here is maxibitx-specific
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
