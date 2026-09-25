# 09 — FAQ

Short answers to the questions that come up most, with pointers to the
doc that carries the full story. Where a question rests on an
assumption that turned out to be wrong, the answer says so rather than
answering the question as asked.

## Origins and design

### What is maxibitx's origin story?

Three projects in a line, each asking a narrower question than the one
before it.

**sbitx** is Ashhar Farhan's (VU2ESE) software for his radio, later
forked and extended at [drexjj/sbitx](https://github.com/drexjj/sbitx).
It is full of features and has grown accordingly: `sbitx.c` and
`sbitx_gtk.c` are more than 3,000 and 12,000 lines, with the GTK user
interface, the DSP and the hardware control intertwined in the same
files. That coupling has real costs — it is hard for a new developer to
find how anything works, and a change in one area can land somewhere
unrelated.

**minibitx** asked how little code it takes to run the sBitx hardware
well. It kept the hardware layer and threw out everything else: wiringPi
gave way to the kernel's GPIO character device, bit-banged I2C to the
kernel's I2C driver, and all of the UI and most of the DSP were left to
mature external SDR applications. What remained was a headless daemon
that brought up the radio and streamed I/Q.

**maxibitx** starts from minibitx's hardware and control layer and adds
back an all-mode transmitter and an onboard demodulator — without
bringing the coupling back. Every job lives in its own small file, no UI
runs in the process, and anything a display or controller needs goes
through a narrow documented interface
([`06_api.md`](06_api.md)). The whole daemon is about 6,300 lines of C
across 21 files, and a large part of that is the external interfaces.

### Can lessons from maxibitx be used to improve sbitx?

Some can be lifted directly; the most valuable one probably cannot.

Portable as-is: the kernel GPIO character device and kernel I2C in place
of wiringPi and bit-banging; feeding ALSA playback continuously rather
than only during transmit; `SCHED_FIFO` on the audio thread; keeping
spectrum and meter computation off the DSP functions the audio path
depends on; and the `filter_tune()` normalization fix described below.
Each is a contained change.

The structural lesson is harder to transplant, because it *is* the
structure. [`ARCHITECTURE.md`](ARCHITECTURE.md) §2 catalogues five
categories of already-diagnosed sbitx/zbitx bugs, and the largest by
measured cost is UI work sharing blocking resources with the real-time
audio path — `zbitx_poll()`'s blocking bit-banged I2C to the display
shares a mutex with the audio thread, costing 52–66 ms per call with two
fields dirty and up to ~2,256 ms when a mode click dirties two hundred.
Several point fixes were tried and one was rejected in review for
regressing display responsiveness elsewhere, which is the signature of a
problem that isn't at any one call site. maxibitx doesn't solve that
problem; it declines to have it, by running no UI in-process at all.
Retrofitting that into a program whose UI and DSP share a file is a
rewrite, not a patch.

What sbitx could take from it without a rewrite is the discipline rather
than the architecture: measure before changing, write the measurement
down, and keep the history in documents instead of in comments.

### What are the main lessons learned building maxibitx?

The one you'd name first is that `maxibitx.c` brings up each piece of
the radio in turn, in dependency order, and each step reports its own
result on the console. GPIO first (relays held in an RX-safe state),
then the si5351 over I2C, then board revision detection which needs that
bus, then the VFO phase table, CW, the demodulator, each external
interface, the codec, and finally the audio thread. Every step prints an
`init:` line, and most degrade rather than abort — if rigctld can't bind
its port the daemon says so and carries on. That ordering made it
possible to get each section working and *keep* it working while the
next one was built, and when something broke the console said which
stage it broke at.

Three more are worth naming alongside it.

**Bench-test the DSP with no hardware attached.** Every signal-chain
change is proven numerically first (`make test-fft-filter`,
`test-tx-pipeline`, `test-rx-filter`, `test-rx-audio`,
`test-upsample48k`). The +66 dB filter normalization error, the LSB
sideband that would have transmitted nothing, the sideband-zero
rejection figure and the limiter's scale-don't-clip behavior were all
caught or confirmed on the bench, not on the air.

**Measurements beat intuition, including mine.** The transmit tone tests
appeared to show the si5351 reference about 1.5 ppm low, and the fix
looked obvious. Receiving WWV showed the reference was at nominal and
the *remote receiver* used for the tests was the thing in error. What
made that provable was noticing that a reference error moves transmit
and receive in opposite directions — so a disagreement between the two
localizes the error instead of averaging into a wrong answer.

**Write the history down somewhere it will survive.**
[`code_comments.md`](code_comments.md) sets the rule that code comments
describe what the code does now and history lives in the docs, and
`tools/check_comments.py` flags drift from it. This exists because the
alternative was observed: comments that narrate a change accumulate
until nobody can tell current behavior from an old story about it.

## DSP and numerics

### Does maxibitx do its FFTs in 16-bit integer? Does that hurt performance?

No — the premise doesn't hold. Everything in the signal chain is
single-precision floating point. `fft_filter.c` allocates with
`fftwf_alloc_complex()` and plans with `fftwf_plan_dft_1d()`, and the
buffers are `complex float` throughout; the binary links `-lfftw3f`, the
single-precision FFTW.

Integers appear only at the edges, where the outside world demands them:
32-bit samples at the ALSA/WM8731 boundary, and 16-bit samples on the
wire in the HPSDR Protocol 1 payload and `iq_stream`'s packets, because
those formats specify int16. Nothing is *computed* in integer.

Had it been integer, it would have hurt — not in raw speed on a Pi 4,
which has hardware floating point, but in the engineering. Fixed-point
FFT work means tracking scaling at every stage and choosing between
overflow and lost resolution at each one. Float lets the pipeline be
unity-gain by construction, which is what makes a claim like "the
limiter deviates from a pure gain by 78 dB" measurable rather than
buried under scaling artifacts.

### Did we find that sbitx used a mix of FFT types?

Not a mix of *types* — sbitx uses one FFT block size essentially
everywhere (`filter_new(1024, 1025)`, Kaiser beta 5), and maxibitx
deliberately adopted the same size. Two related things were found
instead, both recorded in [`ARCHITECTURE.md`](ARCHITECTURE.md) §4 and
the step-2 bench results.

**An uncompensated normalization.** sbitx's `filter_tune()` leaves every
filter's passband gain at an uncorrected +20·log10(N) — +66 dB at this
N — which its `rx_linear()` and `tx_process()` never visibly correct.
Presumably it is absorbed, unremarked, into whatever empirically tuned
gain constant follows. It was caught here by measuring +66.23 dB where
0 dB was expected, and fixed by using 1/N² instead of sbitx's 1/N in the
initial gain: one of the two FFT round-trips inside `window_filter()`
was going uncompensated. maxibitx's filter is unity-gain by
construction, so any gain constant added later calibrates real analog
gain rather than partly undoing an unlabeled FFT artifact.

**A mix of *mechanisms*, which is probably what the question is after.**
sbitx filters narrow CW on receive with a bespoke IIR while transmitting
through an FFT chain — two separate pieces of machinery doing closely
related work. maxibitx's design decision was to run one shared FFT
filter for both directions, which is what makes pitch and width live
operator controls instead of recompiled coefficients. The cost was
quantified before it was accepted: roughly 300 Hz of transition per side
against the elliptic design's measured 135 Hz, about a 2× softer skirt.

That prediction turned out to be wrong in an interesting direction. The
filter that got built is three times longer than the one those numbers
describe, and it measures *sharper* than the elliptic, with a stopband
30–55 dB deeper. The real cost was in the time domain, where nobody had
looked: a linear-phase FIR of that length delays everything by 16 ms, and
a keyed CW element took 20 ms to settle against the elliptic's 7 ms. A
minimum-phase realization of the same response (`u MINPHASE`, on by
default) brings those to 4.5 ms and 8.7 ms with the magnitude response
intact. The unification still isn't complete — the elliptic IIR remains
the *default* receive narrow filter, with the FFT implementation
selectable at runtime (`u FFTFILT`), because it sounded worse on real
signals than the bench predicted, and whether minimum phase closes that
gap is an on-air question nobody has answered yet.

There's a second twist. The FFT filter existed on the receive side to make
pitch and width adjustable, and elliptic coefficients merely can't be
designed *at runtime* — nothing stopped stage 3 from carrying more than one
of them. It now carries twelve, three pitches by four widths, designed
offline and switched by loading a coefficient set. That gives the elliptic
the adjustability the migration was for, at quantized values instead of
continuous ones, and keeps its faster attack. So the receive side has two
working answers now, and the shared-pipeline argument is weaker on receive
than it looked.

### Why is the raw I/Q spectrally inverted?

A station *d* Hz above the dial arrives at −*d* in the baseband I/Q,
because the first mixer's local oscillator sits above the signal. The
inversion is left in deliberately: `sound.c` doesn't correct it, so
HPSDR consumers get what they expect, and the consumers that do care
(`rx_audio.c` via `RX_IQ_SPECTRUM_INVERTED`, and the control panel)
compensate explicitly at their own boundary. Correcting it centrally
would have meant un-correcting it for everything downstream.
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) has the
chain.

