/*
 * usb_gadget.h — USB gadget composite device for minibitx: Audio Class 2.0
 * (UAC2) bidirectional real-audio streaming plus a CDC-ACM serial function
 * for Kenwood TS-480-subset CAT control. Both live in usb_gadget.c - the
 * CAT command handling is in its own clearly-marked section near the
 * bottom of that file, rather than a separate translation unit, since both
 * functions are just two halves of the one composite gadget this file
 * already owns the whole lifecycle of.
 *
 * Originally carried raw baseband I/Q (24-bit/48kHz stereo, I=left,
 * Q=right) for an external SDR application (SDR#, HDSDR, GQRX, SDR
 * Console) to demodulate in software - see git history for that version if
 * this ever needs reviving. Converted to real, already-demodulated 16-bit/
 * 48kHz PCM audio instead (docs/ARCHITECTURE.md's WSJT-X TX audio bridge):
 * a control-surface app like WSJT-X wants a normal sound-card-style audio
 * device to decode/encode from, not raw I/Q it has no built-in way to
 * demodulate itself - real demodulation now happens on-target
 * (rx_audio.c), the same signal the local speaker already gets, just
 * tapped before rx_volume (see rx_audio.h's rx_audio_process() uac_out
 * parameter) so a remote decoder's level doesn't silently change every
 * time the operator touches their own listening volume. Genuinely
 * bidirectional now, unlike the I/Q version (whose playback/host-to-device
 * direction was declared in the USB descriptor but never used): WSJT-X's
 * own generated tone audio arrives over this same gadget's capture-side
 * PCM and rides into the shared TX pipeline for RADIO_MODE_DIGITAL (see
 * sound.c).
 *
 * Architecture overview:
 *   The Linux USB gadget framework is configured via the configfs API under
 *   /sys/kernel/config/usb_gadget/. Two functions are bound into one
 *   composite gadget, both created/torn down together by
 *   uac_gadget_create()/uac_gadget_destroy() in usb_gadget.c - see the path
 *   layout below for the exact attributes each carries. Device/product
 *   strings: "sBitx" / "sBitx Audio". Once bound to a UDC controller
 *   (detected automatically), the host sees a standard, full-duplex USB
 *   audio device called "sBitx" AND a standard USB serial port - Windows
 *   10/11 auto-binds its inbox usbser.sys driver to the ACM interface with
 *   no custom INF needed, the same "just works" story UAC2 already gets
 *   (see docs/dsp_design_notes/usb_gadget_OS_setup.md for current
 *   bench-testing status - written against the I/Q version; the gadget
 *   bring-up steps themselves are unchanged, only what's actually inside
 *   the audio stream is different now).
 *
 *   Sample delivery, RX (device -> host, decoded audio for WSJT-X to
 *   decode): sound.c runs the codec at 96kHz, but this gadget is
 *   configured for 48kHz - sound_process() runs the pre-volume audio
 *   through decim48k_apply() (decim48k.c), a real 96kHz->48kHz decimating
 *   lowpass, before calling uac_push_audio_rx(), so the 48kHz this gadget
 *   advertises is what it actually delivers (see
 *   docs/dsp_design_notes/usb_uac_decimation_design.md for the filter
 *   design). Sample delivery, TX (host -> device, WSJT-X's own generated
 *   audio for RADIO_MODE_DIGITAL): the reverse case decim48k.c never had
 *   to handle - host audio arrives at 48kHz but tx_pipeline.c needs 96kHz,
 *   so sound.c runs uac_pull_audio_tx()'s samples through
 *   upsample48k_apply() (upsample48k.c, decim48k.c's own interpolation
 *   counterpart) before handing them to the shared TX pipeline.
 *
 *   Producer/consumer split, both directions: uac_push_audio_rx() only
 *   ever enqueues into a lock-free ring buffer and never blocks - the same
 *   architecture hpsdr_p1.c's I/Q queue uses, for the same reason (a
 *   stalled/absent USB host must never be able to block sound.c's
 *   real-time audio thread and cause hardware xruns). A dedicated
 *   uac_writer_thread() is the sole consumer, draining that queue and
 *   performing the potentially-blocking ALSA write on its own thread - see
 *   docs/dsp_design_notes/usb_gadget_OS_setup.md §7 for the xrun incident
 *   this fixed. The TX direction mirrors this exactly in reverse:
 *   uac_reader_thread() is the only thing that ever calls the
 *   (potentially-blocking) snd_pcm_readi() on the capture-side PCM, and
 *   only ever enqueues what it reads into a second lock-free ring buffer;
 *   uac_pull_audio_tx() (called from sound.c's real-time thread) only
 *   ever dequeues from it and never blocks, returning fewer samples than
 *   asked for (silence-filled by the caller) rather than waiting on a
 *   slow/absent host.
 *
 *   ALSA delivery — direct to the gadget's own card: binding the UAC2
 *   function to a UDC makes the kernel's u_audio/f_uac2 driver register
 *   its own independent ALSA card ("UAC2Gadget" in /proc/asound/cards"),
 *   with both a playback and a capture PCM substream on its device 0 (one
 *   card, one device, two independent stream directions - not two
 *   separate devices). uac_writer_thread() opens the playback substream
 *   and writes to it (what streams out to the host as ITS capture/
 *   microphone input); uac_reader_thread() opens the capture substream
 *   and reads from it (what the host sends as ITS playback/speaker
 *   output). (An earlier version routed the RX direction through a
 *   separate snd-aloop card instead, on the mistaken assumption that UAC2
 *   reads its capture side from that loopback automatically - it doesn't,
 *   so nothing reached the USB link; see
 *   docs/dsp_design_notes/usb_gadget_OS_setup.md §11 if that approach is
 *   ever tempting again.)
 *
 * Configfs gadget path layout (created by uac_gadget_create() in
 * usb_gadget.c):
 *   /sys/kernel/config/usb_gadget/sbitx_iq/      (configfs root name kept
 *     idVendor, idProduct, bcdUSB, bcdDevice       unchanged from the I/Q
 *     strings/0x409/manufacturer  = "sBitx"        version deliberately -
 *     strings/0x409/product       = "sBitx Audio"  it's an internal path,
 *     strings/0x409/serialnumber  = "0000001"       not host-visible, and
 *     configs/c.1/                                  renaming it risks
 *       strings/0x409/configuration = "Default"    breaking any existing
 *       bmAttributes, MaxPower                      OS-level setup script
 *       function symlinks -> functions/uac2.0/, functions/acm.usb0/
 *     functions/uac2.0/
 *       c_srate  = 48000
 *       c_ssize  = 2        (2 bytes = 16-bit PCM, was 3/24-bit for I/Q)
 *       c_chmask = 3        (2 channels - both carry the SAME mono audio,
 *                             see uac_writer_thread(), for host/app
 *                             compatibility with devices that assume a
 *                             stereo audio interface)
 *       p_srate  = 48000    (playback side - genuinely used now, unlike
 *       p_ssize  = 2         the I/Q version's declared-but-unused pair)
 *       p_chmask = 3
 *     functions/acm.usb0/
 *       (no attributes set — port_num is read-only/assigned by the kernel)
 *
 * Dependencies (must be present on the target system):
 *   Kernel modules : dwc2 (or other device-mode UDC), libcomposite
 *   Userspace libs : libasound2-dev (ALSA — for PCM read/write to the
 *                    UAC2Gadget card; minibitx already links -lasound
 *                    for sound.c)
 *   Kernel config  : CONFIG_USB_CONFIGFS_F_UAC2=y, CONFIG_USB_CONFIGFS_F_ACM=y
 *
 * Thread safety:
 *   uac_init()/uac_stop() are meant to be called once, from main(), around
 *   hpsdr_init()/hpsdr_poll(). uac_push_audio_rx() runs on minibitx's audio
 *   thread and only ever touches its own lock-free queue - it never
 *   blocks and never touches an ALSA PCM handle directly.
 *   uac_writer_thread() (internal to usb_gadget.c) is the only thing that
 *   ever calls into ALSA on the playback handle. uac_pull_audio_tx() also
 *   runs on minibitx's audio thread, only ever touching the OTHER lock-free
 *   queue, never blocking; uac_reader_thread() (internal) is the only
 *   thing that ever calls into ALSA on the capture handle. No lock is
 *   needed anywhere in this: each queue's atomic head/tail split ownership
 *   exactly like hpsdr_p1.c's I/Q queue (producer writes head only,
 *   consumer writes tail only) - just two independent queues now, one per
 *   direction, instead of the one the I/Q version needed.
 */

