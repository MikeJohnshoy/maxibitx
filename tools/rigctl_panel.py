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
                                Its width is selectable - see CWWIDTH
    t  / T <0|1>           get / set PTT (the TX Test section)
    u TONE / U TONE <0|1|2>  get / set the TX test-tone generator: off,
                              1 kHz, or two-tone 700 + 1900 Hz
    u FFTFILT / U FFTFILT <0|1>  get / set WHICH filter NARROW's "on"
                                  state uses - 1 = the shared FFT filter
                                  (rx_filter.c, the default, always
                                  minimum phase), 0 = an elliptic IIR from
                                  the pre-designed bank. Pitch and width
                                  below apply to either one
    l CWPITCH / L CWPITCH <hz>   get / set stage 3's center pitch - 600,
                                  700 or 800 Hz. Moves the RX BFO with it,
                                  so the tone you hear moves too. A real
                                  Hamlib level
    l CWWIDTH / L CWWIDTH <hz>   get / set stage 3's width - 150, 300, 450
                                  or 600 Hz. Both settings snap to the
                                  nearest value the filter bank carries
                                  (src/narrow_filter_bank.h) and the reply
                                  says which one was selected

It also shows a live spectrum, fed by a second, independent UDP
connection to src/interfaces/iq_stream.c's lightweight I/Q telemetry stream (UDP
port 4536, no relation to rigctld's TCP port above, and no relation to
the HPSDR Protocol 1 link WSJT-X/Thetis use for their own I/Q - see
iq_stream.h/.c's file headers for why this is its own third, minimal
path rather than reusing either). That stream carries the same native
96kHz baseband I/Q docs/02_rx_processing_pipeline.md describes, so what
the spectrum plot shows spans the full ±48kHz around dial center - the
same range docs/dsp_design_notes/antialias_filter_design.md's crystal-
filter analysis and rx_audio_demod_design.md's AGC-placement work were
both reasoning about, now actually visible.

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
# The FFT itself still covers the full native +-48kHz (FFT_SIZE stays
# 2048 either way - resolution is unaffected), but only the middle
# +-15kHz gets drawn: docs/dsp_design_notes/antialias_filter_design.md's
# crystal-filter analysis puts the genuinely flat passband at only
# +-17.4/17.5kHz, with an asymmetric, increasingly attenuated skirt past
# that - so displaying the full +-48kHz mostly just shows the filter's
# own rolloff shape rather than real signal content, which is what
# prompted narrowing this.
SPECTRUM_DISPLAY_HALF_SPAN_HZ = 15000

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
        self.resizable(False, False)

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

        cfg = load_config()

        # --- connection row ---
        conn = ttk.Frame(self, padding=8)
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
        freq = ttk.LabelFrame(self, text="Frequency (Hz)", padding=8)
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
        mode = ttk.LabelFrame(self, text="Mode", padding=8)
        mode.grid(row=2, column=0, sticky="ew", padx=8, pady=4)
        self.mode_var = tk.StringVar(value="CW")
        # CWR is CW-reverse: the same key and the same transmitted
        # carrier as CW, listening on the other side of the BFO to move
        # away from an interfering signal (radio.h).
        for i, name in enumerate(("CW", "CWR", "USB", "LSB", "DIGITAL")):
            ttk.Radiobutton(mode, text=name, value=name, variable=self.mode_var,
                             command=self.on_mode_changed).grid(row=0, column=i, padx=(0 if i == 0 else 10, 0))

        # --- RIT ---
        # rigctld's j/J (radio.c's radio_set_rit()/radio_get_rit()) - a
        # receive-only offset, see this file's module docstring. Unlike
        # the narrow-filter checkbox below, there's no separate on/off
        # bit to sync from the server - "off" IS 0 Hz, same convention
        # real Hamlib rigs use - so Clear is just "J 0" spelled out as
        # its own button for a one-click reset mid-QSO.
        rit = ttk.LabelFrame(self, text="RIT - receive only (Hz)", padding=8)
        rit.grid(row=3, column=0, sticky="ew", padx=8, pady=4)
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
        vol = ttk.LabelFrame(self, text="Volume", padding=8)
        vol.grid(row=4, column=0, sticky="ew", padx=8, pady=(4, 8))
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
        mg = ttk.LabelFrame(self, text="Mic Gain (TX, USB/LSB)", padding=8)
        mg.grid(row=5, column=0, sticky="ew", padx=8, pady=(0, 8))
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
        # function - see hamlib.c's u/U comment).
        nf = ttk.LabelFrame(self, text="RX Filter", padding=8)
        nf.grid(row=6, column=0, sticky="ew", padx=8, pady=(0, 8))
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
        ttk.Checkbutton(nf, text="CW filter", variable=self.narrow_var,
                         command=self.on_narrow_toggled).grid(row=0, column=0, sticky="w")

        # Which filter, as two radio buttons rather than a checkbox: the
        # choice is between two named things, and a checkbox labelled with one
        # of them leaves the other unnamed. rigctld's u/U FFTFILT underneath,
        # where 1 is the FFT filter. Indented under the checkbox above,
        # because it means nothing while the filter is out of circuit.
        #
        # The FFT filter is the server's default. The elliptic bank is not a
        # legacy option - it is far cheaper per sample, which matters on a Pi
        # Zero 2W, and it is what the server falls back to for an odd-sized
        # block.
        self.fftfilt_var = tk.BooleanVar(value=True)
        frow = ttk.Frame(nf)
        frow.grid(row=1, column=0, sticky="w", padx=(16, 0))
        ttk.Radiobutton(frow, text="FFT (tunable)", variable=self.fftfilt_var, value=True,
                         command=self.on_fftfilt_toggled).pack(side="left")
        ttk.Radiobutton(frow, text="Elliptic bank", variable=self.fftfilt_var, value=False,
                         command=self.on_fftfilt_toggled).pack(side="left", padx=(12, 0))

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
        # Both on one row: the window is already 1174px tall with
        # resizable(False, False), so a stacked row per control would push
        # the Power/ALC section below a 1080p screen's usable height.
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
        sm = ttk.LabelFrame(self, text="Signal Strength (uncalibrated)", padding=8)
        sm.grid(row=7, column=0, sticky="ew", padx=8, pady=(0, 8))
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
        spec = ttk.LabelFrame(self, text="Spectrum (±15kHz around dial)", padding=8)
        spec.grid(row=8, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.spectrum_canvas_w = 560
        self.spectrum_canvas_h = 180
        self.spectrum_canvas = tk.Canvas(spec, width=self.spectrum_canvas_w,
                                          height=self.spectrum_canvas_h,
                                          background="#111", highlightthickness=0)
        self.spectrum_canvas.pack()
        self.spectrum_status_var = tk.StringVar(value="no spectrum data yet")
        # wraplength pinned to the canvas width: this label's text is the only
        # thing in the window whose length varies at runtime, and the window
        # is sized to its contents and then frozen by resizable(False, False).
        # Without the cap, a long status line silently widens the whole panel
        # and there is no way for the operator to drag it back - measured at
        # 891px against the normal 596px before this was added.
        ttk.Label(spec, textvariable=self.spectrum_status_var,
                   wraplength=self.spectrum_canvas_w).pack(anchor="w", pady=(4, 0))

        # --- TX test ---
        # rigctld's u/U TONE (the test-tone generator, tone_gen.h) and t/T
        # (PTT). With a tone selected, Transmit keys the radio and sends it
        # in any mode; the sideband follows the mode. maxibitx turns the
        # tone off and drops PTT after TONE_GEN_TIMEOUT_S (30 s).
        # docs/dsp_design_notes/tx_test_tones_and_alc.md.
        txt = ttk.LabelFrame(self, text="TX Test - dummy load or low power", padding=8)
        txt.grid(row=9, column=0, sticky="ew", padx=8, pady=(0, 8))
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
        pw = ttk.LabelFrame(self, text="TX Power", padding=8)
        pw.grid(row=10, column=0, sticky="ew", padx=8, pady=(0, 8))
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

        self.protocol("WM_DELETE_WINDOW", self.on_close)

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
        if freq_reply is None or rit_reply is None or vol_reply is None \
                or micgain_reply is None or mode_reply is None or narrow_reply is None \
                or fftfilt_reply is None \
                or pitch_reply is None or width_reply is None \
                or strength_reply is None \
                or tone_reply is None or ptt_reply is None \
                or power_reply is None or alc_reply is None:
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
            # bin FFT_SIZE/2 = dial center) - crop to the middle +-15kHz for
            # display (see SPECTRUM_DISPLAY_HALF_SPAN_HZ's comment on why).
            bin_hz = 96000.0 / FFT_SIZE
            center = len(db) // 2
            half_bins = min(int(round(SPECTRUM_DISPLAY_HALF_SPAN_HZ / bin_hz)), center)
            db = db[center - half_bins:center + half_bins]

            n = len(db)
            xs = np.arange(n) * (w / n)
            peak_db, floor_db = float(np.max(db)), float(np.min(db))
            clipped = np.clip(db, SPECTRUM_DB_FLOOR, SPECTRUM_DB_CEILING)
            frac = (clipped - SPECTRUM_DB_FLOOR) / (SPECTRUM_DB_CEILING - SPECTRUM_DB_FLOOR)
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

            # Dial-center line plus frequency ticks across the displayed
            # +-SPECTRUM_DISPLAY_HALF_SPAN_HZ span.
            span = half_bins * bin_hz  # actual displayed half-span, close to
                                        # SPECTRUM_DISPLAY_HALF_SPAN_HZ but
                                        # snapped to a whole number of bins
            canvas.create_line(w / 2, 0, w / 2, h, fill="#555", dash=(2, 2))
            for offset_hz, label in ((-span, f"-{span/1000:.0f}k"), (-span / 2, f"-{span/2000:.0f}k"),
                                      (0, "dial"), (span / 2, f"+{span/2000:.0f}k"),
                                      (span, f"+{span/1000:.0f}k")):
                x = (offset_hz + span) / (2 * span) * w
                x = min(max(x, 4), w - 4)  # keep the end labels from clipping off-canvas
                canvas.create_text(x, h - 8, text=label, fill="#999", font=("monospace", 8))

            freq_note = f" (dial {self.current_freq_hz:,} Hz)".replace(",", ".") \
                if self.current_freq_hz is not None else ""
            # Peak and floor of what is actually on screen (pre-clip), so the
            # dB window above can be chosen from measurements rather than
            # guesses. Kept terse on purpose - see the wraplength comment
            # where this label is built.
            self.spectrum_status_var.set(
                f"live - {FFT_SIZE}-pt, {bin_hz:.1f} Hz/bin, "
                f"peak {peak_db:.1f}, floor {floor_db:.1f} dB{freq_note}")

        self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)


if __name__ == "__main__":
    Panel().mainloop()
