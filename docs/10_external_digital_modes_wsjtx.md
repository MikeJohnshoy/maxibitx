# 10 — using maxibitx with external digital comm applications (WSJT-X, ...)

Status: code-complete (`ARCHITECTURE.md` §10 step 9) — on-air test with a
real WSJT-X session still outstanding. This doc previously described the
inherited, minibitx-era behavior (raw I/Q for an external SDR-console app
to demodulate, no TX audio path at all); both are gone now, replaced by
the design below.

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

- **RX (device → host):** the receiver's own on-target demodulation
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

1. Point WSJT-X's **Radio** at Hamlib's `NET rigctl` rig, host
   `127.0.0.1`, port `4532` (rigctld, `hamlib.c`) — unchanged from
   before this step, and independent of the audio bridge below. This is
   what lets WSJT-X set frequency and key PTT.
2. Set the radio's mode to `DIGITAL` (`tools/rigctl_panel.py`'s mode
   selector, or an `MD9`/`m DIGITAL` command directly) before
   transmitting — `sound.c`'s TX branch only pulls from this gadget's
   inbound audio queue when `radio_get_mode()` reads
   `RADIO_MODE_DIGITAL`; in any other mode, whatever WSJT-X sends over
   USB is simply never drained into the exciter.
3. Point WSJT-X's **Audio** input and output devices at the USB sound
   card this gadget presents ("sBitx Audio") — not at any onboard mic/
   speaker device, and not at a separate SDR-console app's virtual audio
   cable. This is the actual bridge: RX audio decodes from it, TX audio
   is generated into it.
4. PTT: WSJT-X's own CAT PTT (via rigctld's `T`, or this gadget's own
   Kenwood `TX`/`RX`/`TQ` CAT commands if driving FLrig instead) already
   works in `DIGITAL` mode with no changes from this step — it was
   already mode-independent before this work started
   (`ARCHITECTURE.md` §10 step 9's note on `cw_tx_active()`). There is no
   physical key/mic-PTT path for `DIGITAL` — `cw.c`'s `cw_poll_key()`
   deliberately ignores that GPIO line in this mode, since there's
   nothing for a physical switch to mean here.

## What's still genuinely open

Nothing here has been checked against a real, running WSJT-X session
yet — only bench-verified (`upsample48k_test.c`'s tone-reconstruction/
imaging-rejection numbers, and a full regression pass of every other DSP
bench harness) and confirmed to build and run without crashing. Still
outstanding, same "measure before trusting" discipline as everything else
in this codebase:

- Whether `usb_gadget.c`'s `UAC_RX_AUDIO_SCALE` compile-time constant
  (mapping `rx_audio.c`'s post-AGC amplitude range onto 16-bit PCM) lands
  WSJT-X's decoder in a comfortable input range on a real host, or needs
  retuning — the same kind of first-pass guess `MIC_TX_INPUT_SCALE` was
  for step 8's voice path before a real mic proved it needed a live,
  adjustable multiplier on top.
- Whether WSJT-X's own generated tone actually decodes correctly on the
  air after the full round trip (`uac_reader_thread()` →
  `upsample48k_apply()` → `tx_pipeline.c` → the exciter) — bench numbers
  confirm the DSP math is sound (unity gain, deep imaging rejection), not
  that a real FT8 transmission is intelligible to another station.
- Real wattmeter/ALC calibration for `DIGITAL`'s own TX power — a
  separate, still-open item tracked in `ARCHITECTURE.md` §9, unaffected
  by whether the audio itself arrives correctly.
