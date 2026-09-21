/*
 * usb_gadget.h - maxibitx's USB gadget: one composite device with two
 * functions, both implemented in usb_gadget.c.
 *
 *   UAC2 audio, 16-bit/48kHz mono, both directions. Device -> host is the
 *     demodulated RX audio (rx_audio.c's uac_out tap, before rx_volume,
 *     decimated 96 -> 48kHz by decim48k.c in sound.c). Host -> device is
 *     WSJT-X's TX audio, upsampled to 96kHz (upsample48k.c) and
 *     transmitted through tx_pipeline.c in RADIO_MODE_DIGITAL.
 *   CDC-ACM serial: Kenwood TS-480-subset CAT on /dev/ttyGS0, for FLRig,
 *     WSJT-X and anything else that speaks Kenwood CAT.
 *
 * The host sees a USB audio device ("sBitx Audio") plus a USB serial
 * port; Windows 10/11 binds both with inbox drivers. Bring-up and bench
 * history: docs/dsp_design_notes/usb_gadget_OS_setup.md. (The gadget
 * originally carried raw I/Q - ARCHITECTURE.md §10 steps 9 and 11.)
 *
 * Threading: nothing called from sound.c's real-time audio thread ever
 * blocks. uac_push_audio_rx() and uac_pull_audio_tx() only touch
 * lock-free single-producer/single-consumer ring buffers (the same scheme
 * as hpsdr_p1.c's I/Q queue). uac_writer_thread() is the only writer of
 * the gadget's playback PCM and uac_reader_thread() the only reader of its
 * capture PCM, so a slow or absent USB host can't cause xruns on the
 * radio's own audio (usb_gadget_OS_setup.md §7).
 *
 * ALSA: binding the UAC2 function makes the kernel register its own card,
 * id "UAC2Gadget", with a playback and a capture substream on device 0.
 * Writing its playback substream feeds the host's microphone input;
 * reading its capture substream receives the host's speaker output. (Not
 * snd-aloop - usb_gadget_OS_setup.md §11.)
 *
 * Configfs layout (uac_gadget_create()):
 *   /sys/kernel/config/usb_gadget/sbitx_iq/    ("sbitx_iq" kept from the
 *     idVendor, idProduct, bcdUSB, bcdDevice     I/Q version: internal,
 *     strings/0x409/manufacturer = "sBitx"       and OS setup scripts may
 *     strings/0x409/product      = "sBitx Audio" reference it)
 *     strings/0x409/serialnumber = "0000002"
 *     configs/c.1/  (configuration "Default", bmAttributes, MaxPower,
 *                    symlinks to both functions)
 *     functions/uac2.0/
 *       c_srate = 48000, c_ssize = 2 (16-bit), c_chmask = 1 (mono)
 *       p_srate = 48000, p_ssize = 2,          p_chmask = 1
 *     functions/acm.usb0/  (no attributes - port_num is kernel-assigned)
 *
 * Needs: a device-mode UDC (e.g. dwc2) and libcomposite;
 * CONFIG_USB_CONFIGFS_F_UAC2=y and CONFIG_USB_CONFIGFS_F_ACM=y;
 * libasound2-dev.
 */

#ifndef USB_GADGET_H
#define USB_GADGET_H

/* Creates the composite gadget via configfs and opens both directions of
 * the UAC2 PCM. Call once, after hpsdr_init(). Returns 0 if the gadget and
 * the playback (RX) PCM are up, -1 otherwise (e.g. no UDC) - not fatal;
 * maxibitx runs on without it. A capture (TX) PCM failure is only logged:
 * RX audio still flows, WSJT-X's TX audio just won't arrive. The ACM
 * function binds with the gadget; cat_init() opens it independently. */
int uac_init(void);

/* Queues one 48kHz RX audio sample (post-AGC, pre-volume, unnormalized -
 * usb_gadget.c's UAC_RX_AUDIO_SCALE maps it onto 16-bit PCM; retune that,
 * not the caller, to change the level WSJT-X sees). Never blocks; drops the
 * sample if the queue is full. No-op when not initialized. */
void uac_push_audio_rx(double sample);

/* Copies up to n queued 48kHz TX samples from the host (WSJT-X) into
 * out[], normalized to about [-1, +1]. Returns how many were available
 * (0..n); never blocks - the caller fills any shortfall with silence.
 * Returns 0 when the capture side isn't up. */
int uac_pull_audio_tx(double *out, int n);

/* Tear down the gadget and release both ALSA PCM handles. Safe to call
 * even if uac_init() was never called or failed. */
void uac_stop(void);

/* 1 while the RX (playback) stream is up. Says nothing about the TX
 * direction - uac_pull_audio_tx()'s return count shows that. */
int uac_is_active(void);

/* Kenwood TS-480-subset CAT over the gadget's CDC-ACM port (/dev/ttyGS0) -
 * see the CAT section of usb_gadget.c and
 * docs/04_remote_control_and_iq_output.md. Used by FLRig, and by WSJT-X
 * either directly or through FLRig's relay; only one process can hold the
 * port at a time. Independent of hamlib.c's rigctld server. Best-effort:
 * failure (e.g. no gadget) isn't fatal. Call after uac_init(); there's no
 * other dependency on it. */
int cat_init(void);
void cat_stop(void);

#endif /* USB_GADGET_H */