## Hardware

### Where did the code that initializes and controls the radio hardware come from?

From sbitx, by way of JJ's 64-bit fork and minibitx, and then rewritten
rather than copied. The *knowledge* is Farhan's — which si5351 clock
does what, the crystal filter centre, the LPF relay mapping, the band
scale table — and that carries over. The mechanism does not: wiringPi
and bit-banged I2C were both replaced by kernel interfaces, and the pin
numbers had to be re-derived because wiringPi's numbering isn't the
kernel's.

`data/hw_settings.ini` deserves a mention here: it is deliberately the
*same file* sbitx uses, with the same key names and section layout, so
`cal`, `bfo_freq` and the per-band `scale` table carry straight over
from an existing calibrated board. Those values are physical
measurements of one specific radio, not source constants, and they
belong in a file for exactly that reason.

One trap in that file is worth knowing, because it has cost time once
already: the parser tracks `[sections]`, so a *top-level* key placed
below a section header belongs to that section and is silently ignored.
`bfo_freq`, `xtal_filter_center`, `full_scale_power` and `max_power`
must all appear before the `[tcxo]` line. (`cal` is the exception — it
is accepted both inside `[tcxo]`, where sbitx puts it, and at the top
level.) A key that goes missing without complaint is the symptom.

### How do we control GPIO for the LPF relays and PTT — with wiringPi?

