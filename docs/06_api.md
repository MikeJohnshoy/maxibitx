# 06 — maxibitx API

This is the reference for building an application on top of maxibitx
without changing the daemon: a full SDR app, a logger, a remote
control, a digital-mode bridge. Everything an outside program can do
goes through one of five interfaces, and this document describes all
of them from the client's side - how to connect, what to send, what
comes back, and in what units.

The last section, "Not available yet", lists what an application
can't do today without daemon changes. Read it before designing
around a feature.

## The interfaces at a glance

| Interface | Transport | For | Clients at once |
|---|---|---|---|
| rigctld | TCP 4532, text | Tuning, mode, PTT, RIT, levels, S-meter | Any number |
| iq_stream | UDP 4536 | Raw baseband I/Q, 96 kHz, simple format | Up to 4 |
| HPSDR Protocol 1 | UDP 1024 | I/Q plus tuning and MOX, for existing SDR apps | 1 |
| USB gadget | USB device port | Audio in and out (48 kHz), Kenwood CAT serial | 1 host; CAT port 1 owner |
| TCI | TCP 50001, WebSocket | Control, receive and transmit audio (48 kHz), I/Q (96 or 48 kHz), CW text; for JTDX, WSJT-X Improved, loggers, Hamlib's TCI backend | Up to 4 (settable, 16 at most) |

Each interface is optional. If one fails to come up (no USB device
port, a port already in use), the daemon logs it and runs without it.
rigctld, iq_stream, HPSDR and TCI listen on all network interfaces, with
no authentication - run maxibitx on a trusted network only. (TCI can be
held to one interface with `tci_bind`, below.)

For a new application, rigctld for control plus iq_stream for I/Q is
the simplest pairing: two small, documented protocols, both usable
from any language.

## Minimal client

Tune to 7.074 MHz, select DIGITAL, and find the strongest signal in the
I/Q. This runs as-is against a real maxibitx (change `PI`):

```python
import socket, struct, time
import numpy as np

PI = "192.168.1.50"            # the Pi's address

# --- Control: rigctld, TCP 4532 -------------------------------------
ctl = socket.create_connection((PI, 4532))
rx = ctl.makefile("r")

def cmd(line):
    ctl.sendall((line + "\n").encode())
    return rx.readline().strip()

print(cmd("F 7074000"))      # RPRT 0
print(cmd("M PKTUSB 0"))     # RPRT 0 (DIGITAL)
print(cmd("f"))              # 7074000

# --- I/Q: iq_stream, UDP 4536 ----------------------------------------
iq = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
iq.settimeout(2.0)
iq.sendto(b"hi", (PI, 4536))             # any datagram subscribes
buf, last_sub = [], time.time()
while len(buf) < 8192:
    data, _ = iq.recvfrom(2048)
    magic, seq, n = struct.unpack(">4sII", data[:12])
    if magic != b"IQS1":
        continue
    s = np.frombuffer(data, ">i2", 2 * n, 12).astype(float).reshape(-1, 2)
    buf.extend(s[:, 0] - 1j * s[:, 1])   # conjugate: the raw I/Q is inverted
    if time.time() - last_sub > 1.0:     # re-subscribe well inside 5 s
        iq.sendto(b"hi", (PI, 4536)); last_sub = time.time()

x = np.array(buf[:8192]) / 32767.0
spec = np.fft.fftshift(np.abs(np.fft.fft(x * np.hanning(len(x)))))
freqs = np.fft.fftshift(np.fft.fftfreq(len(x), 1 / 96000))
print("strongest signal at dial %+.0f Hz" % freqs[np.argmax(spec)])
```

A fuller example is `tools/rigctl_panel.py`: a Tkinter control panel
with tuning, mode, RIT, volume, filters, mic gain, S-meter and a live
spectrum, built only on these two interfaces.

## The radio model

There is one radio state, shared by every interface. A change made
through any of them is seen by all the others; the last write wins.

