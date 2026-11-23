# 09 — FAQ

Short answers to the questions that come up most, each with a link to
the doc that has the full story. Where a question rests on an
assumption that turned out to be wrong, the answer says so.

## Origins and design

### What is maxibitx's origin story?

Three projects in a line, each asking a narrower question than the one
before it.

**sbitx** is Ashhar Farhan's (VU2ESE) software for his radio, later
forked and extended at [drexjj/sbitx](https://github.com/drexjj/sbitx).
It is full of features and has grown accordingly: `sbitx.c` and
`sbitx_gtk.c` are more than 3,000 and 12,000 lines, with the GTK user
interface, the DSP and the hardware control intertwined in the same
files. That makes it hard for a new developer to find how anything
works, and a change in one area can land somewhere unrelated.

**minibitx** asked how little code it takes to run the sBitx hardware
well. It kept the hardware layer and threw out everything else: wiringPi
gave way to the kernel's GPIO character device, bit-banged I2C to the
kernel's I2C driver, and the UI and most of the DSP were left to mature
external SDR applications. What remained was a headless daemon that
brought up the radio and streamed I/Q.

**maxibitx** starts from minibitx's hardware and control layer and adds
back an all-mode transmitter and an onboard demodulator, without
bringing the coupling back. Every job lives in its own small file, no UI
runs in the process, and anything a display or controller needs goes
through a documented interface ([`06_api.md`](06_api.md)). The daemon
is about 14,000 lines of C in 36 source files, and a large part of that
is the external interfaces.

### Can lessons from maxibitx be used to improve sbitx?

Some can be lifted directly: the kernel GPIO character device and
kernel I2C in place of wiringPi and bit-banging; feeding ALSA playback
continuously rather than only during transmit; `SCHED_FIFO` on the
audio thread; keeping spectrum and meter work off the audio path; and
the `filter_tune()` normalization fix described below. Each is a
contained change.

The structural lesson is harder to transplant, because it *is* the
structure. The most expensive sbitx/zbitx bug category on record is UI
work sharing blocking resources with the real-time audio path:
`zbitx_poll()`'s bit-banged I2C to the display shares a mutex with the
audio thread, costing 52–66 ms per call and up to about 2.3 s when a
mode click dirties two hundred fields
([`ARCHITECTURE.md`](ARCHITECTURE.md) §2). maxibitx doesn't solve that
problem; it declines to have it, by running no UI in-process.
Retrofitting that into a program whose UI and DSP share a file is a
rewrite, not a patch. What sbitx could take without a rewrite is the
discipline: measure before changing, write the measurement down, and
keep the history in documents instead of in comments.

### What are the main lessons learned building maxibitx?

**Bring the radio up one piece at a time.** `maxibitx.c` starts each
part in dependency order, each step printing an `init:` line, and most
steps degrade rather than abort
([`00_intro.md`](00_intro.md#startup-and-shutdown)). That made it
possible to get each section working and keep it working while the next
was built, and when something broke the console said at which stage.

**Bench-test the DSP with no hardware attached.** Every signal-chain
change is proven numerically first (`make test`). The +66 dB filter
normalization error, an LSB sideband that would have transmitted
nothing, and the limiter's scale-don't-clip behavior were all caught or
confirmed on the bench, not on the air.

**Measurements beat intuition.** A reference error moves transmit and
receive in opposite directions, so comparing the two localizes an error
instead of averaging it into a wrong answer. That is how a remote
receiver, not the radio, was found to be off frequency in the early
tone tests (see
[How is the radio's frequency calibrated?](#how-is-the-radios-frequency-calibrated)).

**Write the history down somewhere it will survive.** Comments describe
what the code does now, and history lives in the docs
([`code_comments.md`](code_comments.md)), because comments that narrate
changes accumulate until nobody can tell current behavior from an old
story about it.

## DSP and numerics

### Does maxibitx do its FFTs in 16-bit integer?

No. The whole signal chain is single-precision floating point:
`fft_filter.c` uses single-precision FFTW (`fftwf_*`, linked with
`-lfftw3f`) on `complex float` buffers. Integers appear only at the
edges, where a format demands them: 32-bit samples at the codec, and
16-bit samples in HPSDR and `iq_stream` packets. Float lets the
pipeline be unity-gain by construction, without tracking fixed-point
scaling at every stage.

### Did sbitx use a mix of FFT types?

Not of FFT types: sbitx uses one block size (`filter_new(1024, 1025)`)
almost everywhere, and maxibitx adopted the same. Two other things were
found ([`ARCHITECTURE.md`](ARCHITECTURE.md) §4):

- **An uncompensated normalization.** sbitx's `filter_tune()` leaves
  each filter's passband gain at +20·log10(N), +66 dB at this N,
  presumably absorbed into an empirically tuned gain constant later.
  maxibitx measured +66.23 dB where 0 dB was expected and fixed it, so
  its filter is unity-gain and any gain constant calibrates real analog
  gain.
- **A mix of mechanisms.** sbitx filters narrow CW on receive with an
  IIR and transmits through an FFT chain. maxibitx has an FFT narrow
  filter (the default: continuously tunable, minimum-phase) and kept a
  bank of 24 elliptic designs as the alternative
  ([`00_intro.md`](00_intro.md#stage-3-the-narrow-filter); the
  comparison is in
  [`rx_narrow_filter_fft_vs_elliptic.md`](dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md)).

### Why is the raw I/Q spectrally inverted?

Because the first mixer's local oscillator sits above the signal, a
station *d* Hz above the dial arrives at −*d* in the baseband I/Q. It
is left that way so HPSDR clients get what they expect, and the
consumers that care (`rx_audio.c` and the control panel) compensate at
their own boundary
([`00_intro.md`](00_intro.md#the-baseband-iq-and-its-consumers)).

## Hardware

### Where did the hardware control code come from?

From sbitx, by way of JJ's 64-bit fork and minibitx, rewritten rather
than copied. The knowledge is Farhan's (which si5351 clock does what,
the crystal filter centre, the LPF relay mapping, the band scale table);
the mechanism is new. `data/hw_settings.ini` is deliberately the same
file sbitx uses, with the same key names, so `cal`, `bfo_freq` and the
per-band `scale` table carry straight over from a calibrated board.

One trap in that file: a top-level key placed below a `[section]`
header belongs to that section and is ignored, so `bfo_freq`,
`xtal_filter_center`, `full_scale_power` and the other top-level keys
go above the first `[section]` (`cal` may also be in `[tcxo]`, where
sbitx puts it). The console names any key in the wrong place
([`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md#the-radio-board-at-startup)).

### Does maxibitx use wiringPi for GPIO?

No. `gpio.c` uses the kernel's GPIO character device (uAPI v2,
`/dev/gpiochip0`), the kernel's own supported interface; wiringPi is
unmaintained and has no Pi 5 support. Every pin number is a BCM number,
not a wiringPi one
([`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md#gpio-pin-numbers)
has the conversion, and [`00_intro.md`](00_intro.md) §4 the pin
tables).

### How did we eliminate bit-banging on the I2C bus?

By deleting it. `i2c.c` opens `/dev/i2c-N` and issues standard SMBus
ioctls, with no timing loop and no GPIO toggling. A bit-banged bus is a
busy-wait, and when it holds a lock the audio thread needs, it stalls
the audio: that is the zbitx display bug described above.

### How does maxibitx run on both the sBitx and the zBitx?

Every difference between the boards is in one file, `radio_hw.c`, as a
profile per board: the pins, the LPF plan, the I2C bus, the knobs, the
relay half of the T/R sequence, and whether the board may transmit.
`hw_settings.ini`'s `sbitx_version` line picks the profile
(`SBITX_V3` for an sBitx DE, v2 or v3, `SBITX_V4` for a zBitx), and
there is no default, because driving one board's pins as another's
could key a transmitter. The rest of the code calls operations that
mean the same on every board. What differs from one radio to the next,
even of the same board, is calibration, and stays in `hw_settings.ini`.

On the zBitx, receive works, transmit is calibrated to 2 W on every
band, and the Pi Zero 2W has CPU to spare; there is no microphone SSB
yet, and the RP2040 front panel isn't driven. The pin and LPF tables are
in [`00_intro.md`](00_intro.md) §4, and the port in
[`zbitx_port_study.md`](dsp_design_notes/zbitx_port_study.md). Adding a
board means adding a profile, with pin numbers taken from that board,
and `make test-radio-hw` checks it.

## Audio and real time

### What was the key to getting rid of the ALSA overruns and underruns?

There were three causes with the same symptom, each with its own fix:

- **Feed playback all the time**, writing silence when not
  transmitting. Writing only during transmit let the device drain and
  underrun between every CW burst.
- **Run the audio thread at `SCHED_FIFO`**, which `make` grants with
  `cap_sys_nice`. As an ordinary thread it is preempted past period
  boundaries.
- **Recover properly**, checking the result of `snd_pcm_prepare()`
  (with `snd_pcm_drop()` first where a driver needs it), and stop
  playback cleanly if recovery fails.

[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md#audio-thread-xruns-hw00-captureplayback)
has the detail.

### Why does nothing block the audio thread?

Because the bug history says that is where the expensive mistakes live.
`sound.c`'s `audio_loop()` takes no lock shared with an ordinary thread,
sleeps nowhere and does no I/O; work for other threads goes through
lock-free rings, and slow-moving controls are plain variables read once
per block. The rule is in `CLAUDE.md`, and
[`interfaces_and_codec_bringup_notes.md`](dsp_design_notes/interfaces_and_codec_bringup_notes.md)
§2 and §5 have the attempts that taught it.

## Power and ALC

### How does ALC work in maxibitx?

A peak limiter holds the transmitted envelope at or below a ceiling, as
a gain rather than a clip, so a signal held at the ceiling comes out
clean, just smaller. POWER moves the ceiling; mic gain (or, in DIGITAL,
the host's level) is the drive pushed into it; ALC reads how far the
limiter is pulling back. `full_scale_power` and `max_power` in
`hw_settings.ini` set the ceiling in watts, so no client can exceed
`max_power`. The limiter never raises quiet audio, and CW never moves
it, so `cw.c`'s 9 ms rise and fall come through untouched.
[`00_intro.md`](00_intro.md#setting-power) is the full account.

### Does the limiter need headroom between `full_scale_power` and `max_power`?

No. Equal values are the normal setting, as a 100 W rig makes 100 W on
CW and 100 W PEP on SSB. The working range is on the drive side, where
mic gain reaches 64× and a host's audio can arrive 20 dB or more above
full scale. A `max_power` below `full_scale_power` simply means running
below rated power.

### How does maxibitx's ALC differ from sbitx's?

sbitx's transmit gain control hasn't been read line by line in this
work, so a point-by-point comparison isn't offered. What maxibitx chose:
output power is set only by the DAC amplitude, with the analog chain at
fixed, calibrated gain per band, which is what makes a feed-forward
limiter sufficient; the ceiling is declared in watts in a settings file;
and the limiter is verified numerically on the bench
([`tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md)).

### How is the radio's frequency calibrated?

With two settings in `hw_settings.ini`, checked against WWV on several
frequencies:

- `cal`, the si5351's reference frequency. Its error grows with
  frequency (1 ppm is about 10 Hz at 10 MHz) and moves transmit and
  receive in *opposite* directions.
- `codec_rate`, the codec's actual sample rate. Its error is the same
  few hertz on every band.

The part of the receive error that grows with frequency is `cal`'s, the
steady part `codec_rate`'s. The procedure is in
[`tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md),
"Frequency calibration".

## Interfaces and applications

### Why are there so many ways to talk to maxibitx?

Because each answers a different need: HPSDR Protocol 1 for existing
SDR applications, rigctld for anything that speaks Hamlib, the USB
gadget for programs that want a sound card and a serial port,
`iq_stream` for a spectrum without HPSDR's session handshake, and TCI
for programs that want control, audio and I/Q over one network
connection ([`00_intro.md`](00_intro.md#what-it-serves),
[`06_api.md`](06_api.md#the-interfaces-at-a-glance)). Several small
interfaces are the cost of keeping UI out of the process, and a lower
cost than one interface that has to be all things.

### Which external applications work well with maxibitx?

SparkSDR, SDR Console and sdrOxide for I/Q; WSJT-X for FT8 over the USB
gadget, decoding on par with SparkSDR, with a two-way contact made;
FLDigi over the gadget's audio; and FLRig for control, set to the
**QMX** rig type. WSJT-X and FLRig can't both own the gadget's COM port
at once. [`06_api.md`](06_api.md#using-it-with-wsjt-x) has the setup,
and its TCI section the setup for JTDX and WSJT-X Improved.

### How does maxibitx work with a key and a microphone?

Through the sBitx's own connections. The key jack's contacts are read as
kernel-timestamped edges and placed at the sample where they happened,
and `keyer.c` is a straight, bug, ultimatic or iambic A/B keyer at
1–60 WPM. A mono plug is detected at start-up, and the ring is the mic
PTT in USB and LSB. The microphone comes in through the codec
([`00_intro.md`](00_intro.md#the-key-jack)). From a computer, CW goes
as text (rigctld `b`, Kenwood `KY`, TCI or the control panel), sent by
the keyer; individual elements can't be keyed remotely
([`06_api.md`](06_api.md#not-available-yet)).

### What is `rigctl_panel.py`?

A Tk control panel laid out for the sBitx's 7-inch touchscreen and
usable in a window on any machine on the network: frequency, mode, RIT,
the CW filter, the keyer and CW text, power and mic gain, an S-meter,
and a live spectrum and waterfall
([`../tools/README.md`](../tools/README.md#rigctl_panelpy)). It is a
client, not part of the daemon: it uses only rigctld and `iq_stream`,
so it is the working proof that [`06_api.md`](06_api.md) is enough to
build a radio front end on.

### Could `rigctl_panel.py` be replaced by a full-featured SDR application?

Yes, and that is the intended path. Everything one needs is already
exposed: I/Q over HPSDR, `iq_stream` or TCI, full control over rigctld
or TCI, and audio over the USB gadget or TCI. It could be a native app
on the Pi's touchscreen (as a separate process, never inside the
daemon), a desktop network application (which SparkSDR and SDR Console
already are), or a browser front end behind a small bridge. The
radio-specific parts are done; what's left is UI, and UI is most of the
work.

## Modes and scope

### Why doesn't maxibitx support NBFM, AM or SAM?

They were out of scope, not impossible
([`ARCHITECTURE.md`](ARCHITECTURE.md) §8). For transmit, and for NBFM
in general, that is sound: this is an HF SSB and CW radio. Receive-side
AM and SAM for shortwave broadcast listening would be fairly cheap:
`rx_audio.c` has the complex signal at stage 2, where an envelope
detector or a carrier-tracking synchronous detector would sit beside the
existing ones. Stage 1 passes one side of the carrier, 0 to 3 kHz, so
AM would need a double-sideband variant of it; SAM can work from one
sideband as it is.

## Working on the code

### How do I know a change hasn't broken the DSP?

Run `make test`. It runs every bench test without hardware, one line
each, and exits non-zero if any fails
([`00_intro.md`](00_intro.md#8-testing) lists them). `make
check-comments` checks the comment policy. A comments-only change must
also leave the machine code identical
([`code_comments.md`](code_comments.md) has the procedure).

### Why is there a comment policy?

Because a stale comment is worse than none. A comment says what the
code does now: units, ranges, invariants, where a constant came from,
coupling to other code and known caveats. History, bugs and how they
were found, and measurements go in `docs/`. A fix that teaches
something gets both ([`code_comments.md`](code_comments.md)).

### What happens if `data/hw_settings.ini` is missing?

maxibitx won't start: the file names the radio board, and without that
line maxibitx prints the valid lines and exits before touching any GPIO
line or clock. Every other key is optional and takes its compiled-in
default, giving a working radio with uncalibrated numbers
([`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md#the-radio-board-at-startup)).

### What's not done?

The radio's open items are in
[`00_intro.md`](00_intro.md#known-limitations), and what an application
can't do yet is in [`06_api.md`](06_api.md#not-available-yet).