Not with wiringPi. It was removed, and that was one of minibitx's
founding changes. `gpio.c` talks straight to the kernel through the
Linux GPIO character-device ioctl API (uAPI v2, `<linux/gpio.h>`),
issuing a `GPIO_V2_GET_LINE_IOCTL` per line against `/dev/gpiochip0` and
holding the returned line-request file descriptor for the life of the
process. One request and one descriptor per line, which matches how the
hardware layer already treats each pin as an independent handle.

Two reasons. wiringPi is unmaintained upstream with no Pi 5 support —
not an immediate problem on a Pi 4, but not a foundation either — and
the character device is the kernel's own current supported interface, so
this isn't a workaround chosen to shed a dependency.

The catch worth knowing about: every offset in `gpio.h` is a **BCM GPIO
number**, the kernel's numbering, not the wiringPi pin numbers the old
code used. The BCM numbers in `radio_hw.h` were derived from a
`gpio readall` capture on real sBitx v2 hardware. Porting to a different
board means re-deriving that mapping the same way, not assuming it.

### How did we eliminate bit-banging on the I2C bus?

By deleting it and letting the kernel's driver do the job. `i2c.c` opens
`/dev/i2c-N` and issues standard SMBus ioctls —
`i2c_write_byte_data()`, `i2c_read_byte_data()` and the block variants —
returning negative errno on failure. That is the whole of it; there is
no timing loop and no GPIO toggling anywhere in the I2C path.