| State | Values | Notes |
|---|---|---|
| Frequency | Hz, integer | The dial. Changing it clears RIT. |
| Mode | CW, CWR, USB, LSB, DIGITAL | Selects both the onboard demodulator and the TX audio source (below). Starts in DIGITAL. CWR is CW-reverse: the other side of the BFO on receive, identical to CW on transmit. |
| PTT | RX / TX | |
| RIT | −9999 to +9999 Hz, plus an on/off flag | Receive only; never moves the transmit frequency. Survives TX; cleared by a frequency change. |
| Volume | 0-100 % | The radio's own speaker (log taper). Doesn't affect USB audio. |
| Narrow filter | on/off, elliptic or FFT | ~300 Hz around the 700 Hz CW pitch, applied to the demodulated audio (speaker and USB). On by default; turn it off outside CW. |
| Mic gain | 0 to 64, linear multiplier | USB/LSB transmit level. Around 1-5 in practice. |

**What each mode transmits.** TX happens when PTT is set, but the audio
source is fixed by the mode:

| Mode | TX audio source | PTT that produces RF |
|---|---|---|
| CW / CWR | The radio's straight key (700 Hz tone, shaped) | The key only (either contact of the key jack) |
| USB / LSB | The radio's mic jack | The mic PTT switch (the key jack's ring line) |
| DIGITAL | USB gadget audio from the host (WSJT-X) | Any: rigctld `T`, CAT `TX`, HPSDR MOX |
| Any, with the test-tone generator on | 1 kHz tone, or 700 + 1900 Hz two-tone | Any |

Remote PTT in CW, CWR, USB or LSB switches the radio to transmit but sends
silence - there is no network or USB audio path in those modes - unless
the test-tone generator is on (`U TONE`). The generator turns itself
off and drops PTT after 30 s in transmit.

**Precedence rules:**

- **The local key wins.** While the radio's key or mic PTT is holding
  TX, remote PTT requests are acknowledged but ignored.
- **An HPSDR client drives tuning.** While an HPSDR app is streaming,
  maxibitx follows that app's receive frequency on every command
  packet, so a frequency set through rigctld or CAT is overridden by
  the app's next command packet. HPSDR carries no frequency back to
  the app, so with an HPSDR app connected, tune from that app.
- **PTT is not released automatically.** If a client sets TX and then
  disconnects or crashes, the radio stays in TX. There's no timeout.
- **Transmit is refused outside the calibrated bands.** PTT only takes
  effect when the dial is inside one of the `[tx_band]` ranges
  `dump_state` advertises; outside them rigctld answers `RPRT -1` and
  the radio stays in receive. On a board that may not transmit (the
  zBitx, for now) every PTT is refused the same way. Returning to
  receive is never refused.

**No notifications.** Nothing is pushed to clients. To stay in sync with
changes made elsewhere (the key, another app), poll - `rigctl_panel.py`
polls once a second.

## rigctld (TCP 4532)

