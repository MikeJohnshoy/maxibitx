# 10 — using maxibitx with external digital comm applications (WSJT-X, ...)

Status: **fully proven on the air — a two-way FT8 contact on
2026-09-23.** Windows 11 + WSJT-X decodes FT8 from this gadget's audio,
with performance on par with a raw-I/Q SDR app (2026-09-21); frequency
control works end to end either through FLrig acting as a relay or with
WSJT-X's own Hamlib client talking directly to this gadget's serial
Kenwood CAT (see "Setting it up" below); and WSJT-X's own generated TX
audio now has another operator's decoder to vouch for it. That last
item was the one thing standing between `DIGITAL` and "fully proven".
What remains open is smaller, and listed below.

This doc previously described the inherited, minibitx-era behavior (raw
I/Q for an external SDR-console app to demodulate, no TX audio path at
all); both are gone now, replaced by the design below.

## What actually happens now

`usb_gadget.c`'s USB Audio Class 2.0 gadget carries real, already-
demodulated 16-bit/48kHz PCM audio, in both directions, over the same USB
link that also carries Kenwood CAT (`04_remote_control_and_iq_output.md`).
There is no I/Q anywhere in this gadget any more, and no separate SDR-
console app (SDR#, HDSDR, GQRX, SDR Console, ...) needed in between — the
device enumerates on the host as an ordinary sound card ("sBitx Audio")
that WSJT-X can select directly as both its receiving and transmitting
audio device, the same way it would talk to a real radio's USB audio
codec or an external SignaLink-style interface.

- **RX (device → host):** in `DIGITAL` (and `USB`), `rx_audio.c`
  demodulates as real upper sideband with no pitch offset, so a station
  `f` Hz above dial arrives at exactly `f` Hz of audio - the relationship
  WSJT-X assumes. It didn't until 2026-09-21: the demod was CW-only in
  every mode, and `DIGITAL` was actually receiving the lower sideband,
  700Hz high (`dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md`
  §10). Keep the narrow CW filter off in `DIGITAL` - it's still centered
  on 700Hz of audio. The receiver's own on-target demodulation
  (`rx_audio.c`) is tapped one stage earlier than the local speaker's own
  audio — after the AGC's makeup gain, but before the operator's own
  `rx_volume`/AF-gain knob — so WSJT-X's decoded level doesn't silently
  drift every time the operator adjusts what they're listening to
  locally. Decimated from maxibitx's native 96kHz down to the 48kHz this
  gadget advertises (`decim48k.c`, unchanged from before this step).
- **TX (host → device):** WSJT-X's own generated audio tone (whatever it
  synthesizes internally for FT8/FT4/etc.) arrives over this gadget's
  capture-side PCM at 48kHz, gets upsampled to maxibitx's native 96kHz
  (`upsample48k.c` — the interpolating counterpart `decim48k.c` never
  needed until now), and rides into the exact same shared FFT TX pipeline
  CW and USB/LSB already use (`tx_pipeline.c`, `ARCHITECTURE.md` §10 step
  8) — always as upper sideband, matching the universal FT8/digital-mode
  convention regardless of which HF band is in use.

## Setting it up

**1. Rig control: two confirmed-working options, one rule that applies
to both.** WSJT-X can reach this gadget's rig control either through
FLrig acting as a relay, or by talking directly to the serial Kenwood
CAT itself (the CDC-ACM function, `04_remote_control_and_iq_output.md`)
with FLrig entirely out of the picture — both are on-air confirmed. The
rule that matters either way: **only one process may ever have the
serial CAT port open at a time.** Windows treats a COM port as
exclusive-access, and it doesn't fail loudly when two processes both try
— it just looks like both are broken (see below).

- **Direct, no FLrig:** in WSJT-X, `Settings` → `Radio` → set **Rig** to
  a Kenwood TS-480 (or equivalent Hamlib serial rig type), point its
  Serial Port at this gadget's COM port, 4800 baud. Make sure FLrig
  either isn't running or has released the port first — with two Hamlib
  clients now confirmed to drive this surface correctly (FLrig, and
  WSJT-X itself), the only remaining failure mode here is both trying to
  hold the port at once.
- **Via FLrig:** in FLrig, `Config` → `Setup` → the XML-RPC server tab —
  confirm the server is enabled, and note its address/port
  (`127.0.0.1:12345` by default). In WSJT-X, set **Rig** to `Flrig`
  instead (a distinct entry in the dropdown, separate from and easy to
  miss among the Kenwood/Hamlib serial entries) and point its Network
  Server address/port at FLrig's. Leave WSJT-X's own Serial Port field
  alone in this mode — it never opens the COM port itself, so FLrig
  keeps sole ownership. Worth choosing this over direct CAT specifically
  when you want FLrig's own panel running at the same time as WSJT-X
  (logging, a waterfall, manual tuning) — direct CAT means WSJT-X alone
  owns the port and FLrig can't be open simultaneously.

The first version of this setup had exactly the conflict the rule above
warns about — WSJT-X's own Hamlib rig type pointed at the same COM port
FLrig already had open — and the symptom was identical on both sides:
WSJT-X's rig-control Settings refused to save, and FLrig's own frequency
changes never reached WSJT-X. Both were the same root cause: WSJT-X was
never actually connected to anything, because the port was already
taken.