This is more than tidiness. The most expensive bug category on record in
[`ARCHITECTURE.md`](ARCHITECTURE.md) §2 is a blocking bit-banged I2C
exchange to the zbitx display that shares a mutex with the audio thread,
so a slow display update stalls audio through lock contention — 52–66 ms
in the ordinary case and over two seconds in the worst one. A bit-banged
bus is a busy-wait holding a lock, which is precisely what a real-time
audio thread cannot afford to be behind.

### Could maxibitx be adapted to work on zbitx hardware?

Probably, and the structure is on your side, but it is a real port
rather than a recompile. The honest answer has three parts.

What helps: the hardware-specific code is confined to `radio_hw.c`,
`gpio.c`, `i2c.c`, `si5351v2.c` and `hw_settings.c`, and everything
above those files works in samples rather than in hardware. The
si5351 and the codec bring-up would likely carry over with changed
constants.

What would need real work: the pin map, which was derived from a
`gpio readall` on sBitx v2 hardware and must be re-derived the same way
rather than assumed; any difference in the IF chain, since the crystal
filter centre and BFO frequency are baked into both the tuning
arithmetic and — at present — into `tx_pipeline.h` as compiled-in
constants (a known limitation); and the per-band power calibration,
which is per-board data.

What is genuinely unknown: CPU headroom. maxibitx runs FFTW at 96 kHz
on a Pi 4. Nobody has measured it on a Pi Zero 2W, and that measurement
should come before any porting work, not after.

## Audio and real time

### What was the key to getting rid of the ALSA overruns and underruns?

There wasn't one key — there were three distinct causes wearing the same
symptom, and each needed its own fix.
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
has the detail.

**Feed playback continuously, not only during transmit.** This is the
one that surprises people. ALSA's underrun detection is tied to the
hardware clock draining the ring buffer against the software pointer,
not to whether you are calling `writei()`. Writing only during a CW
burst left the device with nothing arriving while the key was up, so it
drained its ~43 ms buffer and underran between every burst; the first
write of the next burst then hit that stale underrun and needed
recovery. Hence the "xrun, recovering" storm on every key-down. Writing
silence the rest of the time keeps the device running — which is what
sbitx's own full-duplex path does.

**Give the audio thread real-time priority.** `sound_thread_start()`
requests `SCHED_FIFO` at maximum priority through `pthread_attr_t`, so
`pthread_create()` fails fast if the privilege isn't there rather than
silently falling back long after startup claims success. As an ordinary
`SCHED_OTHER` thread it competes with everything else and gets preempted
past a period boundary. The reported symptom was an immediate xrun flood
at startup with nothing even connected, right after the
`failed to set audio thread to SCHED_FIFO` warning. `make` grants
`cap_sys_nice` for this reason.

**Recover properly, and stop if recovery fails.** A bare
`snd_pcm_prepare()` clears a fresh `-EPIPE`, but on a device stuck for a
more persistent reason `prepare()` itself fails, and a `snd_pcm_drop()`
followed by `prepare()` is what some drivers actually need. Both call
sites must check the return value — the playback path once discarded it
and spun forever printing recovery messages at the full loop rate, which
was a real bug. It now disables playback gracefully with one clear
message instead.

### Why does nothing block the audio thread?

Because the measured bug history says that is where the expensive
mistakes live. `sound.c`'s `audio_loop()` takes no lock shared with an
ordinary thread, sleeps nowhere, and does no I/O; work for other threads
goes through lock-free rings. Control surfaces such as `hamlib.c` and
`usb_gadget.c` only ever set or read plain in-process state, and human-
speed controls like mic gain and TX power are plain variables written by
one thread and read once per block by the other. The rule is written
down in `CLAUDE.md` so it survives contributors.

## Power and ALC

### How does ALC work in maxibitx?

Four things with distinct jobs, and most confusion about ALC comes from
collapsing them. [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md),
"Setting power", is the full account.

Two values in `hw_settings.ini` describe the radio.
`full_scale_power` is what a full-scale signal produces once the
`[tx_band]` scales are calibrated — the board's rated output, a
measurement. `max_power` is the ceiling the limiter holds — a choice.