A subset of Hamlib's `rigctld` text protocol. Stock Hamlib clients work
against it (Hamlib's NET rigctl model, rig 2); it has been checked with
Hamlib 4.5's `rigctl`.

**Framing.** One command per line, `\n`-terminated (`\r\n` is fine).
Commands are case-sensitive. Get commands reply with the value on its
own line; set commands reply `RPRT 0` on success and `RPRT -1` on any
error. A leading `+` (extended-response request) is accepted and
ignored - replies are always the plain form. Long command names
(`\set_freq`) aren't supported, except `\dump_state` and `\chk_vfo`.
Each connection gets its own thread; any number of clients can be
connected.

| Command | Reply | Notes |
|---|---|---|
| `f` | `7074000` | Frequency, Hz |
| `F <Hz>` | `RPRT 0` | Tunes. `RPRT -1` if ≤ 0. Clears RIT. |
| `m` | `PKTUSB` then `2400` | Mode, then passband (two lines). DIGITAL reads as `PKTUSB`. |
| `M <mode> <passband>` | `RPRT 0` | Mode: `CW`, `CWR`, `USB`, `LSB`, `PKTUSB` or `DIGITAL`. Passband is stored and echoed by `m` but not applied. Unknown mode: `RPRT -1`. |
| `t` | `0` or `1` | PTT |
| `T <0\|1>` | `RPRT 0` | Any nonzero value means TX. Also `RPRT 0` when ignored because the local key holds TX. `RPRT -1` when the dial is outside every `[tx_band]` range - those ranges are enforced, not just advertised - on a board that may not transmit, or while `bfo_freq` and `xtal_filter_center` give no usable TX IF (see `XTALCENTER`). |
| `n` | `10` | Tuning step, Hz: how far one click of the radio's tuning knob moves the dial. A push of that knob changes it too. |
| `N <Hz>` | `RPRT 0` | 1 to 10,000,000. Out of range: `RPRT -1`. |
| `j` | `-150` | RIT offset, Hz. The stored value: CAT `RT0;` can switch RIT off without zeroing it. |
| `J <Hz>` | `RPRT 0` | −9999 to 9999; `J 0` turns RIT off. Out of range: `RPRT -1`. |
| `l AF` | `0.670000` | Volume, 0.0-1.0 |
| `L AF <0.0-1.0>` | `RPRT 0` | |
| `u MUTE` / `U MUTE <0\|1>` | `0` / `RPRT 0` | The local speaker muted; the volume is kept and returns on unmute. A push of the volume knob toggles it, and turning that knob unmutes. A real Hamlib function, not advertised in `dump_state` |
| `l STRENGTH` | `-12` | S-meter, integer dB relative to S9 (−54 to +60). Uncalibrated - relative readings only. |
| `l MICGAIN` | `1.000000` | Mic gain - the TX drive control, USB/LSB only (extension) |
| `L MICGAIN <value>` | `RPRT 0` | Clamped to 0-64 (extension) |
| `l RFPOWER` | `1.000000` | TX power, 0.0-1.0 of `max_power` in `data/hw_settings.ini` |
| `L RFPOWER <0.0-1.0>` | `RPRT 0` | Moves the ALC limiter's ceiling. Clamped; it cannot exceed `max_power`. |
| `l ALC` | `0.00` | ALC gain reduction in **dB**, not Hamlib's 0.0-1.0. Peak-held ~1 s so it reads as a meter. Read-only (extension) |
| `u NARROW` / `U NARROW <0\|1>` | `0` / `RPRT 0` | Narrow CW filter on/off (extension). DIGITAL holds it out of circuit whatever is set: WSJT-X's audio is taken downstream of it, so a CW session's 300 Hz filter would hand the decoder a fraction of FT8's ~2.7 kHz window. The `u` form reports the **effective** state, so it reads `0` in DIGITAL; the setting is remembered and comes back on leaving DIGITAL. A `U NARROW 1` sent during DIGITAL still replies `RPRT 0` and takes effect then ([`dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md`](dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md) §11) |
| `u FFTFILT` / `U FFTFILT <0\|1>` | `1` / `RPRT 0` | Which CW filter: 1 the FFT filter (the default, always minimum phase), 0 an elliptic IIR from the pre-designed bank. `CWPITCH`/`CWWIDTH` apply to either (extension) |
| `l CWPITCH` / `L CWPITCH <hz>` | `700` / `RPRT 0` | CW pitch: 500 to 1000 Hz in 100 Hz steps. Moves four things together — the RX BFO (the tone you hear), stage 3's filter center, the TX sidetone, and the CW IF shift that keeps the carrier on the dial. The transmitted frequency does not change. Refused while transmitting (`RPRT 0`, pitch unchanged — read it back). A real Hamlib level, advertised in `dump_state` |
| `l CWWIDTH` / `L CWWIDTH <hz>` | `300` / `RPRT 0` | Stage 3's width: 150, 300, 450 or 600 Hz (extension). Both settings snap to the nearest value `src/narrow_filter_bank.h` carries; the log line reports which one was selected, and a client should display the `l` readback rather than what it asked for |
| `l XTALCENTER` / `L XTALCENTER <hz>` | `40012400` / `RPRT 0` | `xtal_filter_center`, the crystal frequency the dial is mixed onto (extension). The set form moves both receive clocks so the dial stays at the 24 kHz IF, and re-derives the TX IF shifts. It lasts until maxibitx restarts and is never written to `hw_settings.ini`. `RPRT -1`, unchanged, while transmitting or outside 39,900,000-40,100,000 Hz. A value whose TX IF placement is impossible (`bfo_freq − xtal_filter_center` not between the CW pitch and 48 kHz) is accepted, and `T 1` is refused until a usable one is set. For measuring the filter: `tools/xtal_sweep.py` |
| `u KEYER` / `U KEYER <0-4>` | `0` / `RPRT 0` | CW keyer mode: 0 straight key, 1 bug, 2 ultimatic, 3 iambic A, 4 iambic B. Takes effect once the keyer is idle. A mode a `make KEYER=keyer_straight` build doesn't offer: `RPRT -1` (extension) |
| `l KEYSPD` / `L KEYSPD <1-60>` | `20` / `RPRT 0` | Keyer speed, WPM; clamped to 1-60. Takes effect at the next element. A real Hamlib level, advertised in `dump_state` |
| `b <text>` / `\send_morse <text>` | `RPRT 0` | Sends the text as CW at `KEYSPD`, in CW or CWR (`RPRT -1` in any other mode). Letters in either case, digits, `. , ? ' ! / ( ) : ; - _ " @ $ + = &`, and prosigns as `<AR>` `<AS>` `<BK>` `<BT>` `<HH>` `<KN>` `<SK>` `<SN>`; anything else is skipped and named on the console. `3T` between characters, `7T` between words (a run of spaces is one). Queued behind text still going out; `RPRT -1` if the 512-character queue can't take all of it. TX is requested at once. Touching the key or paddle stops it after the element being sent |
| `\stop_morse` | `RPRT 0` | Stops text: the element being sent completes, the rest is dropped |
| `u MORSE` | `1` | 1 while text is queued or being sent (extension, read-only) |
| `u PADREV` / `U PADREV <0\|1>` | `0` / `RPRT 0` | Paddle reversal: 0 tip = dot, ring = dash (the default); 1 swaps them. No effect on a straight key, which keys from either contact (extension) |
| `u TONE` / `U TONE <0\|1\|2>` | `0` / `RPRT 0` | TX test-tone generator: 0 off, 1 single 1 kHz, 2 two-tone 700 + 1900 Hz. Doesn't key the radio; any PTT does. Off after 30 s in TX. Out of range: `RPRT -1` (extension) |
| `v` / `V <vfo>` | `VFOA` / `RPRT 0` | Single VFO; any `V` is accepted. |
| `chk_vfo` | `0` | Not in VFO mode - send commands without a VFO argument. |
| `dump_state` | capability block | Protocol 0. TX ranges come from `data/hw_settings.ini`'s `[tx_band]` entries at 5 W (1.8-30 MHz if it has none), and there are none on a board that may not transmit; modes CW/USB/LSB/PKTUSB; max RIT 9999. `has_get_level` is `AF\|CWPITCH\|RFPOWER\|KEYSPD\|STRENGTH` (`0x40005808`), `has_set_level` is `AF\|CWPITCH\|RFPOWER\|KEYSPD` (`0x5808`). |
| `q`, `Q`, `quit` | (connection closes) | |

Anything else replies `RPRT -1`. The extensions (`MICGAIN`, `ALC`,
`NARROW`, `FFTFILT`, `CWWIDTH`, `XTALCENTER`, `TONE`, `PADREV`, `KEYER`, `MORSE`) aren't Hamlib names or aren't in Hamlib's
units, so a stock Hamlib client won't use them, but they follow the same
syntax. `AF`, `CWPITCH`, `KEYSPD`, `RFPOWER` and `STRENGTH` are real Hamlib levels and are
advertised in `dump_state`.

Two of these are easy to confuse. `RFPOWER` sets a ceiling: it lowers
the limiter's threshold, so turning it up can never exceed `max_power`.
`MICGAIN` is drive: it decides how hard the signal is pushed into
whatever ceiling is in force, so past the ceiling it moves `ALC` rather
than output power. To set a voice level, advance `MICGAIN` until speech
peaks read a couple of dB on `ALC`. In DIGITAL there is no radio-side
drive at all - set the host's output level while watching `ALC`.
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md),
"Setting power", has the whole chain.

