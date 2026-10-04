# tools/

Client-side utilities that talk to a running maxibitx over the network -
none of these run on the Pi as part of maxibitx itself, and none require
changes to build/run maxibitx (beyond whatever server-side support they
depend on, noted per tool below) - plus one developer check.

## `check_comments.py`

Checks source comments against the comment policy
([`../docs/code_comments.md`](../docs/code_comments.md)) and checks that
each source file's header names the file itself. Run it with
`make check-comments` (files changed vs `origin/main`) or
`make check-comments-all` (all of `src/`). Python 3, no extra modules.

## `rigctl_panel.py`

A small desktop GUI: a frequency readout/entry (with quick +/- step
buttons), a volume slider, and a live spectrum display with a waterfall
(about 8 s of history) under it. Talks the same
plain-text rigctld protocol WSJT-X/Thetis/etc. already use against
minibitx's built-in server (`hamlib.c`, default TCP 4532) - see
[`../docs/04_remote_control_and_iq_output.md`](../docs/04_remote_control_and_iq_output.md)
for the full command set. Volume relies on the `l`/`L AF` level commands
added alongside this tool - a minibitx build from before that change
will still connect and show frequency, but volume will read/set nothing.

The spectrum is fed by a second, independent connection - UDP to
`src/interfaces/iq_stream.c`'s lightweight I/Q telemetry stream (port 4536) - kept
deliberately separate from both the HPSDR Protocol 1 link WSJT-X/Thetis
use for their own I/Q (`hpsdr_p1.c`, single-client - a second client
there would silently steal the stream from whichever SDR app connected
first) and the CAT/audio USB gadget (`usb_gadget.c`). See
[`../docs/dsp_design_notes/iq_stream_design.md`](../docs/dsp_design_notes/iq_stream_design.md)
for the full design and bench verification. A minibitx build from before
`iq_stream.c` existed will still connect for frequency/volume; the
spectrum panel will just say it's waiting for data that never arrives.

Run it on a laptop, or on the Pi's own desktop if it has one - it's a
separate process from `minibitx`, connecting over TCP (rigctld) and UDP
(spectrum) like any other client, so it works either locally
(`127.0.0.1`) or from anywhere on the network that can reach the Pi.

**Requirements:** Python 3's standard library, `tkinter`, and `numpy`
(for the spectrum's FFT - the plot itself is drawn on a plain Tkinter
Canvas, no plotting library needed). `tkinter` usually ships with Python
on Windows/macOS, but is a separate package on Debian/Raspberry Pi OS:

```
sudo apt install python3-tk
pip install numpy   # or: sudo apt install python3-numpy
```

**Run:**

```
python3 tools/rigctl_panel.py
```

Enter the Pi's hostname or IP and the rigctld port (4532 by default),
click Connect. The panel remembers the last host/port used
(`~/.maxibitx_panel.json`; if that doesn't exist yet it reads the older
`~/.minibitx_panel.json`) so subsequent launches don't need retyping
them. Frequency and volume both poll once a second while connected, so
the panel stays current even if something else (another rigctld client,
FLRig's CAT, an HPSDR app's MOX) changes state in the meantime - except
the frequency entry box itself, which is left alone while it has focus
so a poll tick can't overwrite what you're mid-way through typing. The
spectrum starts/stops with the same Connect/Disconnect button, spans the
full ±48kHz native-96kHz baseband range around dial center (see
`iq_stream_design.md` for what actually limits how much of that is real,
undistorted signal vs. crystal-filter skirt), and updates at roughly
15fps regardless of how much faster the underlying FFT itself runs.

## `tci_client.py`

A small command-line TCI client, standard library only, for checking
maxibitx's TCI server (`src/interfaces/tci.c`, TCP 50001) from a laptop.
With no options it connects, prints the initialization burst, and checks
it against what JTDX and Hamlib depend on: lowercase keywords, one
command per message, `start;` before `ready;`, within 1.5 s. Options send
commands and print the replies (`-c "vfo:0,0,7074000;"`), record receive
audio and report its format and level (`--audio 5 --wav rx.wav`), report
the I/Q's rate and strongest frequency (`--iq 3 --iq-rate 48000`), or
transmit a test tone through the TCI audio path (`--tx-tone 1000`; this
keys the transmitter - use a dummy load). CW text goes as commands, in CW
mode: `-c "cw_macros:0,cq test |ar|;"` or `-c "cw_msg:0,tu,kb2ml,5nn;"`
(these key the transmitter too). `./test-tci --serve` on any
computer runs a stubbed server to try it against without a radio. See
[`../docs/06_api.md`](../docs/06_api.md), "TCI".

## `xtal_sweep.py`

Measures a radio's crystal filter, to find the right
`xtal_filter_center` for `data/hw_settings.ini`. It holds the dial still,
steps `xtal_filter_center` over rigctld (`L XTALCENTER`, TCP 4532), and
records the whole I/Q spectrum (UDP 4536) at each step.

At each step, the offset *d* from the dial sees the crystal filter at
*setting − d*, multiplied by a response that depends on *d* alone (the
codec, its digital filter and the anti-alias filter, which see the same
IF at every step). The tool separates the two with a robust fit, so each
crystal frequency is measured at up to 45 offsets rather than one. The
result is smoother, signals that come and go are ignored, and it covers
the swept range plus about 11 kHz on each side, so the filter's edges are
measured even when they are outside the sweep.
[`../docs/dsp_design_notes/zbitx_port_study.md`](../docs/dsp_design_notes/zbitx_port_study.md)
§11 explains the method.

```
python3 tools/xtal_sweep.py --host zbitx.local
python3 tools/xtal_sweep.py --host sbitx.local --out sbitx_filter
python3 tools/xtal_sweep.py --refit xtal_sweep_20261004_150000_raw.csv
```

By default it sweeps 39,975,000 to 40,050,000 Hz in 1,000 Hz steps
(about two minutes); the step is also the result's resolution. It prints
the filter against crystal frequency with bars, the peak, the depth of
the measurement, the edges and centres at −3, −6 and −20 dB, and the
−6 dB centre (or the −3 dB one, if the measurement isn't deep enough)
rounded to 100 Hz as the value to put in the ini. It also prints the
response after the filter. It restores the original setting at the end,
also on Ctrl-C, and never writes the ini.

Files, named from `--out` (default `xtal_sweep_<time>`): `.csv` (the
filter, with a 1 kHz window at the dial measured the simple way beside
it), `_if.csv` (the response after the filter), `_raw.csv` (every step's
spectrum; `--refit` reads it again without the radio), and `.png` if
matplotlib is installed.

How deep the filter can be drawn depends on how far the noise in front
of it is above the noise added after it. Band noise on an antenna gives
several dB to a few tens of dB, depending on the band and the time of
day; a noise source at the antenna input gives the most; a dummy load
shows the filter barely at all. The report gives the depth, and doesn't
report an edge below it. In a shallow measurement the edges come out a
little wide, but the centre holds. `--help` lists the other options.

**Requirements:** Python 3 and `numpy`; `matplotlib` for the plot. Needs
a maxibitx with rigctld's `XTALCENTER` level
([`../docs/06_api.md`](../docs/06_api.md)); with an older build it says
so and stops. While a setting leaves no usable transmit IF, maxibitx
refuses to transmit; the sweep won't start while the radio is
transmitting.