Two runtime controls sit either side of the limiter. **POWER**
(`l/L RFPOWER`, 0–1 of `max_power`) *moves the limiter's threshold*; it
is emphatically not a gain applied after the limiter, because a gain
there could walk straight past `max_power`, which is the one thing that
value exists to prevent. **MICGAIN** is the drive: how hard the signal
is pushed into whatever ceiling is in force. Turning drive up does not
raise peak power — it raises the average relative to the peak, and it
moves the ALC meter rather than the wattmeter.

The limiter itself applies `min(1, ceiling/envelope)` to
`2*|out_c[i]|` in `tx_pipeline.c` — the magnitude of the still-complex
signal just before the real part is taken. That is the envelope the
exciter will actually radiate; the input audio's peak is the wrong
quantity, because the 300–3000 Hz bandpass and the sideband zero both
change it and an SSB envelope peak can exceed the audio peak that
produced it.

**It is a gain, never a clip.** A sine times a constant is still a sine,
so a signal held at the ceiling comes out clean, just smaller. Case F in
`tx_pipeline_test.c` measures this: a tone driven 6 dB too hot comes out
at exactly rated power, reads 6.02 dB on the ALC meter, and differs from
the correctly-driven waveform by 7.70e−05 — 82 dB down — where
truncating at the ceiling would differ by 4.99e−01, 6 dB down.

Output is delayed 2 ms while the gain is computed from the undelayed
signal, with attack rate-limited to cross the whole gain range in
exactly that window, so the gain arrives before the peak that needs it.
Release is 250 ms. And the gain only ever reduces: quiet audio transmits
quietly, because raising the average to meet the ceiling is a speech
compressor, a different thing that belongs upstream and isn't built.

CW never moves the gain at all — its envelope is constant at exactly
full scale — so `cw.c`'s 5 ms Blackman-Harris rise and fall come through
untouched.

### Does the limiter need headroom between full_scale_power and max_power?

No, and an early draft of the design note wrongly said it did. A 100 W
rig makes 100 W on CW and 100 W PEP on SSB; its ALC stops the drive from
exceeding rated output rather than reserving part of the PA. Equal
values are the normal setting.

The working range comes from the drive side, where there is plenty:
`mic_tx_gain` reaches 64×, and in DIGITAL the level is the host's
entirely, so the signal arriving at the limiter can exceed full scale by
20 dB or more. Before the limiter existed that overshoot reached
`TX_SAMPLE_CLAMP` and was hard-clipped. Setting `max_power` below
`full_scale_power` means one thing only: running below rated power.

### How does maxibitx's ALC differ from sbitx's?

This one deserves an honest boundary. What maxibitx does is described
above and is measured. What sbitx's current TX gain control does in
detail has *not* been read line by line in the course of this work, so a
point-by-point comparison would be guesswork and isn't offered here.

What can be said is what maxibitx deliberately chose, and why each
choice was available. Output power here is set solely by the DAC
amplitude, with the analog chain at fixed gain, which is what makes a
feed-forward limiter sufficient — there is no loop to close because the
amplitude-to-watts mapping is already calibrated per band. The ceiling
is declared in watts in a settings file rather than living as an
empirical constant in source. And the limiter's behavior is verified
numerically on the bench rather than by ear. Whether and where sbitx
differs is a reading exercise someone should do before claiming an
answer.

### How is the radio's frequency calibrated?

Through `cal` in `hw_settings.ini` — sbitx's own key, in sbitx's own
`[tcxo]` section — which sets the si5351's reference frequency. Every
clock derives from it.

The non-obvious part: a reference error moves transmit and receive in
*opposite* directions. Transmit shifts by `(T/C − 1)(f − 22,600)` and
the receive display by `−(T/C − 1)(f − 24,000)`, because the first
mixer's LO sits above the signal. At 7 MHz 1 ppm is about 7 Hz; at
15 MHz about 15 Hz. That opposite sign is what let a disagreement
between a transmit measurement and a receive measurement localize the
error rather than average into a wrong answer. Receiving WWV is the
reference to check it against.

## Interfaces and applications