## iq_stream (UDP 4536)

The simplest way to get I/Q: no discovery and no handshake.

**Subscribing.** Send any UDP datagram (contents ignored) to port 4536.
Data packets are sent back to the address and port the datagram came
from. Re-send at least once every 5 seconds to stay subscribed; stop,
and you're dropped. Up to 4 subscribers; a fifth replaces the one
heard from least recently. It runs independently of HPSDR - both can
be active at once.

**Packets.** One every 1.333 ms (128 samples at 96 kHz). All fields are
big-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic, ASCII `IQS1` |
| 4 | 4 | Sequence number, uint32, +1 per packet (gaps mean loss) |
| 8 | 4 | Sample count *n*, uint32 (128 today - read it, don't assume it) |
| 12 | 4·*n* | *n* pairs of int16 I, int16 Q |

**Samples.** 96 kHz complex baseband, centered on the dial (0 Hz = the
dial frequency plus any RIT offset), covering about ±32 kHz before the
anti-alias filter rolls off. Scaled so full scale is ±32767; levels are
relative, not calibrated.

**The I/Q is spectrally inverted.** A station *d* Hz above the dial
arrives at −*d* Hz if read as I + jQ. Use I − jQ (negate Q) to get the
conventional orientation, as the example above does. This comes from
the radio's mixer chain, and HPSDR carries the same samples (see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md)).

## HPSDR Protocol 1 (UDP 1024)

For existing SDR applications (SparkSDR, SDR Console and others) that
already speak openHPSDR Protocol 1. A new application is better off
with iq_stream and rigctld, which are simpler and have no
single-client limit.

- **Discovery** answers as a Hermes-Lite 2 (board type `0x88`). A
  second client's discovery gets the "busy" reply while one is
  streaming.
- **One client at a time.** A start command from a new address takes
  the stream over from the current one.
- **Receive:** a single receiver at a fixed 96 kHz; the sample-rate
  request isn't read, so set the app to 96 kHz. 24-bit I/Q, with the
  same spectral orientation as iq_stream - SparkSDR displays and
  decodes it correctly as-is.
- **Read from the client:** the RX1 frequency (addr 0x02) - maxibitx
  tunes to it while receiving; the TX frequency (addr 0x01), used when
  MOX is set; and MOX (addr 0). Everything else in the command packets,
  including the transmit I/Q samples, is ignored.
- **Reported back:** PTT (C0 bit 0) and the five status fields,
  rotating one per frame as on real hardware - all zero except a
  firmware version (0x4A) in field 0. The CW dot/dash bits are always
  0, and no frequency, temperature or power is reported.

## USB gadget (audio + Kenwood CAT)

When the Pi's USB-C port is in device mode (setup in
[`07_build_and_deployment.md`](07_build_and_deployment.md) and
[`dsp_design_notes/usb_gadget_OS_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md)),
maxibitx appears to the host computer as a composite USB device,
"sBitx Audio", with two functions. Windows 10/11 binds both with its
built-in drivers; they're standard USB audio and serial classes.

### USB audio

48 kHz, 16-bit, mono, in both directions - the only format offered.
Set the application's device to exactly that; don't rely on the host
converting.

- **Radio → host** (a microphone input on the host; on Windows
  "Microphone (Source/Sink)"): the demodulated receive audio in the
  current mode, taken before the volume control, so the level the host
  sees doesn't move with the speaker volume. In USB and DIGITAL, audio
  frequency equals distance above the dial, the convention WSJT-X
  expects. It has 15 dB of headroom below full scale.
- **Host → radio** (a speaker output on the host; on Windows
  "Speakers (Source/Sink)"): transmit audio, used only in DIGITAL and
  only while PTT is set. It's sent upper sideband regardless of band.
  In any other mode it's discarded.

Either direction works without the other.

### Kenwood CAT (USB serial)

A Kenwood TS-480 command subset on the gadget's serial port
(`/dev/ttyACM*` on Linux, a COM port on Windows). It's a virtual port,
so any baud rate works. **Only one program can have it open** - on
Windows a second one fails silently. Commands end with `;`. Set
commands get no reply; get commands reply with the same prefix; unknown
commands are silently ignored (Kenwood convention).

| Command | Reply | Notes |
|---|---|---|
| `ID;` | `ID020;` | TS-480 |
| `FA;` / `FA00007074000;` | `FA00007074000;` / — | Frequency, 11 digits Hz |
| `FB;` / `FB…;` | mirrors `FA` / ignored | Single VFO |
| `MD;` / `MD2;` | `MD2;` / — | 1 LSB, 2 USB, 3 CW, 7 CW-Reverse |
| `TX;` / `RX;` | — | PTT on / off |
| `TQ;` / `TQ1;` | `TQ0;` / — | PTT get / set |
| `RT;` / `RT1;` | `RT0;` / — | RIT on/off (offset kept) |
| `RU;` / `RD;` | — | RIT ±10 Hz per command |
| `RC;` | — | RIT cleared to 0 and off |
| `AG0;` / `AG0128;` | `AG0171;` / — | Volume, 000-255 |
| `IF;` | 38-byte status | Frequency, RIT, RIT on, TX/RX, mode, in Hamlib's TS-480 layout |
| `PS;` | `PS1;` | Always on; a set is ignored |
| `AI;` | `AI0;` | No auto-information; a set is ignored |
| `KS;` / `KS025;` | `KS020;` / — | Keyer speed, WPM; clamped to 1-60 (the TS-480's range is 10-60) |
| `KY;` / `KY CQ DE KB2ML;` | `KY0;` / — | Sends text as CW, CW/CWR only. Get: `KY0` while a 24-character message fits, `KY1` when the buffer is full (TS-480). Set: the TS-480's 24 space-padded characters or a longer QMX-style message; trailing padding is one word space. Kenwood prosigns: `[` BT, `_` AR, `<` AS, `#` HH, `>` SK, `]` KN, `\` BK, `%` SN |
| `AC…;` | — | Ignored (no tuner) |

**DIGITAL over CAT.** A TS-480 has no data mode, so DIGITAL reports as
`MD2` (USB) and can't be selected over CAT - select it with rigctld
(`M PKTUSB 0` or `M DIGITAL 0`) or the control panel. Once in DIGITAL,
an `MD2;` is treated as a no-op, so a program that reads the mode back
and writes it again (WSJT-X does) doesn't knock the radio out of
DIGITAL. `MD1;` or `MD3;` still change the mode.

**FLRig:** choose the **QMX** rig type, not Kenwood TS-480. It sends
only the commands above, and its RIT control maps onto `RT`/`RU`/`RD`.

## TCI (TCP 50001)

Expert Electronics' Transceiver Control Interface, v2.0 (the
specification is at [github.com/ExpertSDR3/TCI](https://github.com/ExpertSDR3/TCI)):
one WebSocket connection carrying text commands and binary audio and I/Q
frames. For programs that connect to a TCI radio - JTDX, WSJT-X Improved,
MSHV, loggers, and anything using Hamlib's TCI backend (model 43001) -
it replaces CAT and the USB gadget with one network connection. Several
clients can be connected at once, and a change made by any of them, or
anywhere else in maxibitx, is sent to all. What the implementation does
and why is in [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
and [`dsp_design_notes/tci_design_study.md`](dsp_design_notes/tci_design_study.md).

**Connecting.** `ws://<pi>:50001/` (any path). On connect the server sends
`protocol:ExpertSDR3,2.0;` and `device:maxibitx;`, the initialization
commands, the current state, then `start;` and `ready;` - 33 messages,
one command each. maxibitx identifies as protocol ExpertSDR3 because
that is what JTDX keys its TCI behaviour to (compare the Kenwood surface
answering `ID020;`); `device` says what it really is.

**Framing.** Commands are `name:arg,arg;`, read in either case, several to
a message if a client likes. The server sends lowercase, one command per
message. Every set is answered to every client with the value actually
in force, even when it didn't change; a read (the command without its
value) is answered to the asker alone. A command the server doesn't know
is ignored. Receiver and transceiver numbers other than 0 are ignored.

| Command | Notes |
|---|---|
| `vfo:0,0[,hz];` | The dial, whole Hz, 100 kHz-30 MHz. Channel B (`vfo:0,1`) reads as A; setting it does nothing. |
| `dds:0[,hz];` | The same frequency: the I/Q is centred on the dial. `if:0,ch;` is always 0. |
| `modulation:0[,mode];` | `usb`, `lsb`, `cw`, `cwr`, `digu` (DIGITAL). Another name is answered with the mode in force. |
| `trx:0[,true\|false[,source]];` | PTT - see below. |
| `drive:0[,0-100];` | Transmit power, percent of `max_power` (rigctld's `RFPOWER`). `tune_drive` reads the same. |
| `volume[:dB];` | Speaker volume, -60 to 0 dB; 0.5 dB per percent of rigctld's `AF`, so -50 dB and below is silent (read as -60). |
| `mute[:true\|false];`, `rx_mute:0[,…];` | Volume to 0, and back to where it was. |
| `rit_enable:0[,…];`, `rit_offset:0[,hz];` | RIT, receive only, ±9999 Hz; the offset and the switch are set separately. |
| `cw_macros_speed[:wpm];`, `cw_keyer_speed[:wpm];` | The keyer's one speed, 1-60 WPM. |
| `rx_smeter:0,0;` | `rx_smeter:0,0,<dBm>;` - rigctld's `STRENGTH` with S9 = -73 dBm; uncalibrated. |
| `rx_sensors_enable:true[,ms];` | `rx_sensors:0,<dBm>;` and `rx_channel_sensors:0,0,<dBm>;` every ms (30-1000, default 200), one decimal place. |
| `tx_sensors_enable:…;` | Accepted; no `tx_sensors` are sent, since a DE board measures no power or SWR. |
| `start;` | Answered `start;`. `stop;` is ignored: maxibitx has no stopped state. |
| `split_enable`, `xit_enable`, `xit_offset`, `tune`, `rx_nb_enable`, `rx_nr_enable`, `rx_anf_enable`, `sql_enable`, `lock`, … | Not available: always read false or 0, and a set is answered that way. |

Changes made elsewhere - rigctld, CAT, the panel, the key - reach TCI
clients within 50 ms. `tx_enable:0,<true|false>;` is sent when the band
changes between one maxibitx may transmit on and one it may not
(`[tx_band]` in `hw_settings.ini`). It is always false on a board that
may not transmit.

**Streams**, per client. Each binary frame is a 64-byte header of sixteen
little-endian `uint32` - receiver, sample_rate, format (0 int16, 1 int24,
2 int32, 3 float32), codec, crc, length (samples, counting both channels),
type (0 I/Q, 1 RX audio, 2 TX audio, 3 TX_CHRONO), channels, then eight
reserved - followed by the samples.

| Command | Notes |
|---|---|
| `audio_start:0;` / `audio_stop:0;` | Receive audio: the demodulated audio in the current mode, taken before the volume control, 15 dB of headroom (as the USB gadget's). Default 48 kHz, float32, 2 channels (the same sample on both), length 2048. |
| `audio_samplerate:8000\|12000\|24000\|48000;` | Lower rates through a decimating lowpass at 0.45 of the new rate. |
| `audio_stream_sample_type:int16\|int24\|int32\|float32;`, `audio_stream_channels:1\|2;`, `audio_stream_samples:100-2048;` | Format, channels, and length per frame (default 2048 at 48 kHz, 1024 at 24, 512 at 12, 256 at 8). |
| `iq_start:0;` / `iq_stop:0;`, `iq_samplerate:48000\|96000;` | I/Q, float32 complex, centred on the dial. 96 kHz (the default) as captured; 48 kHz decimated, flat to about ±18 kHz. 192 and 384 kHz aren't available and are answered with the rate in force. Unlike iq_stream and HPSDR, TCI's I/Q is the conventional way up: a station above the dial is at positive frequency (confirmed with sdrOxide). |
| `tx_stream_audio_buffering:50-500;` | Transmit audio kept requested ahead, ms (default 50). |

**PTT and transmit audio.** `trx:0,true,tci;` keys the transmitter with
this client supplying the audio, in USB, LSB or DIGITAL: the server sends
it TX_CHRONO frames (48 kHz, float32, 2 channels, length 2048), and it
answers each with a TX audio frame of that length. Stereo is assumed and
the left channel is used; `channels` and any payload past `length` are
ignored, as JTDX sends them. A client with nothing to send may send zeros
or skip a request. Without the `tci` source (`trx:0,true;`, or Hamlib's
`trx:0,true,Vac;`) PTT behaves as rigctld's `T`: DIGITAL transmits the
USB gadget's audio, and USB, LSB and CW transmit nothing of their own.
In every case: refused outside the `[tx_band]` table (answered
`trx:0,false;`), ignored while the local key holds TX, ignored from other
TCI clients while one holds TX (so one can't unkey another), and
released if the client that set it disconnects.

**CW text**, in CW and CWR only (ignored, with a console line, in other
modes), sent by the keyer as rigctld's `b` is - at the keyer's speed,
with `3T` and `7T` spacing, and stopped by any touch of the key or
paddle:

| Command | Notes |
|---|---|
| `cw_macros:0,<text>;` | Queued in order. Prosigns between bars, `\|AR\|` `\|SK\|` `\|BT\|` `\|KN\|` `\|AS\|` `\|BK\|` `\|HH\|` `\|SN\|`; other letters between bars are sent as letters. `<` and `>` send what follows 5 WPM slower or faster, and the next macro starts at the keyer's speed again. `^`, `~`, `*` stand for `:`, `,`, `;`. Spaces are kept as sent, so successive macros need their own. |
| `cw_msg:0,<prefix>,<callsign>,<suffix>;` | A message, a word space between the parts; `_` is an empty part, as is a missing suffix; `<callsign>$2` sends the callsign twice (up to 5). Stops any text being sent; a `cw_macros` arriving during it waits until it is out. |
| `cw_msg:<callsign>;` | Corrects the callsign of the message being sent, for every character not yet started except the next. Ignored once the callsign is out. |
| `callsign_send:<callsign>;` | Server to every client: the callsign, as corrected, has been sent. |
| `cw_macros_stop;` | The element being sent completes; the rest is dropped. |
| `cw_terminal:true\|false;` | Terminal mode: TX stays on after the text ends, until set false (or `cw_macros_stop`, a touch of the key, or the client disconnecting); `cw_macros_empty;` is sent to every client as the last character queued starts. Answered to every client. |
| `cw_macros_speed_up:N;`, `cw_macros_speed_down:N;` | The keyer's speed by N WPM; the speeds are echoed. |
| `cw_macros_delay;` | Answered `cw_macros_delay:0;` - TX starts as soon as the T/R sequence allows. |

**Settings**, top-level keys in `data/hw_settings.ini`, above the first
`[section]`:

```
tci_port=50001        # 0 turns TCI off
tci_max_clients=4     # 1-16
tci_bind=192.168.1.50 # listen on this interface only; all if absent
```

**With JTDX or WSJT-X Improved.** Settings → Radio: Rig "TCI Client
RX1", with `<pi>:50001` where the serial port would go; PTT method CAT;
and "Use TCI Audio" so the audio comes over TCI rather than from a sound
card. With the mode setting on "Data/Pkt" the program selects DIGITAL
(`digu`) itself. Not yet tried on the air.

`tools/tci_client.py` is a small standard-library client that connects,
checks the initialization burst, and can send commands, record receive
audio, report I/Q, or transmit a test tone.

## Using it with WSJT-X

On-air confirmed on Windows 11: WSJT-X decodes FT8 on par with SparkSDR
on the same I/Q. Transmitting FT8 hasn't been confirmed by another
station yet.

1. **Rig control**, one of:
   - Direct: WSJT-X Settings → Radio → Rig "Kenwood TS-480", Serial Port
     = the gadget's COM port. FLRig must not have the port open.
   - Through FLRig: FLRig owns the COM port (QMX rig type); in WSJT-X
     set Rig to "FLRig" and point it at FLRig's XML-RPC server
     (127.0.0.1:12345 by default). Use this when you want FLRig open
     too.
   - rigctld (Rig "Hamlib NET rigctl", 127.0.0.1:4532 or the Pi's
     address) should also work, but hasn't been tried.
2. **Select DIGITAL** with the control panel or rigctld (`M PKTUSB 0`),
   not over CAT (see above). Turn the narrow filter off.
3. **Audio:** Input "Microphone (Source/Sink)", Output "Speakers
   (Source/Sink)"; set WSJT-X's input channel to Mono. If Windows won't
   open the device by name after a firmware change altered its format,
   see `ARCHITECTURE.md` §10 step 11 (Windows caches the old format
   against the device's serial number).
4. **PTT:** CAT. The radio's key line is ignored in DIGITAL.
5. **Split:** off. `FR`/`FT`/split aren't implemented.

Transmit power in DIGITAL hasn't been calibrated on a wattmeter - see
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md),
"Known limitations".

## Not available yet

What an application can't do today without changes to the daemon:

- **Network audio other than TCI's.** HPSDR's transmit samples are
  ignored; receive and transmit audio over the network go through TCI.
- **Remote element keying.** CW from a computer goes as text (`b`,
  `KY`, TCI's `cw_macros` and `cw_msg`), sent by the keyer; TCI's `keyer`
  command is ignored; there's no way to key individual elements
  remotely, and remote PTT in CW on its own sends no carrier.
- **Transmit metering.** Forward/reflected power, SWR, and the INA260's
  supply voltage and current aren't exposed.
- **Change notifications**, except over TCI, which pushes every change.
  rigctld and CAT are poll-only; Kenwood `AI` is answered but not
  honored.
- **A PTT safety timeout.** A TCI client that disconnects while holding
  TX releases it, but any other client that sets TX and disappears
  leaves the radio transmitting. (Transmit *is* refused outside the
  calibrated bands - that check is on the frequency, not on time.)
- **Split and a second VFO**, a variable receive passband, AGC
  settings, and a calibrated S-meter.
- **HPSDR sample rates other than 96 kHz**, and more than one HPSDR
  client.
- **TCI's 192 and 384 kHz I/Q**, second receiver, and line-out stream.