#ifndef USB_GADGET_H
#define USB_GADGET_H

/* Configure the composite UAC2+ACM gadget via configfs and open both
 * directions of the UAC2 side's ALSA PCM. Call once, after
 * hpsdr_init()/hpsdr_poll(). Returns 0 if the gadget and at least the
 * playback (RX/outbound) PCM are ready, -1 if gadget creation or the
 * playback PCM fails (e.g. no UDC found) — not a hard failure for the rest
 * of minibitx, which keeps running over HPSDR/UDP either way. A capture
 * (TX/inbound) PCM open failure is logged but does not fail this call -
 * WSJT-X TX audio just won't arrive until that's resolved, independent of
 * whether decoded RX audio is flowing. The ACM function comes up as part
 * of the same gadget bind; cat_init() (below) is what actually opens and
 * uses the resulting /dev/ttyGS0, independently of this succeeding or
 * failing. */
int uac_init(void);

/* Deliver one 48kHz real-audio sample (decoded RX audio, e.g. rx_audio.c's
 * post-AGC/pre-rx_volume tap, decimated to 48kHz by decim48k_apply() - see
 * sound.c). Unnormalized: this file's own UAC_RX_AUDIO_SCALE (usb_gadget.c)
 * is the one compile-time constant that maps the DSP's natural amplitude
 * range onto 16-bit PCM's +-32767 - retune that, not the caller, if the
 * level WSJT-X sees needs adjusting. Enqueues into a lock-free ring buffer
 * for uac_writer_thread() to drain and flush to the gadget's PCM one
 * period at a time - never blocks; silently drops the sample if the queue
 * is full (writer thread stalled behind a slow/absent USB host). No-op if
 * uac_init() hasn't succeeded (or after uac_stop()). */
