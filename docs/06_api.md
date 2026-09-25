# 06 — maxibitx API

This is the reference for building an application on top of maxibitx
without changing the daemon: a full SDR app, a logger, a remote
control, a digital-mode bridge. Everything an outside program can do
goes through one of four interfaces, and this document describes all
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

Each interface is optional. If one fails to come up (no USB device
port, a port already in use), the daemon logs it and runs without it.
rigctld, iq_stream and HPSDR listen on all network interfaces, with no
authentication - run maxibitx on a trusted network only.

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
| Mode | CW, CWR, USB, LSB, DIGITAL | Selects both the onboard demodulator and the TX audio source (below). Starts in CW. CWR is CW-reverse: the other side of the BFO on receive, identical to CW on transmit. |
| PTT | RX / TX | |
| RIT | −9999 to +9999 Hz, plus an on/off flag | Receive only; never moves the transmit frequency. Survives TX; cleared by a frequency change. |
| Volume | 0-100 % | The radio's own speaker (log taper). Doesn't affect USB audio. |
| Narrow filter | on/off, elliptic or FFT | ~300 Hz around the 700 Hz CW pitch, applied to the demodulated audio (speaker and USB). On by default; turn it off outside CW. |
| Mic gain | 0 to 64, linear multiplier | USB/LSB transmit level. Around 1-5 in practice. |

**What each mode transmits.** TX happens when PTT is set, but the audio
source is fixed by the mode:

| Mode | TX audio source | PTT that produces RF |
|---|---|---|
| CW / CWR | The radio's straight key (700 Hz tone, shaped) | The key only |
| USB / LSB | The radio's mic jack | The mic PTT switch (the same key line) |
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
  the radio stays in receive. Returning to receive is never refused.

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
| `T <0\|1>` | `RPRT 0` | Any nonzero value means TX. Also `RPRT 0` when ignored because the local key holds TX. `RPRT -1` when the dial is outside every `[tx_band]` range - those ranges are enforced, not just advertised. |
| `j` | `-150` | RIT offset, Hz. The stored value: CAT `RT0;` can switch RIT off without zeroing it. |
| `J <Hz>` | `RPRT 0` | −9999 to 9999; `J 0` turns RIT off. Out of range: `RPRT -1`. |
| `l AF` | `0.670000` | Volume, 0.0-1.0 |
| `L AF <0.0-1.0>` | `RPRT 0` | |
| `l STRENGTH` | `-12` | S-meter, integer dB relative to S9 (−54 to +60). Uncalibrated - relative readings only. |
| `l MICGAIN` | `1.000000` | Mic gain - the TX drive control, USB/LSB only (extension) |
| `L MICGAIN <value>` | `RPRT 0` | Clamped to 0-64 (extension) |
| `l RFPOWER` | `1.000000` | TX power, 0.0-1.0 of `max_power` in `data/hw_settings.ini` |
| `L RFPOWER <0.0-1.0>` | `RPRT 0` | Moves the ALC limiter's ceiling. Clamped; it cannot exceed `max_power`. |
| `l ALC` | `0.00` | ALC gain reduction in **dB**, not Hamlib's 0.0-1.0. Peak-held ~1 s so it reads as a meter. Read-only (extension) |
| `u NARROW` / `U NARROW <0\|1>` | `0` / `RPRT 0` | Narrow CW filter on/off (extension) |
| `u FFTFILT` / `U FFTFILT <0\|1>` | `0` / `RPRT 0` | Narrow filter type: 0 elliptic, 1 FFT (extension) |
| `u MINPHASE` / `U MINPHASE <0\|1>` | `1` / `RPRT 0` | Which realization the FFT filter uses: 1 minimum phase (the default, 4.5 ms group delay), 0 linear phase (16 ms). Same magnitude response either way; no effect while the elliptic is selected (extension) |
| `l CWPITCH` / `L CWPITCH <hz>` | `700` / `RPRT 0` | CW pitch: 600, 700 or 800 Hz. Moves four things together — the RX BFO (the tone you hear), stage 3's filter center, the TX sidetone, and the CW IF shift that keeps the carrier on the dial. The transmitted frequency does not change. Refused while transmitting (`RPRT 0`, pitch unchanged — read it back). A real Hamlib level, advertised in `dump_state` |
| `l CWWIDTH` / `L CWWIDTH <hz>` | `300` / `RPRT 0` | Stage 3's width: 150, 300, 450 or 600 Hz (extension). Both settings snap to the nearest value `src/narrow_filter_bank.h` carries; the log line reports which one was selected, and a client should display the `l` readback rather than what it asked for |
| `u TONE` / `U TONE <0\|1\|2>` | `0` / `RPRT 0` | TX test-tone generator: 0 off, 1 single 1 kHz, 2 two-tone 700 + 1900 Hz. Doesn't key the radio; any PTT does. Off after 30 s in TX. Out of range: `RPRT -1` (extension) |
| `v` / `V <vfo>` | `VFOA` / `RPRT 0` | Single VFO; any `V` is accepted. |
| `chk_vfo` | `0` | Not in VFO mode - send commands without a VFO argument. |
| `dump_state` | capability block | Protocol 0. TX ranges come from `data/hw_settings.ini`'s `[tx_band]` entries at 5 W; modes CW/USB/LSB/PKTUSB; max RIT 9999. `has_get_level` is `AF\|CWPITCH\|RFPOWER\|STRENGTH`, `has_set_level` is `AF\|CWPITCH\|RFPOWER`. |
| `q`, `Q`, `quit` | (connection closes) | |

