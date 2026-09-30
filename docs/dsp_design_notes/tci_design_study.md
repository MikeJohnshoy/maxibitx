# TCI server: a design study

Status: **study only - nothing is implemented.** Written before any code,
as the keyer's was ([`cw_keyer_design_study.md`](cw_keyer_design_study.md)),
to find out whether a TCI server fits maxibitx, what it has to do to work
with the programs that would actually connect to it, and in what order to
build it. Where this study says a client does something, it was read in
that client's source unless it says otherwise; predictions are labelled
as predictions.

The request: offer TCI (Expert Electronics' Transceiver Control Interface)
as an alternative to `hpsdr_p1.c` and the USB audio gadget.

## 1. The answer in brief

**Feasible, and a good fit.** TCI is one WebSocket connection carrying
text commands for control and binary frames for audio and I/Q. maxibitx
already has every piece of radio behind it: the control calls
(`radio.h`, `rx_audio.h`, `sound.h`), 48 kHz demodulated RX audio (made for
the USB gadget), a 48 kHz TX audio path into the transmit pipeline (made
for DIGITAL), 96 kHz I/Q (made for HPSDR and `iq_stream.c`) and a keyer
that sends text (`keyer_send_text()`). What is new is the transport - a
small WebSocket server - and the protocol layer on top.

**It complements HPSDR and the gadget rather than replacing them** (§3).
The programs that speak TCI as clients are digital-mode programs (JTDX,
MSHV, WSJT-X Improved), loggers (Log4OM, RUMlog, Swisslog and others)
and anything using Hamlib's TCI backend. For them, one TCI connection
replaces CAT plus a USB sound device, over the network, and adds CW
macros. The panadapter programs maxibitx serves over HPSDR - SparkSDR,
Thetis - are TCI *servers* themselves, the radio side for other programs,
so they will not connect to maxibitx over TCI. Upstream WSJT-X can use
Hamlib's TCI backend for CAT, but that backend discards audio, so its
audio still needs the gadget.

**The protocol is easy; compatibility is the work.** The spec is short.
What decides whether JTDX connects is a set of expectations no spec
states - lowercase keywords, one command per WebSocket message, an echo
of every set, `start;` before `ready;`, a sensor value that must contain a
decimal point or the client crashes - collected in §4 from reading the
clients and from three open-source TCI servers that learned them the hard
way.

**Size, as a prediction:** about 1500-2000 lines - comparable to
`usb_gadget.c` - in four new files, plus bench harnesses. Nothing in the
audio thread changes shape: it gains two ring-buffer writes and one
ring-buffer read per block, the same kind it already does for the gadget
and `iq_stream.c`.

## 2. What TCI is

Sources: the official specification, *TCI Protocol* v2.0 (12 January
2024, [ExpertSDR3/TCI](https://github.com/ExpertSDR3/TCI), MIT-style
licence), and the server and client sources in §4.

**Transport.** A WebSocket (RFC 6455) server on TCP. Text frames carry
commands; binary frames carry streams. Several clients may connect at
once, and the server keeps them in step: any change - made at the radio
or by any client - is sent to every client.

**Commands.** `name:arg1,arg2;` - `:`, `,` and `;` are reserved; the spec
says case does not matter. A set is `vfo:0,0,7030000;`; a read is the
same command without the value, `vfo:0,0;`, answered with the full form.
The spec groups them as:

- **Initialization**, sent by the server on connect: `protocol`, `device`,
  `receive_only`, `trx_count`, `channel_count`, `vfo_limits`, `if_limits`,
  `modulations_list`, then `ready`.
- **Bidirectional control**: `start`/`stop`, `dds` (panorama centre),
  `if` (offset within it), `vfo`, `modulation`, `trx` (PTT, with an
  optional audio source: `tci`, `mic1`, `mic2`, `micpc`, `ecoder2`),
  `tune`, `drive` (0-100), `rit_enable`/`rit_offset`, `xit_*`,
  `split_enable`, `volume` (dB, -60 to 0), `mute`, `cw_macros_speed`,
  `cw_keyer_speed`, and many DSP switches (NB, NR, ANF, AGC, squelch).
- **Unidirectional**, per client: `iq_start`/`iq_stop`, `iq_samplerate`
  (48/96/192/384 kHz), `audio_start`/`audio_stop`, `audio_samplerate`
  (8/12/24/48 kHz), `audio_stream_sample_type` (int16, int24, int32,
  float32), `audio_stream_channels`, `audio_stream_samples`,
  `tx_stream_audio_buffering` (50-500 ms, default 50), `rx_sensors_enable`,
  `tx_sensors_enable`, `spot`.
- **Notifications**, server to client: `rx_channel_sensors` (dBm; the
  older `rx_sensors` and `rx_smeter` are what some clients still use),
  `tx_sensors` (mic dBm, power W, peak W, SWR), `tx_frequency`,
  `tx_enable` (whether TX is allowed on this band), `cw_macros_empty`,
  `callsign_send`.

**Streams.** Every binary frame is a 64-byte header of sixteen `uint32`s -
`receiver`, `sample_rate`, `format`, `codec` (0), `crc` (0), `length`,
`type`, `channels`, eight reserved - then the samples. `type` is 0 I/Q,
1 RX audio, 2 TX audio, 3 TX_CHRONO, 4 line out. `length` is the number of
real samples - so stereo audio of 1024 frames has `length` 2048. RX audio
defaults to 48 kHz, float32, 2 channels, 2048 samples per frame.

**Transmit audio is pulled, not pushed.** The server sends TX_CHRONO
frames, each asking for `length` samples; the client answers each with a
TX audio frame of that length. The server sends them on its own clock
without waiting; a client that has nothing to send may skip one, and
sending zeros is preferred.

**CW.** Two ways to send text (§8): `cw_macros:0,<text>;`, queued in
order, with prosigns between vertical bars (`|SK|`), `<`/`>` to lower or
raise the speed by 5 WPM mid-text, and `^ ~ *` standing for the reserved
`: , ;`; and `cw_msg:0,<prefix>,<callsign>,<suffix>;`, whose callsign can
be corrected (`cw_msg:<callsign>;`) until it has been sent, reported by
`callsign_send`. `cw_macros_stop;` stops either. `cw_terminal:true;` keeps
the transmitter on after a macro ends.

## 3. Who connects

From Expert Electronics' list of software with TCI support
([eesdr.com](https://eesdr.com/en/software-en/software-en)), and the
sources read for §4:

| Kind | Programs | What they use |
|---|---|---|
| Digital modes | JTDX, MSHV, WSJT-X Improved | Control, RX and TX audio |
| Loggers | Log4OM, RUMlog, Swisslog, MacLoggerDX, LogHX, 5MContest, OCLog | Frequency and mode; CW macros for the contest loggers |
| Bridges | Hamlib's TCI backend (so any Hamlib program, including upstream WSJT-X, for CAT); VSELink (N1MM+) | Control; Hamlib also `cw_macros` |
| Accessories | amplifiers (RF2K-S and others), tuners, antenna switches, ESP32 libraries | Frequency and `trx` |

**Not TCI clients:** SparkSDR and Thetis run TCI *servers* - they are the
radio side for the programs above - and ExpertSDR3 is the reference
server. So TCI does not replace HPSDR for panadapter use; HPSDR stays.

What this means for maxibitx's own users:

- **A WSJT-X Improved or JTDX user on a PC** gets control, receive and
  transmit audio over one network connection - no USB gadget, no virtual
  audio cable, no COM port.
- **An upstream WSJT-X user** keeps using the gadget (or Hamlib's rigctld
  backend, as now); TCI adds nothing there without audio.
- **A contest logger** gets CW macros straight into the keyer - the
  closest thing yet to the remote CW the API docs list as missing.

## 4. What the clients actually need

Read in source by a research pass (paths are in the projects named;
clones kept outside the repo):

**Hamlib** (`rigs/tci/tci2.c`, Hamlib master, September 2026). Control
only - it discards binary frames.
- Connects, then reads frames until `READY`, failing if it hasn't arrived
  within 256 frames (binary ones count). On the way it records `DEVICE`,
  `MODULATIONS_LIST`, `VFO`, `MODULATION`, `TRX`, `VFO_LIMITS`,
  `TRX_COUNT`, and builds its mode table from `MODULATIONS_LIST`: a mode
  missing from the list can't be set.
- Sends UPPERCASE keywords; sets are fire-and-forget; reads (`VFO`,
  `MODULATION`, `TRX`, `SPLIT_ENABLE`) wait 2 s for a reply matched by
  prefix, retrying by reconnecting.
- Treats each text frame as exactly one command, matched at its start -
  **one command per frame.**
- PTT with a data source sends `TRX:0,true,Vac` - not a source the spec
  lists.
- Knows CW and CWR but not CWL/CWU.
- Default target `127.0.0.1:50001`.

**JTDX** (`TCITransceiver.cpp`) - the strictest client.
- Matches keywords as exact **lowercase** strings.
- After connecting waits up to 1.5 s for `ready`; **a `start;` must have
  arrived by then** or it reports "TCI SDR is not switched on".
- `protocol:ExpertSDR3,…` (or `Thetis`) turns on behaviour it needs: mode
  names compared in lowercase, and `trx:0,true,tci;` for PTT. A `device`
  named SunSDR2DX/PRO without that protocol halves TX amplitude.
- **Waits for the echo of every set** - 2 s for `vfo`, 1 s for
  `modulation` and `trx`, 500 ms for `split_enable` and `audio_start` -
  and drops the connection if it doesn't come. The echo is compared as a
  string: `vfo:0,0,14074000` must come back exactly, whole Hz.
- **Never sets the audio format.** It ignores the header's rate, format
  and channels and assumes 48 kHz, float32, stereo, `length` = total
  floats; it decimates the left channel to 12 kHz itself.
- Answers each TX_CHRONO with a TX audio frame copying the chrono's rate,
  format and `length`, filling `length` floats as L=R pairs, always
  float32; the frame's payload is sized larger than `length` and the
  excess is junk, and `channels` is left uninitialised.
- Parses `tx_sensors`/`tx_power`/`tx_swr` values with `split(".")[1]` -
  **a value without a decimal point crashes it.** `drive`, `trx` and
  `modulation` need all their arguments.
- Falls back to port 40001 if none is set.

**WSJT-X Improved** (`TCITransceiver.cpp` in its suite) - the same code
as JTDX, except that it waits a fixed 2 s after connecting (so everything,
`start;` included, must arrive inside 2 s) and sends `split_enable:false;`
with no transceiver number.

**What the open-source servers learned** (Thetis `TCIServer.cs`,
AetherSDR `TciProtocol.cpp`/`TciServer.cpp`, OK1BR sdr-for-linux
`tci_server.c`):
- All send lowercase and one command per frame, and put `start;` before
  `ready;` (AetherSDR learned this from a bug report; sdr-for-linux sends
  it after, copying piHPSDR, which works only because JTDX waits).
- AetherSDR identifies as `protocol:ExpertSDR3,1.5` with its own `device`
  name, to get JTDX's ExpertSDR3 behaviour without the SunSDR halving,
  and includes `drive` in the init burst to avoid a JTDX crash.
- TX_CHRONO: AetherSDR sends `length` 2048, `channels` 2, float32 on a
  clock; Thetis keeps about `buffering + 50 ms` requested ahead with at
  most 64 chronos outstanding. On receive both trust `length`, not the
  payload size or `channels`.
- A claim in one AetherSDR issue that WSJT-X/JTDX "negotiate int16" is
  not borne out by either client's source.

**Rules for maxibitx's server, from all of the above:**

1. Send lowercase keywords, one command per WebSocket frame; parse
   commands case-insensitively and several per frame.
2. On connect send, in order: `protocol:ExpertSDR3,2.0;`,
   `device:maxibitx;`, the initialization commands, the current state
   (`vfo`, `modulation`, `drive`, `trx`, `split_enable`, `rit_*`,
   `volume`, `tx_enable`), `start;`, then `ready;` - well under 256 frames
   and 1.5 s.
3. Echo every set to every client, with the value actually in force, and
   answer every read.
4. Frequencies in whole Hz; mode names lowercase; sensor values always
   with a decimal point.
5. Audio defaults 48 kHz, float32, stereo, 2048 samples per frame;
   `length` is total samples.
6. TX audio: trust the header's `length`, take the left channel, ignore
   `channels` and extra payload; accept any `trx` audio source string.

`protocol:ExpertSDR3` is a compatibility identifier - JTDX's behaviour is
keyed to it - in the same way the Kenwood CAT surface answers `ID020;`
(TS-480). `device:maxibitx` says what the radio really is.

## 5. What maxibitx already has

| TCI | maxibitx | Notes |
|---|---|---|
| `vfo`, `dds` | `radio_tune_to()`, `freq_hdr` | One VFO. The I/Q is centred on the dial, so `dds` = `vfo` and `if` = 0. Channel B (`vfo:0,1`) is answered with A. |
| `modulation` | `radio_set_mode()` | `usb`, `lsb`, `cw`/`cwu` → CW, `cwl`/`cwr` → CWR, `digu` → DIGITAL. §6. |
| `trx` | `radio_set_tx()` with the same rules as rigctld `T` and CAT `TX`: refused outside the `[tx_band]` table, ignored while the local key holds TX | The echo reports the state actually in force. |
| `tx_enable` | `hw_settings_tx_allowed(freq_hdr)` | Sent on connect and on band change. |
| `drive` | `sound_set_tx_power()` (0-1 of `max_power`) | `drive/100`. |
| `rit_enable`, `rit_offset` | `radio_set_rit_enabled()`, `radio_set_rit()` | RX only, as rigctld `J`. |
| `split_enable`, `xit_*` | none | Always echoed `false` / 0. |
| `volume`, `mute` | `rx_audio_set_volume()` (0-100) | dB ↔ percent mapping to settle (§12). |
| `cw_macros_speed`, `cw_keyer_speed` | `keyer_set_wpm()` | One speed for both in maxibitx. |
| `cw_macros`, `cw_msg`, `cw_macros_stop` | `keyer_send_text()`, `keyer_stop_text()`, `cw_text_queued()` | §8. |
| `rx_channel_sensors`, `rx_smeter` | `rx_audio_get_strength_db()` | dB re S9 → dBm with S9 = -73 dBm; uncalibrated, as the S-meter already says. |
| `tx_sensors` | none on a DE board (no bridge) | Send `0.0` values, or only what is known (§12). |
| RX audio | `sound.c` → `decim48k` → `uac_push_audio_rx()`, 512 samples per block at 48 kHz, on the audio thread | Tap the same samples (§7). |
| TX audio | `uac_pull_audio_tx()` → `upsample48k` → `tx_pipeline` in DIGITAL | Add TCI as a second source (§7). |
| I/Q | the antialiased 96 kHz block sent to `hpsdr_send_iq()` and `iq_stream_send()` | 96 kHz direct, 48 kHz via a new complex decimator; 192/384 kHz can't be made from a 96 kHz capture. |

## 6. The control surface

**Modes.** `modulations_list:USB,LSB,CW,CWR,CWU,CWL,DIGU;` - CW and CWR
for Hamlib, CWU and CWL for ExpertSDR-minded clients, all mapping to the
two CW modes; `digu` is DIGITAL (maxibitx's data mode is USB only, so
`digl` is not offered). The mode echoed and pushed is the name the client
used if it set it, else `cw`, `cwr`, `usb`, `lsb`, `digu`. (Which CW names
loggers expect is a test item, §13.)

**PTT and audio source.** `trx:0,true[,source];`:

- In DIGITAL, or with source `tci`, the transmit audio comes from the TCI
  stream of the client that keyed.
- Source `vac` (Hamlib) or none, in DIGITAL: the USB gadget's audio, as
  today - the audio is arriving by some other route.
- In USB/LSB with no source: the mic, as rigctld `T` does.
- In CW: TX with no audio, as every remote PTT does today; CW goes
  through `cw_macros`.

**Several clients.** Last writer wins, and every change is pushed to all
clients - the same behaviour rigctld and CAT have between them today. A
logger and JTDX side by side is the normal case: the logger follows the
frequency JTDX sets.

**State changes made elsewhere** - the panel, rigctld, CAT, the key -
must reach TCI clients. maxibitx has no change notifications, so the TCI
thread samples the state it publishes every 50 ms and pushes what
changed. That is cheap (a dozen integer reads) and keeps TCI out of every
other module.

## 7. Streams

**RX audio.** The audio thread already makes 512 samples at 48 kHz per
block for the gadget. It would also write them to a TCI ring - the same
lock-free single-producer, single-consumer ring `usb_gadget.c` uses,
skipped entirely when no TCI client has `audio_start`ed. The TCI sender
thread reads the ring and, per client, converts to the requested format
(int16/24/32 or float32), rate (48 kHz as is; 24, 12, 8 kHz by a
decimating FIR), channels (mono, or the same sample on both) and frame
size, then queues the frame on that client's socket.

**TX audio.** A second ring, filled by the TCI thread from the keying
client's TX audio frames and read by the audio thread in place of
`uac_pull_audio_tx()` when TCI is the source (§6) - same format, doubles
at 48 kHz, so the upsampler and the rest of the DIGITAL branch are
unchanged. TX_CHRONO frames are sent by the TCI thread, paced by the
audio thread's block count: `length` 2048 (1024 frames, 21.3 ms),
`channels` 2, float32, keeping `tx_stream_audio_buffering` (default
50 ms) requested ahead of what has arrived, at most a fixed number
outstanding. On TX release the ring is emptied, so a later transmission
never starts with stale audio (the gadget path has this problem today:
nothing drains its TX ring outside a DIGITAL transmission).

**I/Q.** The same 96 kHz block the audio thread gives `iq_stream_send()`,
into a third ring, only while a client has `iq_start`ed. Sent as float32
complex at 96 kHz; at 48 kHz after a complex half-band decimator. A
request for 192 or 384 kHz is answered `iq_samplerate:96000;` - the rate
actually in force. Clients that need 192 kHz for full function (the TCI
Remote panadapter asks for at least 96 kHz) are served at 96.

**Bandwidth** per client, computed:

| Stream | Rate |
|---|---|
| I/Q, 96 kHz float32 | 768 kB/s, 6.1 Mbit/s |
| I/Q, 48 kHz float32 | 384 kB/s, 3.1 Mbit/s |
| RX audio, 48 kHz float32 stereo (JTDX's assumption) | 384 kB/s, 3.1 Mbit/s |
| TX audio, 48 kHz float32 stereo (client to radio) | 384 kB/s, 3.1 Mbit/s |
| RX audio, 12 kHz int16 mono | 24 kB/s, 0.2 Mbit/s |

A JTDX session is about 3 Mbit/s each way while transmitting - easy on
Ethernet or a USB network link, and within a Pi Zero 2W's 2.4 GHz Wi-Fi
but not with much to spare once I/Q is added (a prediction, to measure).

## 8. CW over TCI

The keyer already sends text; TCI needs a translator and three small
additions to `keyer.c`.

- **`cw_macros`** → a new `morse_from_tci()` in `morse.c`: `|XX|`
  prosigns, `^ ~ *` back to `: , ;`, everything else as `morse_from_plain()`.
  Then `keyer_send_text()` and `cw_text_queued()`, exactly as rigctld `b`
  does.
- **`<` and `>`, speed changes inside the text** - the keyer has no such
  thing. Proposal: two more internal codes in `morse.h` (speed -5, speed
  +5 WPM), which the keyer applies at the next character start, never
  mid-character - its existing rule for speed changes.
- **`cw_macros_empty`**, sent when the last queued character starts - the
  keyer knows when the queue runs dry but doesn't say. Proposal:
  `keyer_text_queued()`, the characters not yet started; the TCI thread
  watches it fall to zero.
- **`cw_msg`** with callsign correction and `callsign_send`: the TCI
  thread holds the callsign and suffix itself and feeds the keyer one
  character at a time while the keyer's queue is nearly empty, so a
  correction applies to everything not yet started; when the last
  callsign character starts it sends `callsign_send`. A new `cw_msg`
  stops the one in progress, a `cw_macros` during a `cw_msg` queues
  behind it, and a `cw_msg` during a `cw_macros` stops the macro - the
  spec's priority rules.
- **`cw_terminal`**: while true, TX is held after text ends. The
  hang floor mechanism in `cw.c` already holds TX; this is one more
  reason to hold it.
- **`cw_macros_stop`** → `keyer_stop_text()`. A paddle touch stops TCI
  text too, as it stops all text.
- The spec's `keyer:0,true;` - a client keying individual elements - is
  left out: over a network its timing would be the network's, not the
  operator's.

## 9. Structure

```
tci_ws.c      WebSocket server: listen, HTTP upgrade (SHA-1 + base64,
              RFC 6455 §4.2.2), frame read/write (masking, 16/64-bit
              lengths, continuation, ping/pong, close), one thread per
              client, a per-client send queue. Knows nothing of TCI.
tci.c         The protocol: init burst, command parser and echo rules,
              state sampling and broadcast, CW (§8), stream set-up per
              client, TX source arbitration.
tci_stream.c  The three rings the audio thread touches, the sender thread
              that turns them into per-client frames, TX_CHRONO pacing,
              the rate and format converters.
tci.h         What the audio thread calls: tci_push_audio_rx(),
              tci_push_iq(), tci_pull_audio_tx(); and tci_init()/
              tci_stop() for maxibitx.c.
```

**The audio thread's contract is unchanged:** it never blocks, never
takes a lock, never does I/O. Each push is skipped when no client wants
that stream (one atomic read), as `iq_stream_send()` already does.
Everything that touches a socket runs on TCI's own threads, at normal
priority.

**The WebSocket server is written here, not taken from a library** -
maxibitx's rule so far (`gpio.c`, `i2c.c`): the server side of RFC 6455
is small, SHA-1 is about a hundred lines, and the RFC gives a test vector
(`dGhlIHNhbXBsZSBub25jZQ==` → `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`, checked).
libwebsockets is the alternative if the hand-written server proves
fragile.

**Configuration**, top-level keys in `hw_settings.ini` like
`ext_ptt_delay_ms`: `tci_port` (default 50001, Hamlib's and ExpertSDR3's
default; 0 turns TCI off) and `tci_max_clients` (default 4).

**Security.** TCI has no authentication, and a TCI client can transmit.
maxibitx's rigctld and HPSDR already listen on every interface with none
either, so this adds no new kind of exposure, but it is worth saying in
the docs and worth a `tci_bind` setting (default all interfaces) for a
station on a shared network.

## 10. Coexistence

- **HPSDR, `iq_stream.c`, the gadget, rigctld and CAT are unchanged** and
  run alongside. All the stream taps are independent readers of the same
  per-block data.
- **One transmitter, several PTT sources.** TCI's `trx` joins rigctld `T`,
  CAT `TX` and HPSDR MOX: all set the same `in_tx` through
  `radio_set_tx()`, all defer to the local key, all are refused outside
  the band table. TCI adds the one new rule: the TX audio source (§6).
- **The TX test tone** keeps priority over every source, as in
  `sound.c` now.

## 11. Getting TCI over the USB cable

TCI needs a network. Over Ethernet or Wi-Fi it works as it is. For a
station using the one USB-C cable today, the gadget could add a network
function - NCM - beside UAC2 and ACM, giving the PC a point-to-point
Ethernet link to the Pi. That is new gadget work (`usb_gadget.c` has no
network function) and needs an address on each end - a fixed pair, or a
small DHCP server on the Pi. Windows 10 (from version 2004) and 11 are
understood to include an NCM class driver (to be checked, §13). This is
optional and separate: TCI is useful without it.

## 12. Risks and unknowns

- **Client quirks beyond those found.** §4 covers the clients whose
  source is public. Log4OM, MSHV and the Mac loggers are closed; they
  are test items, not reading items.
- **Echo precision.** JTDX compares echoes as strings. A `vfo` set to a
  frequency maxibitx adjusts (none today; `radio_tune_to()` takes Hz as
  given) would fail its echo. Any future rounding must keep this in mind.
- **Volume.** TCI's `volume` is dB, -60 to 0; maxibitx's is 0-100 over
  a digital gain whose top is `RX_VOLUME_MAX`. A mapping is needed that
  round-trips exactly, since clients may compare echoes.
- **TX sensors on a DE board.** No power or SWR measurement exists.
  Sending `0.0` avoids JTDX's crash but reads as "no output"; not sending
  `tx_sensors` at all may be better, since JTDX only asks for them after
  `tx_sensors_enable`. To decide in testing.
- **Pi Zero 2W.** CPU for per-client conversion is small (a prediction:
  a few percent of one core per stream); Wi-Fi bandwidth with I/Q is the
  real question. Part of the Zero 2W measurements still to do.
- **Latency for digital modes.** TX audio passes a 50 ms request-ahead
  buffer plus the network; FT8 tolerates far more, but it adds to what
  WSJT-X's own timing already absorbs.

## 13. Suggested order of work and testing

Each step is usable on its own.

1. **`tci_ws.c` and a `test-tci-ws` harness**: handshake against the RFC
   vector, masked and unmasked frames, 7/16/64-bit lengths,
   fragmentation, ping/pong, close, oversize and malformed frames,
   several clients - all over loopback, no radio.
2. **`tci.c` control**: init burst, the command table of §5-§6, echo and
   broadcast, state sampling. `test-tci` drives it with scripted
   sessions copied from §4 (a JTDX startup, a Hamlib startup) and checks
   every reply against the rules. On the radio: Hamlib's `rigctl` with
   its TCI model, then a logger.
3. **RX and TX audio**: the two rings, the converters, TX_CHRONO pacing,
   TX source arbitration. On the radio: JTDX and WSJT-X Improved
   decoding and making an FT8 contact, as the gadget was proved.
4. **CW**: `morse_from_tci()`, the keyer additions, `cw_msg`. Bench: the
   same key-stream checks as `test-keyer` part 4. On the radio: a
   contest logger's macros.
5. **I/Q**: 96 and 48 kHz. On the radio: any TCI panadapter client (TCI
   Remote), and the Zero 2W bandwidth measurement.
6. **Optional: NCM on the gadget** (§11).

A small Python client in `tools/` (standard library only) would make
steps 2-5 checkable from a laptop, as `rigctl_panel.py` does for rigctld.

## 14. Decisions wanted before building

1. **Port**: 50001 (Hamlib, ExpertSDR3) - or 40001 (JTDX's fallback)?
   Proposed 50001; both clients let the user set it.
2. **Advertised identity**: `protocol:ExpertSDR3,2.0` with
   `device:maxibitx`, for JTDX compatibility (§4) - agreed?
3. **CW mode names** in `modulations_list`: all of CW, CWR, CWU, CWL
   (proposed), or a smaller set?
4. **Scope of the first delivery**: steps 1-3 (control and audio - what
   replaces the gadget for JTDX/WSJT-X Improved), with CW and I/Q after?
5. **NCM over USB**: in scope, or TCI over the network only for now?
6. **TX sensors on boards without a bridge**: send zeros, or nothing?

## Sources

- [ExpertSDR3/TCI - *TCI Protocol* v2.0 specification](https://github.com/ExpertSDR3/TCI)
- [Expert Electronics - software with TCI support](https://eesdr.com/en/software-en/software-en)
- [Hamlib](https://github.com/Hamlib/Hamlib) (`rigs/tci/tci2.c`)
- [JTDX](https://github.com/jtdx-project/jtdx) (`TCITransceiver.cpp`)
- [WSJT-X Improved](https://sourceforge.net/projects/wsjt-x-improved/files/)
- [ramdor/Thetis](https://github.com/ramdor/Thetis) (`TCIServer.cs`)
- [aethersdr/AetherSDR](https://github.com/aethersdr/AetherSDR) (`TciProtocol.cpp`, `TciServer.cpp`), and its [issue #1439](https://github.com/ten9876/AetherSDR/issues/1439)
- [OK1BR/sdr-for-linux](https://github.com/OK1BR/sdr-for-linux) (`tci_server.c`)
- [TCI Protocol Reference - TCI Remote compatibility guide](https://pure-editions.com/on7off/TCI-Remote/TCI_Protocol_Reference.html)
- [hamlib-tci-sidecar](https://github.com/jfrancis42/hamlib-tci-sidecar)
- RFC 6455, *The WebSocket Protocol*