void uac_push_audio_rx(double sample);

/* Pulls up to n 48kHz real-audio samples the host has sent (WSJT-X's own
 * TX tone, captured by uac_reader_thread() from the gadget's capture-side
 * PCM) into out[], normalized to roughly [-1, +1] (uac_reader_thread()'s
 * own fixed, unambiguous 16-bit-PCM-to-normalized-double conversion - no
 * separate compile-time scale needed here the way UAC_RX_AUDIO_SCALE is,
 * since incoming PCM's full-scale meaning isn't in question the way a raw
 * analog mic's achieved amplitude was for MIC_TX_INPUT_SCALE). Returns the
 * number of samples actually available (0 to n) - never blocks; the
 * caller (sound.c) is responsible for filling any shortfall with silence
 * rather than stale/uninitialized data. 0 if uac_init() hasn't succeeded,
 * the capture PCM never opened, or after uac_stop(). */
int uac_pull_audio_tx(double *out, int n);

/* Tear down the gadget and release both ALSA PCM handles. Safe to call
 * even if uac_init() was never called or failed. */
void uac_stop(void);

/* Returns 1 while the UAC2 stream is initialized and ready to accept RX
 * samples (uac_push_audio_rx()), 0 otherwise. Does not by itself say
 * whether the TX/capture direction is also up - a host can be draining RX
 * audio fine while no app on it has yet started sending TX audio, or vice
 * versa; uac_pull_audio_tx()'s own return count is the one to watch for
 * that direction's actual data flow. */
int uac_is_active(void);

/* Kenwood TS-480-subset CAT control, reached over this gadget's CDC-ACM
 * (ttyGS) serial function - see the "CAT control" section at the bottom
 * of usb_gadget.c for the command set and implementation, and
 * docs/04_remote_control_and_iq_output.md for why TS-480 specifically.
 *
 * This exists for control surfaces that only know how to talk CAT to a
 * real (or real-enough) Kenwood radio - FLRig chief among them, since it
 * has no generic "connect to a Hamlib rigctld-style server" option the
 * way WSJT-X does. WSJT-X keeps using the existing Hamlib NET rigctl
 * server (hamlib.c) unchanged; this is a second, independent control
 * surface, not a replacement.
 *
 * Like uac_init(), this is best-effort: cat_init() failing (most commonly
 * because the ACM gadget function isn't bound - no USB gadget support on
 * this hardware/kernel, or the gadget failed to bind at all, see
 * dsp_design_notes/usb_gadget_OS_setup.md) is not fatal. minibitx keeps running on
 * whatever subset of control surfaces actually came up. Call after
 * uac_init() (the two functions bind together as one gadget, but
 * cat_init()/cat_stop() have no other dependency on uac_init() having
 * succeeded). */
int cat_init(void);
void cat_stop(void);

#endif /* USB_GADGET_H */