Anything else replies `RPRT -1`. The extensions (`MICGAIN`, `ALC`,
`NARROW`, `FFTFILT`, `MINPHASE`, `CWWIDTH`, `TONE`) aren't Hamlib names or aren't in Hamlib's
units, so a stock Hamlib client won't use them, but they follow the same
syntax. `AF`, `CWPITCH`, `RFPOWER` and `STRENGTH` are real Hamlib levels and are
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
| `AC…;` | — | Ignored (no tuner) |

**DIGITAL over CAT.** A TS-480 has no data mode, so DIGITAL reports as
`MD2` (USB) and can't be selected over CAT - select it with rigctld
(`M PKTUSB 0` or `M DIGITAL 0`) or the control panel. Once in DIGITAL,
an `MD2;` is treated as a no-op, so a program that reads the mode back
and writes it again (WSJT-X does) doesn't knock the radio out of
DIGITAL. `MD1;` or `MD3;` still change the mode.

**FLRig:** choose the **QMX** rig type, not Kenwood TS-480. It sends
only the commands above, and its RIT control maps onto `RT`/`RU`/`RD`.

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

- **Network transmit audio.** There's no way to send voice or data
  audio for transmission over the network. HPSDR's transmit samples
  are ignored, and USB audio is used only in DIGITAL. A network SSB or
  AFSK transmitter needs a new audio input.
- **Network receive audio.** Demodulated audio leaves the Pi only
  through its speaker and USB. A networked app demodulates the I/Q
  itself.
- **Remote CW.** Remote PTT in CW sends no carrier, and there's no
  remote keying command, so CW from a computer (a keyer, CW macros)
  isn't possible.
- **Transmit metering.** Forward/reflected power, SWR, and the INA260's
  supply voltage and current aren't exposed.
- **Change notifications.** State is poll-only. Kenwood `AI` is
  answered but not honored.
- **A PTT safety timeout.** A client that sets TX and disappears
  leaves the radio transmitting. (Transmit *is* refused outside the
  calibrated bands - that check is on the frequency, not on time.)
- **Split and a second VFO**, a variable receive passband, AGC
  settings, and a calibrated S-meter.
- **HPSDR sample rates other than 96 kHz**, and more than one HPSDR
  client.