### Why are there so many ways to talk to maxibitx?

Because they answer different questions, and each was added when
something real needed it. None is a fallback for another.

**HPSDR Protocol 1** (UDP) exists because mature SDR applications
already speak it — it is how SparkSDR and SDR Console get I/Q and
control without anyone writing a maxibitx-specific driver.

**rigctld** (TCP 4532) exists because Hamlib is the lingua franca of
station control, so WSJT-X, FLRig and anything else that knows how to
talk to a radio can tune and key this one.

**The USB gadget** exists because WSJT-X on a host PC needs a *sound
card* and a *serial port*, not a network protocol. The Pi presents
itself as both over one USB cable, which is why FT8 works with the same
setup a commercial rig would use.

**`iq_stream`** (UDP 4536) exists because the control panel needed a
spectrum and neither of the other two was a good fit: HPSDR wants a
discovery handshake and a session, and rigctld carries no samples.
It is deliberately the most minimal thing that works — send any datagram
to subscribe, re-send within five seconds to stay subscribed, and
packets of 128 I/Q pairs arrive with a four-byte magic and a sequence
number.

The underlying principle is in the README: no UI in the process, and
everything a display or controller needs goes through a documented
interface. Several interfaces is the cost of that, and it's a much lower
cost than one interface that has to be all things.

### Which external applications work well with maxibitx?

**SparkSDR** and **SDR Console** over HPSDR Protocol 1 for I/Q
reception; **WSJT-X** for FT8 over the USB gadget's audio and CAT,
decoding on par with SparkSDR on the same I/Q in a side-by-side and
confirmed in both directions by a two-way contact on 2026-09-23;
**FLDigi** over that same gadget audio path; and **FLRig** for control.

Two setup details that cost real time to find, both in
[`06_api.md`](06_api.md). FLRig should be set to the **QMX** rig type,
not Kenwood TS-480. And WSJT-X and FLRig cannot both own the gadget's
COM port at once: either point WSJT-X's Hamlib client straight at the
serial port with FLRig closed, or let FLRig own the port and set WSJT-X's
rig to "FLRig" pointing at its XML-RPC server. The same rule applies to
any other application that wants that port.

### How does maxibitx work with a key and a microphone?

It uses the sBitx hardware's own connections. The straight key is a GPIO
line polled by `cw_poll_key()` once per audio block inside the audio
thread — at audio rate, not on a UI timer, which is what keeps iambic-
style timing honest. The microphone arrives through the WM8731 codec and
is scaled by `mic_tx_gain` on the way into the transmit pipeline.

Remote operation is the incomplete half. Remote PTT works, but in CW it
transmits no carrier, because there is no remote keying path — a
computer can assert PTT but has no way to send elements. The test-tone
generator is the current workaround for making a carrier without a key.
Remote *audio* input exists only for DIGITAL, through the USB gadget.
Remote keying and remote voice audio are both real gaps rather than
oversights; they are listed in
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)'s known
limitations.

### What is `rigctl_panel.py`?

A small standalone Tk control panel, about a thousand lines of Python,
that runs on the Pi or any machine on the network. It shows frequency,
mode, RIT, volume, the narrow-filter switches and its pitch/width
selectors, mic gain, TX power, an S-meter, an ALC meter, a TX test section
and a live spectrum.

Two things make it more interesting than a utility. First, it is a
*client*, not part of the daemon — it speaks the ordinary rigctld
protocol for control and subscribes to `iq_stream` for the spectrum, so
everything it does, any other application can do. It is the working
proof that [`06_api.md`](06_api.md) is sufficient. Second, it is a
development instrument: the `u FFTFILT` switch exists so an operator can
A/B the elliptic and FFT narrow filters on real signals, which is how
the FFT version was found to sound worse than the bench predicted — and
`u MINPHASE` is there for the follow-up A/B, between the two realizations
of that same FFT filter.

### Could `rigctl_panel.py` be replaced by a full-featured SDR application?

