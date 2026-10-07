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

A touch-friendly control panel, laid out for the sBitx's 7-inch 800×480
screen and just as usable in a window on a laptop. It is a client like
any other: control over rigctld (`hamlib.c`, TCP 4532) and the spectrum
over the I/Q stream (`iq_stream.c`, UDP 4536), so it runs on the Pi
itself or anywhere on the network
([`../docs/06_api.md`](../docs/06_api.md)).

The screen, top to bottom:

- **Top bar:** band menu, the frequency, mode (CW, CWR, USB, LSB, DIGI),
  the RX/TX light, volume, and the settings gear. The band menu remembers the last
  frequency used on each band.
- **RIT:** −100, −10, the offset (tap it to clear), +10, +100. While
  it's set, a green marker on the spectrum, scale and waterfall shows
  where the receiver is listening; the display stays centred on the dial,
  so a signal doesn't move when RIT does. An offset past the span shows
  as an arrow at that edge.
  **FILTER:** the CW filter on or off, its width and its centre (the CW
  pitch). In DIGI the radio holds the filter out of circuit and the
  group says so.
- **Spectrum:** span (5, 10 or 30 kHz), a status line, the S-meter, then
  the trace, a frequency scale and the waterfall under it. Each frame
  averages the 6 or so FFTs since the last one, over time and never
  across frequencies, so the noise floor holds steady and signals stay
  narrow. A dimmer peak line behind the trace holds each frequency's
  highest level and falls back 15 dB a second, which leaves a CW
  station's outline standing between its dits; it can be switched off
  in Settings.
- **By mode:** in CW, ten macro buttons; in USB/LSB, mic gain and the ALC
  meter; in DIGI, a note that the digital-mode program has the audio.
- **Bottom row:** keyer type, WPM, the CW text field, Send and Stop,
  power, and Tune.

**Tuning digit by digit.** Tap a digit of the frequency to make it the
tuning step (it turns amber and underlined). Then the mouse wheel over a
digit, a drag up or down on it, or the Up and Down keys change it; Left
and Right move the step. Double-click the frequency to type one in:
`14058.2` (kHz), `14.0582` (MHz) or `14058200` (Hz).

**Settings** open from the gear at the top right (or the RX/TX button):
the connection, the peak line, paddle reversal, the CW filter type (FFT
or elliptic), the TX test tones with their Transmit switch, the ten
macros (a button label and the text sent), and the macro fields. A field
is a name and a value: MYCALL and PARK to start with, and up to eight of
your own (NAME, QTH, RIG...). A macro's `{NAME}` is replaced by that
field's value, and a macro naming a field with no value isn't sent, so a
CQ never goes out without your call. Settings, the last host and the
band memory are kept in `~/.maxibitx_panel.json`.

**Tune** keys the transmitter with the 1 kHz test tone, so the carrier is
1 kHz from the dial, at the power set beside it; maxibitx drops it after
30 s. **APF, NR and XIT** are on the screen but disabled: maxibitx doesn't
have them yet.

The panel connects at start-up to the last host used (127.0.0.1 the first
time, which is right on the Pi) and keeps trying every 5 s while it
isn't connected, so it comes back on its own when maxibitx restarts.

```
python3 tools/rigctl_panel.py                   # a window, last host
python3 tools/rigctl_panel.py --fullscreen      # on the sBitx's own screen
python3 tools/rigctl_panel.py --host sbitx.local
```

**Requirements:** Python 3 with `tkinter`, and `numpy`. The IBM Plex
fonts are used when installed, DejaVu otherwise. On Raspberry Pi OS:

```
sudo apt install python3-tk python3-numpy fonts-ibm-plex
```

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
fit also takes out the band's level rising and falling during the sweep
(fading), which moves every offset of a step together and would
otherwise be drawn into the filter as ripple. The result is smoother,
signals that come and go are ignored, and it covers
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
it), `_if.csv` (the response after the filter), `_steps.csv` (the
band's level changes it took out, step by step), `_raw.csv` (every step's
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