This gadget's own rigctld surface (`hamlib.c`, TCP port 4532) is a
*third*, independent way WSJT-X could in principle reach rig control —
Hamlib's `NET rigctl` rig type, host `127.0.0.1`, port `4532` — but with
direct serial CAT now confirmed working, there's been no reason to try
it, and it remains untested. If you ever do, the same one-owner rule
applies: don't leave FLrig's own serial CAT connection open on the same
COM port at the same time, since rigctld and FLrig would then be two
independent processes both wanting the one physical serial port.

**2. Set the radio's mode to `DIGITAL`** (`tools/rigctl_panel.py`'s mode
selector, or `MD2`/`m DIGITAL` over either CAT surface) before
transmitting — `sound.c`'s TX branch only pulls from this gadget's
inbound audio queue when `radio_get_mode()` reads `RADIO_MODE_DIGITAL`;
in any other mode, whatever WSJT-X sends over USB is simply never
drained into the exciter. Note the digit is `2`, not `9` — `DIGITAL`
reports itself over CAT as Kenwood mode `2` (USB), not a dedicated data
digit; a real TS-480 has no data-mode digit to borrow, and FT8 *is*
upper sideband regardless (see `ARCHITECTURE.md` §10 step 12 for why,
and for the guard that keeps a host reading `MD2` back and writing it
out again from silently walking the radio out of `DIGITAL`).

**3. Point WSJT-X's Audio input and output at this gadget's own audio
device** — not at any onboard mic/speaker, and not at a separate
SDR-console app's virtual audio cable. Two things worth knowing here,
both learned the hard way on a real Windows 11 box:

- **The device does not show up named "sBitx Audio."** Linux's `f_uac2`
  gadget driver has no way to set a per-endpoint name string, so Windows
  falls back to a generic topology label — look for **"Microphone
  (Source/Sink)"** (RX, device→host) and **"Speakers (Source/Sink)"**
  (TX, host→device) in WSJT-X's device lists. Only the top-level
  composite USB device in Device Manager actually reports "sBitx Audio";
  neither audio sub-endpoint does.
- **The device advertises exactly one format — 48000Hz, 16-bit, mono —
  and no alternates**, unlike most commercial USB audio interfaces. If
  WSJT-X (or Audacity, while checking) refuses to open "Microphone
  (Source/Sink)" specifically while Windows' own legacy "Sound Mapper"
  device works fine, that's this: the Sound Mapper path silently
  converts formats and a direct-by-name open doesn't. Set the app's own
  channel count/sample rate to match (WSJT-X: the small Mono/Left/Right/
  Both selector next to its Input device) rather than assuming the
  device will adapt. If it still refuses after that, and this is a
  device that has been plugged in since before a firmware update that
  changed its channel count, see `ARCHITECTURE.md` §10 step 11 — Windows
  can cache a stale format against this gadget's fixed USB serial
  number, and only a serial-number bump (forcing Windows to treat it as
  a new device) clears that, not a replug.

**4. PTT:** WSJT-X's own CAT PTT already works in `DIGITAL` mode with no
special handling — it was already mode-independent before the audio
bridge was built (`ARCHITECTURE.md` §10 step 9's note on
`cw_tx_active()`). Whichever CAT path drives PTT (FLrig's relay, or this
gadget's own `TX`/`RX`/`TQ` commands directly) works the same way. There
is no physical key/mic-PTT path for `DIGITAL` — `cw.c`'s
`cw_poll_key()` deliberately ignores that GPIO line in this mode, since
there's nothing for a physical switch to mean here.

## What's still genuinely open

- **`hamlib.c`'s rigctld surface (TCP 4532)** as WSJT-X's `NET rigctl`
  rig type — untested, with or without FLrig in the picture, since
  direct serial CAT already covers the need. Low priority unless a
  reason to prefer it over direct CAT/FLrig comes up.
- **Split operation** — WSJT-X's rig-control layer can reach split via
  Hamlib's `FR`/`FT`/`SP`, none of which either of this gadget's CAT
  surfaces implement yet. Leave split off in WSJT-X until that gap is
  closed.
- Whether `usb_gadget.c`'s `UAC_RX_AUDIO_SCALE` compile-time constant
  lands WSJT-X's decoder in a comfortable input range, or needs
  retuning — decodes and a completed contact say it is good enough, but
  it has never been independently measured.
- **How far off frequency the transmission actually is.** The contact
  doesn't settle this: FT8 decoders search a wide window and report the
  offset they find, so one tolerates an error far larger than the
  6.25 Hz of bin quantization. The placement figure comes from the
  test-tone measurements in
  `../03_tx_processing_pipeline.md`, not from the QSO.
- **Transmit level from the host.** There is no radio-side drive
  control in `DIGITAL` — `mic_tx_gain` touches only the mic branch — so
  WSJT-X's own output level sets it, and should be set by watching
  rigctld's `l ALC` reading.
- Real wattmeter/ALC calibration for `DIGITAL`'s own TX power — a
  separate, still-open item tracked in `ARCHITECTURE.md` §9, unaffected
  by whether the audio itself arrives correctly.