Yes, and that is the intended path rather than a concession. Everything
a complete radio application needs is already exposed: I/Q for a
panadapter over HPSDR or `iq_stream`, full tuning, mode, RIT, PTT,
power and metering over rigctld, and demodulated audio over the USB
gadget. [`06_api.md`](06_api.md) exists specifically so somebody can
build that application without modifying the daemon.

There are three shapes it could take. A native app on the Pi 4 driving a
local touchscreen — this is the one that must stay a *separate process*,
because putting a UI back in the daemon reintroduces the single most
expensive bug category in the project's history. A network application
on a desktop, which is what SparkSDR and SDR Console already are, so
this case is partly answered. Or a browser front-end talking to a small
bridge process, which would suit a phone or tablet.

The honest caveat is that a complete application is a large amount of
work, and the parts of it that are radio-specific are the parts already
done. What's left is UI, and UI is most of it.

## Modes and scope

### Why doesn't maxibitx support NBFM, AM or SAM?

The recorded reason in [`ARCHITECTURE.md`](ARCHITECTURE.md) §8 is short:
"AM/FM — not relevant to this HF hardware," alongside onboard digital
mode encode/decode as explicitly deferred. v1 scope was SSB and CW
sharing one pipeline, plus DIGITAL as a mode value for host-generated
audio, and CW-reverse, which has since become a mode of its own (`CWR`).

That reason is sound for transmit and for NBFM generally — this is an HF
SSB radio, its crystal filter is about 2.4 kHz wide, and nothing about
the hardware wants FM. It is thinner for AM and SAM on *receive*, where
shortwave broadcast listening is a real use and the demodulator
architecture would take them fairly cheaply: `rx_audio.c` already has
the complex signal in hand at stage 2, and an envelope detector or a
synchronous detector with a carrier-tracking loop would slot in beside
the existing branch. The honest statement is that they were out of scope
rather than impossible, and that receive-side AM/SAM is a reasonable
thing to ask for.

Note also that the widest filter is set by the crystal filter, so AM
reception would be single-sideband-wide by nature — which is what SAM
would be good at.

## Working on the code

### How do I know a change hasn't broken the DSP?

Run the bench tests — `make test-fft-filter`, `test-tx-pipeline`,
`test-rx-filter`, `test-rx-audio`, `test-upsample48k`. They link the DSP
files without any hardware code, so they run anywhere, and they print
measured numbers against expected ones rather than just passing or
failing. `make check-comments` flags comments that have drifted into
narrating history, and errors outright if a source file's header names a
different file — the signature of a file uploaded under the wrong name,
which has happened twice and broken the build both times.

For a comment-only change there is a stricter check: the machine code
must be identical, compared through `objdump -d` plus the `.rodata` and
`.data` sections. [`code_comments.md`](code_comments.md) has the
procedure.

### Why is there a comment policy?

Because comments that tell stories go stale silently, and a stale
comment is worse than none — it actively misleads. The rule is that a
comment says what the code does *now*: units, ranges, invariants, where
a constant came from, coupling to other code, and known open caveats.
History — earlier versions, bugs and how they were found, on-air
reports, dates, old values, long derivations and measurement tables —
goes in `docs/`, where it can be read as a narrative and where being
about the past is the point. Every fix that teaches something gets both:
a short current-tense comment, and the story in the doc that covers that
behavior.

### What happens if `data/hw_settings.ini` is missing?

The daemon says so and runs on compiled-in defaults. That is the general
pattern: most initialization steps degrade rather than abort. The
settings file carries physical measurements of one specific board —
reference frequency, BFO, crystal filter centre, per-band power scales,
rated power — so running without it gives a working radio with
uncalibrated numbers rather than no radio.

### What's not done?

The current list, from
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) and the
design notes: two-tone power and IMD measurements to establish this
board's honest rated output; voice power never checked on a wattmeter;
`POWER` and `ALC` bench-tested but not yet exercised against a running
daemon; remote keying and remote voice audio absent; carrier placement a
few Hz low from whole-bin rotation (6.25 Hz on SSB, 9.4 Hz on CW); and
the pipeline carrying about a block of the previous transmission in its
overlap-save history, with nothing resetting it between bursts.
