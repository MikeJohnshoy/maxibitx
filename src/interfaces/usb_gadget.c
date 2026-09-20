// usb_gadget.c
//
// See usb_gadget.h for the full design (bidirectional 16-bit/48kHz real
// audio over the UAC2 function, Kenwood CAT over the ACM function). This
// file used to carry raw I/Q for an external SDR application to
// demodulate; that entire path (the paired I/Q ring buffer, the 24-bit
// packing math, uac_push_iq()) is gone - see git history if it's ever
// needed again.

#include "usb_gadget.h"
#include "cw.h"
#include "radio.h"    // freq_hdr, in_tx, radio_tune_to()/radio_set_tx(),
                       // RIT_MAX_HZ, radio_get_rit()/radio_set_rit()/
                       // radio_rit_enabled()/radio_set_rit_enabled() -
                       // previously hand-declared below one at a time;
                       // now pulled in directly (same cleanup hamlib.c
                       // got when it needed RIT_MAX_HZ too).
#include "rx_audio.h"
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
 * Compile-time configuration
 * --------------------------------------------------------------------- */

// Root of the Linux USB gadget configfs hierarchy. Deliberately left as
// "sbitx_iq" rather than renamed to match the new "sBitx Audio" product
// string - this is an internal configfs path, never seen by the host, and
// renaming it risks breaking any existing OS-level gadget setup script
// that references it (see usb_gadget.h's path-layout comment).
#define UAC_GADGET_ROOT "/sys/kernel/config/usb_gadget/sbitx_iq"

// PCM parameters to match the UAC2 descriptor
#define UAC_RATE 48000
#define UAC_CHANNELS 1        // mono, both directions (was 2 - see uac_gadget_create()'s
                              // c_chmask/p_chmask comment for why the duplicate-to-stereo
                              // hedge was removed)
#define UAC_SAMPLE_BYTES 2    // packed on-wire bytes (16-bit PCM - was 3/24-bit for I/Q)
#define UAC_PERIOD_FRAMES 512 // ALSA period size in frames
#define UAC_PERIODS 4         // number of periods in the ring buffer

// One frame = UAC_CHANNELS * UAC_SAMPLE_BYTES bytes
#define UAC_FRAME_BYTES (UAC_CHANNELS * UAC_SAMPLE_BYTES)

// Internal per-period byte buffers: one for the playback (RX/outbound)
// direction, one for the capture (TX/inbound) direction - each holds up to
// UAC_PERIOD_FRAMES frames before/after an ALSA write/read.
#define UAC_BUF_FRAMES UAC_PERIOD_FRAMES
static uint8_t uac_pcm_buf[UAC_BUF_FRAMES * UAC_FRAME_BYTES];         // playback (RX)
static uint8_t uac_pcm_capture_buf[UAC_BUF_FRAMES * UAC_FRAME_BYTES]; // capture (TX)

// Maps rx_audio.c's uac_out tap (rx_audio.h) onto 16-bit PCM's +-32767
// range. rx_audio.c's AGC keeps that tap's natural amplitude centered
// around its own AGC_TARGET_AMPLITUDE (500000000.0, rx_audio.c) - this is
// the one compile-time knob the user asked for to retune the level WSJT-X
// actually sees; nothing downstream of uac_writer_thread() needs to change
// if this is adjusted. Not shared with rx_audio.c's own macro (different
// translation units, and this is usb_gadget.c's own concern, not
// rx_audio.c's) - if AGC_TARGET_AMPLITUDE is ever changed there, this
// should be revisited too.
#define UAC_RX_AUDIO_SCALE (32767.0 / 500000000.0)

/* ---------------------------------------------------------------------
 * Audio handoff queues - two independent lock-free SPSC ring buffers, one
 * per direction, replacing the single paired I/Q queue the old version
 * used. Same design as hpsdr_p1.c's I/Q queue (that file's comments have
 * the full derivation of why a plain mutex isn't safe to share with a
 * real-time producer/consumer thread): each queue's producer only ever
 * writes its own head; its consumer only ever writes its own tail.
 * Neither ever blocks on the other, and the two queues don't interact.
 *
 * RX (device -> host, decoded audio for WSJT-X to decode): producer is
 * uac_push_audio_rx() (sound.c's SCHED_FIFO audio thread), consumer is
 * uac_writer_thread() (drains it into the gadget's playback PCM).
 *
 * TX (host -> device, WSJT-X's own generated tone): producer is
 * uac_reader_thread() (reads the gadget's capture PCM), consumer is
 * uac_pull_audio_tx() (sound.c's audio thread, for RADIO_MODE_DIGITAL).
 * --------------------------------------------------------------------- */
#define UAC_QUEUE_CAP                                                                             \
  8192 // power of two; ~170ms at 48kHz -
       // generous slack against USB-side stalls
#define UAC_QUEUE_MASK (UAC_QUEUE_CAP - 1)

static double uac_q_rx[UAC_QUEUE_CAP]; // device -> host (RX/outbound audio)
static atomic_uint uac_q_rx_head = 0;
static atomic_uint uac_q_rx_tail = 0;

static double uac_q_tx[UAC_QUEUE_CAP]; // host -> device (TX/inbound audio)
static atomic_uint uac_q_tx_head = 0;
static atomic_uint uac_q_tx_tail = 0;

/* ---------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
static snd_pcm_t *uac_pcm_handle = NULL;         // ALSA PCM write handle (playback/RX) -
                                                  // owned by uac_writer_thread() only
static snd_pcm_t *uac_pcm_capture_handle = NULL; // ALSA PCM read handle (capture/TX) -
                                                  // owned by uac_reader_thread() only
static int uac_gadget_up = 0;                    // 1 after configfs gadget is created
static volatile int uac_active = 0;              // 1 while the RX/playback stream is up
static volatile int uac_capture_active = 0;      // 1 while the TX/capture stream is up
static pthread_t uac_writer_tid;
static volatile int uac_writer_running = 0; // 1 while uac_writer_thread() should keep looping
static pthread_t uac_reader_tid;
static volatile int uac_reader_running = 0; // 1 while uac_reader_thread() should keep looping

/* ---------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

// Write a NUL-terminated string to a sysfs/configfs attribute file.
// Returns 0 on success, -1 on error (errno is preserved).
static int uac_write_attr(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, value, strlen(value));
  close(fd);
  return (n == (ssize_t)strlen(value)) ? 0 : -1;
}

// Create a directory if it does not already exist.
// Mirrors `mkdir -p` for a single level.
static int uac_mkdir(const char *path) {
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

// Create a symbolic link, tolerating EEXIST.
static int uac_symlink(const char *target, const char *link) {
  if (symlink(target, link) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

// Probe for an ALSA card whose /proc/asound/cardN/id matches target_id
// exactly. Sets card_idx to the card number and returns 0 on success,
// -1 if not found.
static int uac_find_card_by_id(const char *target_id, int *card_idx) {
  for (int c = 0; c < 32; c++) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char id[64] = {0};
    if (fgets(id, sizeof(id), f)) {
      // Strip trailing newline
      id[strcspn(id, "\n")] = '\0';
      if (strcmp(id, target_id) == 0) {
        *card_idx = c;
        fclose(f);
        return 0;
      }
    }
    fclose(f);
  }
  return -1;
}

// Detect the first UDC (USB Device Controller) available on this system by
// listing /sys/class/udc/. Copies the UDC name into buf (max len).
// Returns 0 on success, -1 if no UDC is found.
static int uac_find_udc(char *buf, size_t len) {
  DIR *d = opendir("/sys/class/udc");
  if (!d)
    return -1;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.')
      continue;
    snprintf(buf, len, "%s", de->d_name);
    closedir(d);
    return 0;
  }
  closedir(d);
  return -1;
}

/* ---------------------------------------------------------------------
 * Gadget configuration via configfs
 * --------------------------------------------------------------------- */

// Create and configure the UAC2 gadget under configfs.
// Idempotent: if the gadget already exists (leftover from a previous run
// that didn't shut down cleanly - Ctrl+C, systemd restarting us, a crash,
// or SIGKILL - none of which reach uac_stop()/uac_gadget_destroy()), this
// function detects the existing tree and skips redundant mkdir/write calls.
// Returns 0 on success, -1 on any configfs error.
static int uac_gadget_create(void) {
  char path[256];

  // --- Gadget root ---
  if (uac_mkdir(UAC_GADGET_ROOT) < 0) {
    fprintf(stderr, "uac: cannot create gadget root %s: %s\n", UAC_GADGET_ROOT, strerror(errno));
    return -1;
  }

  // Set as soon as a gadget directory exists, not only after a full
  // success, so a failed bind still gets cleaned up by uac_stop() -
  // see docs/dsp_design_notes/usb_gadget_OS_setup.md §8 for the restart bug this fixed.
  uac_gadget_up = 1;

  // Self-heal: unbind a gadget left BOUND by a previous run before
  // reconfiguring/rebinding, or the kernel refuses with EBUSY on every
  // restart after the first - see docs/dsp_design_notes/usb_gadget_OS_setup.md §8.
  // Idempotent even when nothing was actually bound.
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  {
    FILE *udc_check = fopen(path, "r");
    if (udc_check) {
      char cur[256] = {0};
      if (fgets(cur, sizeof(cur), udc_check))
        cur[strcspn(cur, "\n")] = '\0';
      fclose(udc_check);
      if (cur[0] != '\0') {
        printf("uac: gadget still bound to '%s' from a previous run - unbinding first\n", cur);
        // NOT "" - see uac_gadget_destroy()'s unbind comment below.
        if (uac_write_attr(path, "\n") < 0)
          fprintf(stderr, "uac: unbind write failed: %s\n", strerror(errno));
      }
    }
    // ENOENT here just means no leftover tree at all (first boot,
    // or a prior clean uac_gadget_destroy()) - nothing to unbind.
  }

  // USB IDs: use the HermesLite vendor/product pair to stay compatible with
  // SDR apps that enumerate by USB ID, while the product string distinguishes us.
  uac_write_attr(UAC_GADGET_ROOT "/idVendor", "0x04B4");  // Cypress / generic
  uac_write_attr(UAC_GADGET_ROOT "/idProduct", "0x0008"); // generic audio
  uac_write_attr(UAC_GADGET_ROOT "/bcdUSB", "0x0200");    // USB 2.0
  uac_write_attr(UAC_GADGET_ROOT "/bcdDevice", "0x0100");

  // --- String descriptors (English) ---
  snprintf(path, sizeof(path), "%s/strings/0x409", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/strings/0x409/manufacturer", UAC_GADGET_ROOT);
  uac_write_attr(path, "sBitx");
  snprintf(path, sizeof(path), "%s/strings/0x409/product", UAC_GADGET_ROOT);
  uac_write_attr(path, "sBitx Audio");
  // Serial number: a FIXED value on purpose, so a host sees one stable
  // device identity across replugs rather than minting a fresh device
  // instance (and, on Windows, a fresh COM port number for the ACM
  // function) every time the cable is touched.
  //
  // The cost of that stability is that a host also caches per-device
  // state against this identity, and keeps using it even after the
  // gadget's own descriptors change underneath. Windows in particular
  // caches an audio endpoint's supported/default PCM format keyed on
  // VID/PID/serial - so after step 11's 2-channel -> 1-channel change,
  // a Windows box that had already enumerated the stereo version can go
  // on believing this device is stereo, and refuse every direct (non-
  // Sound-Mapper) open with a format it no longer supports. Unplugging
  // and replugging does NOT clear that, precisely because the serial
  // hasn't changed.
  //
  // Bumped 0000001 -> 0000002 to force exactly one clean break: a host
  // that has cached the old stereo descriptors sees an entirely new
  // device and builds its endpoint state fresh from the current
  // descriptors. This is a deliberate one-time bump, not a value to
  // randomize per run - a serial that changed every start would leave a
  // trail of ghost device instances and hand FLRig a different COM port
  // number on every restart. Bump it again (0000003, ...) only if some
  // future descriptor change needs the same clean break.
  snprintf(path, sizeof(path), "%s/strings/0x409/serialnumber", UAC_GADGET_ROOT);
  uac_write_attr(path, "0000002");

  // --- UAC2 function ---
  snprintf(path, sizeof(path), "%s/functions/uac2.0", UAC_GADGET_ROOT);
  if (uac_mkdir(path) < 0 && errno != EEXIST) {
    fprintf(stderr, "uac: cannot create uac2 function: %s\n", strerror(errno));
    return -1;
  }

  // Capture (host reads decoded audio from us): 1 ch (mono), 16-bit,
  // 48 kHz - genuinely single-channel, matching the convention every
  // real ham-radio digital-mode audio interface (SignaLink, RigBlaster,
  // and similar) already uses, both directions. An earlier revision
  // duplicated the mono sample onto 2 channels "for compatibility with
  // host-side apps/drivers that assume a stereo audio interface" - that
  // hedge was never actually needed (WSJT-X and Windows' own audio stack
  // treat a mono UAC2 device as the ordinary case, not an edge case) and
  // cost real code: uac_writer_thread()'s packing had to write every
  // sample twice, and uac_reader_thread() had to average two channels
  // back into one on the way in, inventing an ambiguity (true stereo vs.
  // duplicated-mono from the host) that a single real channel doesn't
  // have. Removed once WSJT-X bring-up on real hardware stalled and this
  // was identified as unnecessary complexity worth shedding.
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_srate", UAC_GADGET_ROOT);
  uac_write_attr(path, "48000");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_ssize", UAC_GADGET_ROOT);
  uac_write_attr(path, "2"); // 2 bytes = 16-bit PCM (was 3/24-bit for I/Q)
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_chmask", UAC_GADGET_ROOT);
  uac_write_attr(path, "1"); // bitmask: ch0 only (mono) - was "3" (L+R)

  // Playback (host -> device): WSJT-X's own generated TX tone for
  // RADIO_MODE_DIGITAL - genuinely used now, unlike the I/Q version's
  // declared-but-unused pair (see usb_gadget.h). Mono, same reasoning
  // as the capture side above.
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_srate", UAC_GADGET_ROOT);
  uac_write_attr(path, "48000");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_ssize", UAC_GADGET_ROOT);
  uac_write_attr(path, "2");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_chmask", UAC_GADGET_ROOT);
  uac_write_attr(path, "1"); // was "3" (L+R)

  // --- ACM (CDC-ACM) function: Kenwood TS-480-subset CAT control ---
  // See the CAT section near the bottom of this file and
  // docs/04_remote_control_and_iq_output.md. Unlike
  // uac2.0 above, the kernel's f_acm function has essentially no
  // configurable attributes to set here (just a read-only port_num) -
  // creating the directory is the whole job. Binding this gives us
  // /dev/ttyGS0 on this side; Windows 10/11 auto-binds its inbox
  // usbser.sys driver on the host side with no custom INF, since this
  // presents as a standard bInterfaceClass=0x02/bInterfaceSubClass=0x02
  // CDC-ACM interface - same "just works" story UAC2 already gets for
  // audio class devices.
  snprintf(path, sizeof(path), "%s/functions/acm.usb0", UAC_GADGET_ROOT);
  if (uac_mkdir(path) < 0 && errno != EEXIST) {
    fprintf(stderr, "uac: cannot create acm function: %s\n", strerror(errno));
    return -1;
  }

  // --- Config c.1 ---
  snprintf(path, sizeof(path), "%s/configs/c.1", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409/configuration", UAC_GADGET_ROOT);
  uac_write_attr(path, "Default");
  snprintf(path, sizeof(path), "%s/configs/c.1/bmAttributes", UAC_GADGET_ROOT);
  uac_write_attr(path, "0xC0"); // self-powered + bus-powered
  snprintf(path, sizeof(path), "%s/configs/c.1/MaxPower", UAC_GADGET_ROOT);
  uac_write_attr(path, "250"); // 250 x 2 mA = 500 mA

  // --- Link function into config ---
  char func_abs[256], link_path[256];
  snprintf(func_abs, sizeof(func_abs), "%s/functions/uac2.0", UAC_GADGET_ROOT);
  snprintf(link_path, sizeof(link_path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
  uac_symlink(func_abs, link_path);

  snprintf(func_abs, sizeof(func_abs), "%s/functions/acm.usb0", UAC_GADGET_ROOT);
  snprintf(link_path, sizeof(link_path), "%s/configs/c.1/acm.usb0", UAC_GADGET_ROOT);
  uac_symlink(func_abs, link_path);

  // --- Bind to the UDC ---
  char udc_name[256] = {0}; // sized to match dirent.d_name's worst case
  if (uac_find_udc(udc_name, sizeof(udc_name)) < 0) {
    fprintf(stderr, "uac: no UDC found — USB gadget not available\n");
    // Not a hard failure: minibitx continues over HPSDR/UDP without UAC
    return -1;
  }
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  if (uac_write_attr(path, udc_name) < 0) {
    fprintf(stderr, "uac: cannot bind to UDC '%s': %s\n", udc_name, strerror(errno));
    return -1;
  }

  printf("init: USB gadget bound to UDC '%s' (UAC2 audio + ACM CAT)\n", udc_name);
  return 0;
}

// Tear down the gadget: unbind from UDC, unlink function, remove configfs nodes.
// A best-effort cleanup — errors are logged but not fatal.
static void uac_gadget_destroy(void) {
  char path[256];

  // Unbind: write a single newline to UDC, NOT a true empty (0-byte)
  // write - a 0-length write doesn't reliably reach the kernel's UDC
  // store callback, matching the shell idiom `echo "" > UDC` (which
  // itself writes one byte) rather than the literal empty string. See
  // docs/dsp_design_notes/usb_gadget_OS_setup.md §8 for the bench story behind this.
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  uac_write_attr(path, "\n");

  // Remove the function symlinks from the config
  snprintf(path, sizeof(path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
  unlink(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/acm.usb0", UAC_GADGET_ROOT);
  unlink(path);

  // Remove config strings, config, and the uac2.0 function's own
  // directory - but NOT functions/acm.usb0's own directory. That one
  // is deliberately skipped: bench-confirmed to hang this process
  // forever, unkillable even with SIGKILL (a kernel-side issue in
  // u_serial.c/usb_f_acm.c freeing the gserial/ttyGS0 port on rmdir -
  // see docs/dsp_design_notes/usb_gadget_OS_setup.md §14 for the full kernel stack and
  // writeup). Safe to skip: configfs is in-memory and doesn't survive
  // a reboot, and uac_gadget_create()'s self-heal already tolerates a
  // leftover tree, so restarting on the same boot just reuses the
  // never-freed ACM function instead of recreating it.
  char dirs[4][256];
  snprintf(dirs[0], 256, "%s/configs/c.1/strings/0x409", UAC_GADGET_ROOT);
  snprintf(dirs[1], 256, "%s/configs/c.1", UAC_GADGET_ROOT);
  snprintf(dirs[2], 256, "%s/functions/uac2.0", UAC_GADGET_ROOT);
  snprintf(dirs[3], 256, "%s/strings/0x409", UAC_GADGET_ROOT);
  for (int i = 0; i < 4; i++)
    rmdir(dirs[i]); // silently tolerate ENOTEMPTY / ENOENT

  // Also deliberately skipped: rmdir(UAC_GADGET_ROOT) itself, which
  // would fail anyway (ENOTEMPTY) since acm.usb0 is still there -
  // stated outright here rather than left as a silent ENOTEMPTY.

  printf("uac: gadget shutdown\n");
}

/* ---------------------------------------------------------------------
 * UAC2 gadget ALSA PCM setup
 * --------------------------------------------------------------------- */

// Open and configure the UAC2 gadget's own ALSA PCM for write (playback
// side, device -> host). Once bound to a UDC, the kernel's UAC2 function
// driver registers its OWN independent ALSA card, always id "UAC2Gadget"
// in /proc/asound/cards - its device-0 playback PCM is what's actually
// wired to the real USB isochronous endpoint the host reads (as ITS
// capture/microphone input). An earlier version wrote to a separate
// snd-aloop "Loopback" card instead, on the mistaken assumption UAC2 reads
// its capture side from that automatically - it doesn't, so nothing ever
// reached the USB link; see docs/dsp_design_notes/usb_gadget_OS_setup.md
// §11 for that bench story.
// Returns 0 on success, -1 on ALSA error.
static int uac_alsa_open(void) {
  int card_idx = -1;
  if (uac_find_card_by_id("UAC2Gadget", &card_idx) < 0) {
    fprintf(stderr, "uac: UAC2Gadget ALSA card not found - is the gadget bound to a UDC?\n");
    return -1;
  }

  // Device 0's playback substream - the local write side that feeds the
  // host's capture stream. Device 0's *capture* substream is the reverse
  // direction (host-to-device audio - see uac_alsa_open_capture() below,
  // and p_srate/p_ssize/p_chmask in uac_gadget_create()), not this device
  // string.
  char dev_name[64];
  snprintf(dev_name, sizeof(dev_name), "hw:%d,0", card_idx);

  int err;
  if ((err = snd_pcm_open(&uac_pcm_handle, dev_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    fprintf(stderr, "uac: snd_pcm_open(%s, playback) failed: %s\n", dev_name, snd_strerror(err));
    uac_pcm_handle = NULL;
    return -1;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(uac_pcm_handle, hw);

  // Interleaved, 16-bit LE, 48 kHz, mono (UAC_CHANNELS)
  snd_pcm_hw_params_set_access(uac_pcm_handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(uac_pcm_handle, hw, SND_PCM_FORMAT_S16_LE);
  unsigned int rate = UAC_RATE;
  snd_pcm_hw_params_set_rate_near(uac_pcm_handle, hw, &rate, NULL);
  snd_pcm_hw_params_set_channels(uac_pcm_handle, hw, UAC_CHANNELS);

  snd_pcm_uframes_t period = UAC_PERIOD_FRAMES;
  snd_pcm_hw_params_set_period_size_near(uac_pcm_handle, hw, &period, NULL);
  unsigned int periods = UAC_PERIODS;
  snd_pcm_hw_params_set_periods_near(uac_pcm_handle, hw, &periods, NULL);

  if ((err = snd_pcm_hw_params(uac_pcm_handle, hw)) < 0) {
    fprintf(stderr, "uac: snd_pcm_hw_params (playback) failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    return -1;
  }

  if ((err = snd_pcm_prepare(uac_pcm_handle)) < 0) {
    fprintf(stderr, "uac: snd_pcm_prepare (playback) failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    return -1;
  }

  printf("uac: UAC2Gadget playback PCM opened: %s @ %u Hz, 16-bit, %d ch\n", dev_name, rate,
         UAC_CHANNELS);
  return 0;
}

// Open and configure the UAC2 gadget's own ALSA PCM for read (capture
// side, host -> device) - the SAME card uac_alsa_open() above opens, same
// device 0, just the other of its two independent PCM substream
// directions (see usb_gadget.h's "one card, two substream directions"
// note). What's read here is what the host SENT (WSJT-X's own generated
// TX tone, as ITS playback/speaker output). Best-effort: a failure here is
// logged by the caller and does not prevent the RX/playback direction
// (uac_alsa_open() above) from working.
// Returns 0 on success, -1 on ALSA error.
static int uac_alsa_open_capture(void) {
  int card_idx = -1;
  if (uac_find_card_by_id("UAC2Gadget", &card_idx) < 0) {
    fprintf(stderr, "uac: UAC2Gadget ALSA card not found for capture - is the gadget bound to a UDC?\n");
    return -1;
  }

  char dev_name[64];
  snprintf(dev_name, sizeof(dev_name), "hw:%d,0", card_idx);

  int err;
  if ((err = snd_pcm_open(&uac_pcm_capture_handle, dev_name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
    fprintf(stderr, "uac: snd_pcm_open(%s, capture) failed: %s\n", dev_name, snd_strerror(err));
    uac_pcm_capture_handle = NULL;
    return -1;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(uac_pcm_capture_handle, hw);

  snd_pcm_hw_params_set_access(uac_pcm_capture_handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(uac_pcm_capture_handle, hw, SND_PCM_FORMAT_S16_LE);
  unsigned int rate = UAC_RATE;
  snd_pcm_hw_params_set_rate_near(uac_pcm_capture_handle, hw, &rate, NULL);
  snd_pcm_hw_params_set_channels(uac_pcm_capture_handle, hw, UAC_CHANNELS);

  snd_pcm_uframes_t period = UAC_PERIOD_FRAMES;
  snd_pcm_hw_params_set_period_size_near(uac_pcm_capture_handle, hw, &period, NULL);
  unsigned int periods = UAC_PERIODS;
  snd_pcm_hw_params_set_periods_near(uac_pcm_capture_handle, hw, &periods, NULL);

  if ((err = snd_pcm_hw_params(uac_pcm_capture_handle, hw)) < 0) {
    fprintf(stderr, "uac: snd_pcm_hw_params (capture) failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_capture_handle);
    uac_pcm_capture_handle = NULL;
    return -1;
  }

  if ((err = snd_pcm_prepare(uac_pcm_capture_handle)) < 0) {
    fprintf(stderr, "uac: snd_pcm_prepare (capture) failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_capture_handle);
    uac_pcm_capture_handle = NULL;
    return -1;
  }

  printf("uac: UAC2Gadget capture PCM opened: %s @ %u Hz, 16-bit, %d ch\n", dev_name, rate,
         UAC_CHANNELS);
  return 0;
}

/* ---------------------------------------------------------------------
 * Writer thread — owns uac_pcm_handle exclusively; the only thing that
 * ever calls snd_pcm_writei() on it. uac_push_audio_rx() (called from
 * sound.c's real-time audio thread) only ever touches the RX lock-free
 * queue above and never blocks; this thread is the sole consumer,
 * waiting for a full period's worth of samples and writing them to the
 * gadget's PCM, so a blocked or absent USB host can only cost USB audio
 * quality, never the radio's own real hardware timing. Ported from an
 * earlier design that wrote inline from the audio thread and
 * reintroduced sound.c's xrun flood whenever no host was draining the
 * gadget - see docs/dsp_design_notes/usb_gadget_OS_setup.md §7 for that bench story.
 * --------------------------------------------------------------------- */
static void *uac_writer_thread(void *arg) {
  (void)arg;

  // err_streak drives the backoff below; host_was_draining logs state
  // *transitions* only, not every attempt. Both start pessimistic and
  // require sustained evidence before flipping - see
  // docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why (a naive version logged a
  // spurious transition on every cold boot with no cable connected).
  unsigned err_streak = 0;
  unsigned success_streak = 0;
  unsigned pending_err_streak = 0; // err_streak snapshotted at the start
                                   // of the current run of successes,
                                   // for the "draining again after N
                                   // failed writes" message once that
                                   // run is confirmed real (below)
  int host_was_draining = 0;

  while (uac_writer_running) {
    unsigned head = atomic_load_explicit(&uac_q_rx_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&uac_q_rx_tail, memory_order_relaxed);
    unsigned available = (head - tail) & UAC_QUEUE_MASK;

    if (available < UAC_BUF_FRAMES) {
      // Not enough queued yet - this thread isn't real-time
      // critical (see above), so a plain short sleep is fine.
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L}; // 2ms
      nanosleep(&ts, NULL);
      continue;
    }

    // Back off the retry pace itself, not just the logging, while no
    // host is draining the gadget - ramps from one period up to a
    // ~1s cap as the failure streak grows, reset immediately on a
    // success. See docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why (retrying
    // at full pace forever would make "no cable" a low-grade cost
    // instead of the fully-supported idle state it's meant to be).
    if (err_streak > 0) {
      unsigned backoff_periods = err_streak;
      if (backoff_periods > 100)
        backoff_periods = 100; // cap ~1.07s
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 10700000L};
      for (unsigned b = 0; b < backoff_periods && uac_writer_running; b++)
        nanosleep(&ts, NULL);
      if (!uac_writer_running)
        break;
    }

    for (int s = 0; s < UAC_BUF_FRAMES; s++) {
      double sample = uac_q_rx[tail];
      tail = (tail + 1) & UAC_QUEUE_MASK;

      // Scale rx_audio.c's uac_out tap onto 16-bit PCM's signed range
      // (UAC_RX_AUDIO_SCALE above) and clamp - this tap is deliberately
      // unnormalized/uncapped (rx_audio.h), so clamping into the
      // destination range is this file's own job, not rx_audio.c's.
      double scaled = sample * UAC_RX_AUDIO_SCALE;
      if (scaled > 32767.0)
        scaled = 32767.0;
      if (scaled < -32768.0)
        scaled = -32768.0;
      int16_t pcm = (int16_t)scaled;

      uint8_t *slot = uac_pcm_buf + s * UAC_FRAME_BYTES;
      // One mono channel (see uac_gadget_create()'s c_chmask comment) -
      // SND_PCM_FORMAT_S16_LE packs it as 2 bytes, little-endian.
      slot[0] = (uint8_t)(pcm & 0xFF);
      slot[1] = (uint8_t)((pcm >> 8) & 0xFF);
    }
    atomic_store_explicit(&uac_q_rx_tail, tail, memory_order_release);

    // Flush a full period to the gadget's PCM. This can block (or
    // fail) if nothing is draining the other side - that's now
    // confined to this thread only.
    snd_pcm_sframes_t written =
        snd_pcm_writei(uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);

    if (written == -EPIPE) {
      // Buffer underrun (most commonly: no host draining the
      // gadget yet) - attempt recovery then retry once.
      snd_pcm_prepare(uac_pcm_handle);
      written = snd_pcm_writei(uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);
    }

    if (written < 0) {
      // Most commonly means no USB host has activated the capture
      // interface yet (cable unplugged, or no app has opened it) -
      // a normal, expected state, not a fault, so log the
      // *transition* into it once rather than every retry. See
      // docs/dsp_design_notes/usb_gadget_OS_setup.md §11.
      if (host_was_draining) {
        fprintf(stderr,
                "uac: no USB data user (%s)\n",
                snd_strerror((int)written));
        host_was_draining = 0;
      }
      success_streak = 0;
      err_streak++;
      snd_pcm_recover(uac_pcm_handle, (int)written, 1 /*silent*/);
    } else {
      if (success_streak == 0) {
        // First success of a new run - remember how many writes
        // failed right before it, for the "draining again after
        // N failed writes" message below, once/if this run turns
        // out to be real (not just the buffer swallowing a few
        // writes for free with nothing on the other end - see
        // the success_streak threshold below). err_streak itself
        // gets reset right away regardless, since the backoff
        // pacing above needs to see "no current failure streak"
        // as soon as a write succeeds.
        pending_err_streak = err_streak;
      }
      success_streak++;
      err_streak = 0;

      // Require more consecutive successes than UAC_PERIODS before
      // trusting it as proof of a draining host - see
      // docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why a short run can
      // succeed purely from empty-buffer slack.
      if (!host_was_draining && success_streak > UAC_PERIODS) {
        if (pending_err_streak > 0) {
          fprintf(stderr, "uac: USB host draining again after %u failed write(s)\n",
                  pending_err_streak);
        } else {
          fprintf(stderr, "uac: USB host draining the gadget\n");
        }
        host_was_draining = 1;
      }
    }
  }

  return NULL;
}

/* ---------------------------------------------------------------------
 * Reader thread — owns uac_pcm_capture_handle exclusively; the only
 * thing that ever calls snd_pcm_readi() on it. Mirrors
 * uac_writer_thread() above in reverse: it's the sole producer into the
 * TX lock-free queue (uac_pull_audio_tx(), called from sound.c's
 * real-time audio thread, is the sole consumer and never blocks), so a
 * blocked or absent USB host (WSJT-X not running, or not yet sending TX
 * audio) can only cost this thread's own pacing, never the radio's real
 * hardware timing. Same backoff/retry/transition-logging pattern as the
 * writer thread, for the same reason (docs/dsp_design_notes/
 * usb_gadget_OS_setup.md §11) - just watching for a host that starts
 * SENDING instead of one that starts DRAINING.
 * --------------------------------------------------------------------- */
static void *uac_reader_thread(void *arg) {
  (void)arg;

  unsigned err_streak = 0;
  unsigned success_streak = 0;
  unsigned pending_err_streak = 0;
  int host_was_sending = 0;

  while (uac_reader_running) {
    unsigned head = atomic_load_explicit(&uac_q_tx_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&uac_q_tx_tail, memory_order_acquire);
    unsigned free_space = UAC_QUEUE_CAP - ((head - tail) & UAC_QUEUE_MASK) - 1;

    if (free_space < UAC_BUF_FRAMES) {
      // uac_pull_audio_tx() isn't draining fast enough yet (or at all,
      // e.g. not in RADIO_MODE_DIGITAL) - same "not real-time critical,
      // a short sleep is fine" reasoning as the writer thread's own
      // backpressure wait above.
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L}; // 2ms
      nanosleep(&ts, NULL);
      continue;
    }

    if (err_streak > 0) {
      unsigned backoff_periods = err_streak;
      if (backoff_periods > 100)
        backoff_periods = 100; // cap ~1.07s
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 10700000L};
      for (unsigned b = 0; b < backoff_periods && uac_reader_running; b++)
        nanosleep(&ts, NULL);
      if (!uac_reader_running)
        break;
    }

    snd_pcm_sframes_t got =
        snd_pcm_readi(uac_pcm_capture_handle, uac_pcm_capture_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);

    if (got == -EPIPE) {
      // Buffer overrun (most commonly: no host sending TX audio yet) -
      // attempt recovery then retry once.
      snd_pcm_prepare(uac_pcm_capture_handle);
      got = snd_pcm_readi(uac_pcm_capture_handle, uac_pcm_capture_buf,
                           (snd_pcm_uframes_t)UAC_BUF_FRAMES);
    }

    if (got < 0) {
      // Most commonly means no USB host has activated the playback
      // interface yet (cable unplugged, WSJT-X not running or not yet
      // transmitting) - a normal, expected state, not a fault, so log
      // the *transition* into it once rather than every retry.
      if (host_was_sending) {
        fprintf(stderr, "uac: no USB TX audio source (%s)\n", snd_strerror((int)got));
        host_was_sending = 0;
      }
      success_streak = 0;
      err_streak++;
      snd_pcm_recover(uac_pcm_capture_handle, (int)got, 1 /*silent*/);
      continue;
    }

    if (success_streak == 0) {
      pending_err_streak = err_streak;
    }
    success_streak++;
    err_streak = 0;

    if (!host_was_sending && success_streak > UAC_PERIODS) {
      if (pending_err_streak > 0) {
        fprintf(stderr, "uac: USB host sending TX audio again after %u failed read(s)\n",
                pending_err_streak);
      } else {
        fprintf(stderr, "uac: USB host sending TX audio\n");
      }
      host_was_sending = 1;
    }

    // Unpack S16_LE mono frames, normalized to roughly [-1, +1] - a
    // fixed, unambiguous conversion (see usb_gadget.h's
    // uac_pull_audio_tx() comment for why this needs no separate
    // compile-time scale the way UAC_RX_AUDIO_SCALE does). One real
    // channel now (c_chmask/p_chmask above), not two averaged together -
    // removes the earlier "does the host send true stereo or duplicate
    // one channel" ambiguity entirely, since there's only one channel to
    // read.
    for (snd_pcm_sframes_t s = 0; s < got; s++) {
      uint8_t *slot = uac_pcm_capture_buf + s * UAC_FRAME_BYTES;
      int16_t sample = (int16_t)(slot[0] | (slot[1] << 8));
      double mono = (double)sample / 32768.0;

      uac_q_tx[head] = mono;
      head = (head + 1) & UAC_QUEUE_MASK;
    }
    atomic_store_explicit(&uac_q_tx_head, head, memory_order_release);
  }

  return NULL;
}

/* ---------------------------------------------------------------------
 * Public API — see usb_gadget.h
 * --------------------------------------------------------------------- */

int uac_init(void) {
  if (uac_gadget_create() < 0) {
    // uac_gadget_create already printed the reason. Note: it may
    // still have left a gadget directory that needs cleanup - it
    // sets uac_gadget_up itself now (as soon as it creates one),
    // precisely so a failed bind here doesn't skip that cleanup.
    return -1;
  }

  if (uac_alsa_open() < 0) {
    uac_gadget_destroy();
    uac_gadget_up = 0;
    return -1;
  }

  uac_writer_running = 1;
  if (pthread_create(&uac_writer_tid, NULL, uac_writer_thread, NULL) != 0) {
    fprintf(stderr, "uac: failed to start writer thread\n");
    uac_writer_running = 0;
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    uac_gadget_destroy();
    uac_gadget_up = 0;
    return -1;
  }

  uac_active = 1;

  // Capture (TX/inbound) direction is best-effort, same spirit as the
  // rest of this file: a failure here does not unwind the RX/playback
  // side already brought up above, and WSJT-X's decoded audio keeps
  // flowing either way - only TX audio into RADIO_MODE_DIGITAL won't
  // arrive until this succeeds (see usb_gadget.h's uac_init() comment).
  if (uac_alsa_open_capture() == 0) {
    uac_reader_running = 1;
    if (pthread_create(&uac_reader_tid, NULL, uac_reader_thread, NULL) != 0) {
      fprintf(stderr, "uac: failed to start reader thread\n");
      uac_reader_running = 0;
      snd_pcm_close(uac_pcm_capture_handle);
      uac_pcm_capture_handle = NULL;
    } else {
      uac_capture_active = 1;
    }
  }

  printf("uac: USB audio stream ready — device name: 'sBitx Audio'\n");
  return 0;
}

void uac_push_audio_rx(double sample) {
  if (!uac_active)
    return;

  // Lock-free producer (see the writer-thread comment above): never
  // blocks, never touches uac_q_rx_tail. On overflow (writer thread
  // stalled behind a slow/absent USB host) it silently drops the new
  // sample rather than waiting - exactly hpsdr_p1.c's
  // hpsdr_send_iq()/IQ_QUEUE pattern.
  unsigned head = atomic_load_explicit(&uac_q_rx_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&uac_q_rx_tail, memory_order_acquire);
  unsigned next_head = (head + 1) & UAC_QUEUE_MASK;
  if (next_head == tail)
    return; // queue full - drop this sample

  uac_q_rx[head] = sample;
  atomic_store_explicit(&uac_q_rx_head, next_head, memory_order_release);
}

int uac_pull_audio_tx(double *out, int n) {
  if (!uac_capture_active)
    return 0;

  // Lock-free consumer (see the reader-thread comment above): never
  // blocks, never touches uac_q_tx_head. Returns fewer samples than
  // asked for (0 if none queued) rather than waiting - sound.c fills
  // any shortfall with silence (usb_gadget.h).
  unsigned tail = atomic_load_explicit(&uac_q_tx_tail, memory_order_relaxed);
  unsigned head = atomic_load_explicit(&uac_q_tx_head, memory_order_acquire);
  unsigned available = (head - tail) & UAC_QUEUE_MASK;

  int count = (int)available;
  if (count > n)
    count = n;

  for (int i = 0; i < count; i++) {
    out[i] = uac_q_tx[tail];
    tail = (tail + 1) & UAC_QUEUE_MASK;
  }
  atomic_store_explicit(&uac_q_tx_tail, tail, memory_order_release);
  return count;
}

void uac_stop(void) {
  uac_active = 0;
  uac_capture_active = 0;

  if (uac_writer_running) {
    uac_writer_running = 0;
    pthread_join(uac_writer_tid, NULL);
  }
  if (uac_reader_running) {
    uac_reader_running = 0;
    pthread_join(uac_reader_tid, NULL);
  }

  if (uac_pcm_handle) {
    snd_pcm_drain(uac_pcm_handle);
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    printf("uac: ALSA playback PCM closed\n");
  }
  if (uac_pcm_capture_handle) {
    snd_pcm_close(uac_pcm_capture_handle);
    uac_pcm_capture_handle = NULL;
    printf("uac: ALSA capture PCM closed\n");
  }

  if (uac_gadget_up) {
    uac_gadget_destroy();
    uac_gadget_up = 0;
  }
}

int uac_is_active(void) { return uac_active; }

/* =======================================================================
 * Kenwood TS-480-subset CAT control, over this same gadget's CDC-ACM
 * function acm.usb0 (created/bound in uac_gadget_create() above; see
 * usb_gadget.h for why this is folded into the same file rather than a
 * separate translation unit). See docs/04_remote_control_and_iq_output.md
 * for the command set and why TS-480, and docs/dsp_design_notes/usb_gadget_OS_setup.md
 * §13 for the bench-test checklist. Best-effort like the rest of this
 * file: cat_init() failing is not fatal.
 *
 * Unlike hamlib.c's TCP server, no accept()/one-thread-per-client model
 * is needed - a USB gadget serial function is inherently one logical
 * connection at a time, so this is a single persistent reader thread
 * that opens the device once and re-opens it if it ever drops.
 * ======================================================================= */

#define CAT_TTY_PATH "/dev/ttyGS0"
#define CAT_LINE_MAX 64
#define CAT_OPEN_RETRY_MAX_MS 1000 // backoff cap while the device is missing/erroring

static volatile int cat_running = 0;
static int cat_fd = -1;
static pthread_t cat_thread_tid;

// Mode itself is real now - radio_get_mode()/radio_set_mode() (radio.h)
// are the single owner this surface and hamlib.c's rigctld M/m now both
// agree on - see radio.h's enum radio_mode comment.
//
// mode_to_kenwood_digit()/kenwood_digit_to_mode() below translate
// between that enum and the single-digit Kenwood MD codes this CAT
// surface actually sends/parses. 1/2/3 (LSB/USB/CW) are the standard
// Kenwood convention every TS-480-alike (QMX included) agrees on.
//
// RADIO_MODE_DIGITAL reports as '2' (USB), and that is deliberate.
// An earlier revision sent '9' as a guess at "some data mode", flagged
// in this comment as unconfirmed. It is now confirmed WRONG, by two
// independent sources that agree: QRP Labs' own QMX CAT Programming
// Manual documents digit 9 as "FSK Reverse", and Hamlib's default
// kenwood_mode_table (kenwood.c - ts480.c defines no table of its own,
// so the default is what WSJT-X gets) maps 9 to RTTY-REVERSE. That
// table has no DATA/PKT digit at all in the 1-9 range a TS-480 IF
// response can even express, so there is no honest digit to send: a
// real TS-480 has no data mode. Sending '9' made WSJT-X display the rig
// as RTTY-R.
//
// '2' is the truthful answer instead - FT8 and friends ARE upper
// sideband, and "run the rig in USB and let the digital-mode app own
// the audio" is exactly how a TS-480 is operated for data. The cost is
// that DIGITAL is not reachable or distinguishable over CAT, only
// selectable locally; see the MD set handler for the guard that keeps a
// host's own "MD2" from silently dropping us out of DIGITAL (and with
// it, the uac_pull_audio_tx() TX path) back into mic-sourced USB.
static const struct { enum radio_mode mode; char digit; } mode_digits[] = {
  { RADIO_MODE_LSB,     '1' },
  { RADIO_MODE_USB,     '2' },
  { RADIO_MODE_CW,      '3' },
  { RADIO_MODE_DIGITAL, '2' }, // reports as USB - see comment above
};
#define MODE_DIGITS_COUNT (sizeof(mode_digits) / sizeof(mode_digits[0]))

static char mode_to_kenwood_digit(enum radio_mode m) {
  for (size_t i = 0; i < MODE_DIGITS_COUNT; i++)
    if (mode_digits[i].mode == m)
      return mode_digits[i].digit;
  return '3'; // unreachable given the enum's own values; a safe fallback if that ever changes
}

// Returns 1 and sets *out on a recognized digit, 0 otherwise - so an
// MD set for a digit this table doesn't know stays "anything else is
// silently ignored", same as every other unrecognized CAT command on
// this surface (see cat_handle_command()'s comment), rather than
// silently mapping it to some arbitrary mode.
static int kenwood_digit_to_mode(char digit, enum radio_mode *out) {
  for (size_t i = 0; i < MODE_DIGITS_COUNT; i++) {
    if (mode_digits[i].digit == digit) {
      *out = mode_digits[i].mode;
      return 1;
    }
  }
  return 0;
}

static void cat_send(const char *s) {
  if (cat_fd < 0)
    return;
  // Best-effort - a port that's not actually open on the host side isn't
  // fatal, same spirit as hamlib.c's send_line() using MSG_NOSIGNAL.
  ssize_t n = write(cat_fd, s, strlen(s));
  (void)n;
}

// Handles one already-terminated command (the ';' stripped). Kenwood CAT
// convention, unlike Hamlib's rigctld: "set" commands get no reply,
// only "get" queries do - so most branches below send nothing.
//
// A get's console line only prints when the reply actually differs from
// the last reply of the SAME kind, to avoid flooding the console with
// FLRig's frequent status polling - see docs/04_remote_control_and_iq_output.md
// "Kenwood-CAT emulation over USB" for why. Pass a `static char[]` local
// to each call site (persists across calls, scoped to that command) as
// `last`. Only used on the get side - a *set* always logs.
static void cat_log_get(char *last, size_t last_size, const char *new_reply,
                        const char *log_line) {
  if (strncmp(last, new_reply, last_size) == 0)
    return;
  snprintf(last, last_size, "%s", new_reply); // truncates+NUL-terminates safely
  printf("%s", log_line);
}

static void cat_handle_command(char *cmd) {
  size_t len = strlen(cmd);
  if (len == 0)
    return;

  // --- ID: get only - identify as a Kenwood TS-480 (ID code 020),
  // matching the QRP Labs QMX's own choice for the same reason: FLRig
  // (or anything else) will expect TS-480 behavior from every other
  // command once it sees this. Never actually changes, so this only
  // ever logs once (see cat_log_get() above). ---
  if (len == 2 && strncmp(cmd, "ID", 2) == 0) {
    static char last[8] = "";
    cat_send("ID020;");
    cat_log_get(last, sizeof(last), "ID020;", "cat: ID -> 020 (TS-480)\n");
    return;
  }

  // --- AC: antenna tuner control, part of FLRig's standard status
  // poll - minibitx has no tuner to report on, so silently ignored
  // rather than logged every poll forever. See docs/04's
  // "Kenwood-CAT emulation over USB". ---
  if (len >= 2 && cmd[0] == 'A' && cmd[1] == 'C') {
    return;
  }

  // --- FA: VFO A frequency, 11-digit Hz. Bare "FA" is a get; "FA<11
  // digits>" is a set. ---
  if (len >= 2 && cmd[0] == 'F' && cmd[1] == 'A') {
    if (len == 2) {
      static char last[16] = "";
      char buf[16], log_line[48];
      snprintf(buf, sizeof(buf), "FA%011d;", freq_hdr);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: FA -> %d Hz\n", freq_hdr);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      long f = strtol(cmd + 2, NULL, 10);
      if (f > 0) {
        radio_tune_to((uint32_t)f);
        printf("cat: FA%s -> tuned to %ld Hz\n", cmd + 2, f);
      } else {
        printf("cat: FA%s -> invalid frequency, ignored\n", cmd + 2);
      }
    }
    return;
  }

  // --- FB: VFO B. minibitx has one VFO - mirror FA on get, accept and
  // ignore on set, same "nothing else to switch to" stance Hamlib's V/
  // chk_vfo already takes. ---
  if (len >= 2 && cmd[0] == 'F' && cmd[1] == 'B') {
    if (len == 2) {
      static char last[16] = "";
      char buf[16], log_line[64];
      snprintf(buf, sizeof(buf), "FB%011d;", freq_hdr);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: FB -> %d Hz (single VFO, mirrors FA)\n",
               freq_hdr);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      printf("cat: FB%s -> ok (single VFO, not applied)\n", cmd + 2);
    }
    return;
  }

  // --- RT: RIT on/off - bare "RT;" is a get (replies "RT0;"/"RT1;"),
  // "RT0;"/"RT1;" is a set. Maps straight onto radio_set_rit_enabled()/
  // radio_rit_enabled() (radio.h) - the value itself (radio_get_rit())
  // is untouched either way, exactly like a real rig's RIT ON/OFF
  // button leaves whatever's dialed into the RIT knob alone. See
  // docs/04_remote_control_and_iq_output.md for why this needed a real
  // enable bit in radio.c rather than reusing rigctld's offset-only
  // model (hamlib.c's j/J, which has no separate on/off concept at
  // all - 0 Hz IS off there). ---
  if (len >= 2 && cmd[0] == 'R' && cmd[1] == 'T') {
    if (len == 2) {
      static char last[8] = "";
      char buf[8], log_line[64];
      snprintf(buf, sizeof(buf), "RT%d;", radio_rit_enabled() ? 1 : 0);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: RT -> %s (%+d Hz)\n",
               radio_rit_enabled() ? "on" : "off", radio_get_rit());
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      int on = (cmd[2] != '0');
      radio_set_rit_enabled(on);
      printf("cat: RT%c -> RIT %s (%+d Hz)\n", cmd[2], on ? "on" : "off", radio_get_rit());
    }
    return;
  }

  // --- RC: RIT/XIT clear - real Kenwood rigs zero the offset itself
  // (not just disable it), so this calls radio_set_rit(0) rather than
  // radio_set_rit_enabled(0) - same call rigctld's "J 0" makes, which
  // also implicitly disables (see radio_set_rit()'s comment, radio.h).
  // No reply, matching TX/RX/TQ-set's bare-command convention above. ---
  if (len == 2 && strncmp(cmd, "RC", 2) == 0) {
    radio_set_rit(0);
    printf("cat: RC -> RIT cleared\n");
    return;
  }

  // --- RU / RD: step RIT up/down by a fixed CAT_RIT_STEP_HZ per call,
  // clamped to +/-RIT_MAX_HZ. Real Kenwood rigs accept an optional
  // digit-count suffix ("RU005;") selecting how many of the rig's own
  // configured step sizes to move - accepted here but ignored rather
  // than guessed at, since minibitx has no configured step size to
  // multiply and this hasn't been checked against a real rig's exact
  // convention. Implicitly (re-)enables RIT via radio_set_rit(), same
  // as turning a real RIT knob does regardless of the ON/OFF button's
  // last state. ---
#define CAT_RIT_STEP_HZ 10
  if (len >= 2 && cmd[0] == 'R' && (cmd[1] == 'U' || cmd[1] == 'D')) {
    int delta = (cmd[1] == 'U') ? CAT_RIT_STEP_HZ : -CAT_RIT_STEP_HZ;
    int hz = radio_get_rit() + delta;
    if (hz > RIT_MAX_HZ) hz = RIT_MAX_HZ;
    if (hz < -RIT_MAX_HZ) hz = -RIT_MAX_HZ;
    radio_set_rit(hz);
    printf("cat: R%c -> RIT %+d Hz\n", cmd[1], hz);
    return;
  }

  // --- TX / RX: bare, immediate, no reply (Kenwood convention) - same
  // entry point and same "local CW key wins" guard as Hamlib's T. ---
  if (len == 2 && strncmp(cmd, "TX", 2) == 0) {
    if (cw_tx_active()) {
      printf("cat: TX -> ignored, local CW key holds TX\n");
    } else {
      radio_set_tx(1);
      printf("cat: TX -> TX on\n");
    }
    return;
  }
  if (len == 2 && strncmp(cmd, "RX", 2) == 0) {
    if (cw_tx_active()) {
      printf("cat: RX -> ignored, local CW key holds TX\n");
    } else {
      radio_set_tx(0);
      printf("cat: RX -> TX off\n");
    }
    return;
  }

  // --- TQ: transmit state - "TQ;" is a get (replies), "TQ0;"/"TQ1;" is
  // a set (no reply, same as TX/RX above - this is just another way to
  // ask for the same thing). ---
  if (len >= 2 && cmd[0] == 'T' && cmd[1] == 'Q') {
    if (len == 2) {
      static char last[8] = "";
      char buf[8], log_line[32];
      snprintf(buf, sizeof(buf), "TQ%d;", in_tx ? 1 : 0);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: TQ -> %s\n", in_tx ? "TX" : "RX");
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      int tx_on = (cmd[2] != '0');
      if (cw_tx_active()) {
        printf("cat: TQ%c -> ignored, local CW key holds TX\n", cmd[2]);
      } else {
        radio_set_tx(tx_on);
        printf("cat: TQ%c -> %s\n", cmd[2], tx_on ? "TX on" : "TX off");
      }
    }
    return;
  }

  // --- MD: mode - real now, see mode_to_kenwood_digit()'s comment above. ---
  if (len >= 2 && cmd[0] == 'M' && cmd[1] == 'D') {
    if (len == 2) {
      static char last[8] = "";
      char buf[8], log_line[32];
      snprintf(buf, sizeof(buf), "MD%c;", mode_to_kenwood_digit(radio_get_mode()));
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: MD -> %c\n", mode_to_kenwood_digit(radio_get_mode()));
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      enum radio_mode m;
      if (kenwood_digit_to_mode(cmd[2], &m)) {
        // DIGITAL reports itself as '2'/USB (see mode_digits[] above),
        // so a host that reads the mode back and writes it out again -
        // which WSJT-X does routinely - would otherwise walk us out of
        // DIGITAL and into plain USB, quietly switching the TX source
        // from uac_pull_audio_tx() to the mic and killing transmit.
        // Treat an incoming USB request while already in DIGITAL as the
        // no-op it was almost certainly meant to be. Any OTHER mode
        // request (CW, LSB) is a genuine change and still applies, so
        // this doesn't strand the radio in DIGITAL.
        if (m == RADIO_MODE_USB && radio_get_mode() == RADIO_MODE_DIGITAL) {
          printf("cat: MD%c -> already DIGITAL (reports as USB), staying\n", cmd[2]);
        } else {
          radio_set_mode(m);
          printf("cat: MD%c -> ok\n", cmd[2]);
        }
      } else {
        // Unrecognized digit - same "anything else is silently
        // ignored" convention as any other unhandled CAT command, see
        // kenwood_digit_to_mode()'s comment above.
        printf("cat: MD%c -> unrecognized, ignored\n", cmd[2]);
      }
    }
    return;
  }

  // --- IF: combined status string - get only. The field layout is no
  // longer a guess: it is the classic 38-byte Kenwood IF response, and
  // Hamlib (which is what WSJT-X drives its rig control through) parses
  // it by FIXED character offsets and rejects a wrong-length reply
  // outright. Hamlib's kenwood.c defaults `if_len` to 37 for any backend
  // that doesn't override it - ts480.c doesn't - where if_len counts the
  // reply INCLUDING the leading "IF" but EXCLUDING the ';' terminator.
  // So the wire reply must be exactly 38 bytes: "IF" + 35 payload + ';'.
  // A length mismatch is a hard -RIG_EPROTO after the port's retries,
  // not something Hamlib shrugs off.
  //
  // Layout, as 0-indexed offsets into the reply (';' stripped), which is
  // exactly how Hamlib indexes it:
  //    [0..1]   "IF"
  //    [2..12]  frequency, 11 digits
  //    [13..16] frequency step, 4 chars       (we send zeros)
  //    [17..22] RIT/XIT offset, sign + 5 digits
  //    [23]     RIT on/off
  //    [24]     XIT on/off                    (we have none - '0')
  //    [25]     memory bank                   (none - '0')
  //    [26..27] memory channel                (none - "00")
  //    [28]     RX/TX          <- kenwood_get_ptt() reads exactly here
  //    [29]     mode digit     <- kenwood_get_mode()/IF reads exactly here
  //    [30]     VFO A/B/MEM    <- kenwood_get_vfo_if() reads exactly here,
  //                               and returns -RIG_EPROTO on anything but
  //                               '0'/'1'/'2' - we are always VFO A ('0')
  //    [31]     scan, [32] split, [33] tone, [34..35] tone number,
  //    [36]     reserved                      (all zero here)
  //
  // This replaces an earlier best-effort reconstruction that was 38
  // payload+IF chars (39 on the wire) and put RX/TX and mode one
  // position late, because its RIT field was 5 chars instead of 6 and
  // its tail was one digit too long. That made every Hamlib IF-based
  // call - get_ptt, get_vfo, get_split_vfo - fail with -RIG_EPROTO,
  // which is what WSJT-X surfaces as a "Rig Control Error". rig_open()
  // itself had always succeeded, since Hamlib's kenwood_open() only
  // hard-fails if `ID;` goes unanswered (ours answers ID020;) - which is
  // why CAT looked healthy from FLRig, whose parser is far more
  // forgiving, while WSJT-X refused to connect.
  //
  // RIT_MAX_HZ is 9999, so the signed 6-char field can never overflow.
  if (len == 2 && strncmp(cmd, "IF", 2) == 0) {
    static char last[48] = "";
    char buf[48], log_line[96];
    int rit = radio_get_rit();
    int rit_on = radio_rit_enabled();
    //                       freq      step  RIT   riton xit/bank/memch
    //                         |         |     |     |    |  txrx mode
    //                         |         |     |     |    |    |   |  tail
    snprintf(buf, sizeof(buf), "IF%011d" "0000" "%+06d" "%d" "0000" "%d" "%c" "0000000;",
             freq_hdr, rit, rit_on ? 1 : 0,
             in_tx ? 1 : 0, mode_to_kenwood_digit(radio_get_mode()));
    cat_send(buf);
    snprintf(log_line, sizeof(log_line), "cat: IF -> sent (freq %d, RIT %+d%s, %s, mode %c)\n",
             freq_hdr, rit, rit_on ? " on" : " (off)", in_tx ? "TX" : "RX", mode_to_kenwood_digit(radio_get_mode()));
    cat_log_get(last, sizeof(last), buf, log_line);
    return;
  }

  // --- AG: AF (volume) gain. Kenwood convention: P1 is a 1-digit VFO
  // selector (minibitx has one VFO, so it's accepted but ignored either
  // way), P2 is a 3-digit level, 000-255. Bare "AG" or "AG0" (no level
  // digits) is a get; "AG0nnn" is a set - this is the exact same volume
  // rigctld's "l"/"L AF" (hamlib.c) already exposes, just reached over
  // the Kenwood-CAT wire format instead of rigctld's own - both paths
  // end up calling rx_audio_set_volume()/rx_audio_get_volume(), so a
  // change made through one is immediately visible through the other.
  // FLRig's own volume slider sends a continuous stream of "AG0nnn;"
  // sets while it's being dragged, one per tick, not just on release -
  // matched here by just applying each one directly (rx_audio_set_volume()
  // is cheap, and rigctld's panel-side "only send on release" throttling
  // was about that panel's own poll-thread traffic, not a real
  // requirement here).
  if (len >= 2 && cmd[0] == 'A' && cmd[1] == 'G') {
    if (len <= 3) {
      // Get - "AG" or "AG0" (a lone VFO digit, no level yet).
      static char last[16] = "";
      char buf[16], log_line[48];
      int percent = rx_audio_get_volume();
      int level255 = (percent * 255 + 50) / 100; // 0-100 -> 0-255, rounded
      if (level255 > 255)
        level255 = 255;
      snprintf(buf, sizeof(buf), "AG0%03d;", level255);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: AG -> %03d (volume %d%%)\n", level255, percent);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      // Set - cmd[2] is the VFO digit (ignored), cmd+3 is the 3-digit
      // level. Tolerant of anything strtol can parse there rather than
      // demanding exactly 3 digits, same spirit as FA's plain strtol use
      // above.
      long level255 = strtol(cmd + 3, NULL, 10);
      if (level255 < 0)
        level255 = 0;
      if (level255 > 255)
        level255 = 255;
      int percent = (int)((level255 * 100 + 127) / 255); // 0-255 -> 0-100, rounded
      rx_audio_set_volume(percent);
      printf("cat: %s -> volume %d%%\n", cmd, percent);
    }
    return;
  }

  // --- PS: power status. Hamlib's kenwood_open() queries this right
  // after ID, and explicitly tolerates a timeout (it just clears its own
  // has_ps flag and carries on), so this is not required for correctness
  // - but a timeout costs the port's full retry budget on every open,
  // for no reason. maxibitx is trivially "on" whenever it is answering
  // CAT at all, so answer honestly and instantly. A set is accepted and
  // ignored: there is no software power switch to throw, and silently
  // shutting the daemon down on a stray "PS0;" would be a nasty
  // surprise. ---
  if (len >= 2 && cmd[0] == 'P' && cmd[1] == 'S') {
    if (len == 2) {
      static char last[8] = "";
      cat_send("PS1;");
      cat_log_get(last, sizeof(last), "PS1;", "cat: PS -> 1 (on)\n");
    } else {
      printf("cat: PS%c -> accepted, ignored (no software power switch)\n", cmd[2]);
    }
    return;
  }

  // --- AI: auto-information. Kenwood rigs can be told to push
  // unsolicited status updates; this surface never does that, so the
  // truthful answer is always 0. Hamlib's kenwood_open() both reads this
  // and may write "AI0;" (ignoring errors either way) - again not
  // required, but answering avoids a pointless timeout on every open.
  // A set to any value is accepted and ignored rather than honored:
  // implementing real auto-reporting would mean pushing status from the
  // CAT thread unprompted, which nothing needs yet. ---
  if (len >= 2 && cmd[0] == 'A' && cmd[1] == 'I') {
    if (len == 2) {
      static char last[8] = "";
      cat_send("AI0;");
      cat_log_get(last, sizeof(last), "AI0;", "cat: AI -> 0 (no auto-information)\n");
    } else {
      printf("cat: AI%c -> accepted, ignored (auto-information not implemented)\n", cmd[2]);
    }
    return;
  }

  // Unknown command - Kenwood radios generally stay silent on anything
  // they don't recognize (no Hamlib-style "unknown command" reply
  // convention exists here), so match that rather than invent one.
  // Logging is still throttled to repeat-suppression (not full
  // silence, unlike AC above) since an unrecognized command here is
  // more likely a genuine gap worth noticing than AC's known-benign
  // poll - but a client stuck retrying the very same unrecognized
  // command every cycle (as FLRig already does for AC, and might for
  // some other command not yet seen) shouldn't get a fresh line every
  // single time either.
  {
    static char last_unrecognized[CAT_LINE_MAX] = "";
    if (strncmp(last_unrecognized, cmd, sizeof(last_unrecognized)) != 0) {
      snprintf(last_unrecognized, sizeof(last_unrecognized), "%s", cmd);
      printf("cat: %s -> unrecognized, ignored\n", cmd);
    }
  }
}

// Puts the ACM tty into raw mode: no line discipline, no echo, one byte
// read at a time (VMIN=1/VTIME=0). Without this, the kernel's tty layer
// applies ordinary canonical-mode line editing/echo to what's actually a
// binary-ish, semicolon-terminated protocol with no real newlines -
// harmless-looking in a first read, but silently wrong, the same class of
// "looks fine, isn't" bug as this file's 0-byte-vs-1-byte UDC unbind write
// earlier in this project.
static void cat_set_raw(int fd) {
  struct termios tio;
  if (tcgetattr(fd, &tio) < 0)
    return;
  cfmakeraw(&tio);
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSANOW, &tio);
}

static void *cat_thread_fn(void *arg) {
  (void)arg;
  char buf[CAT_LINE_MAX];
  size_t buf_len = 0;
  unsigned open_backoff_ms = 10;

  while (cat_running) {
    if (cat_fd < 0) {
      cat_fd = open(CAT_TTY_PATH, O_RDWR | O_NOCTTY);
      if (cat_fd < 0) {
        // Most commonly: the ACM gadget function isn't bound yet
        // (or at all). Back off rather than spin - same lesson as
        // this file's UAC2 writer-thread fix: an absent/not-yet-
        // ready consumer/producer must not cost CPU forever.
        struct timespec ts = {.tv_sec = open_backoff_ms / 1000,
                              .tv_nsec = (open_backoff_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        if (open_backoff_ms < CAT_OPEN_RETRY_MAX_MS)
          open_backoff_ms *= 2;
        continue;
      }
      cat_set_raw(cat_fd);
      open_backoff_ms = 10; // reset now that it's open again
      buf_len = 0;
      printf("cat: %s opened\n", CAT_TTY_PATH);
    }

    char c;
    ssize_t n = read(cat_fd, &c, 1);
    if (n <= 0) {
      // Device gone (gadget unbound/rebound, cable pulled) - close
      // and let the top of the loop reopen it with backoff.
      if (cat_running)
        printf("cat: %s closed, will retry\n", CAT_TTY_PATH);
      close(cat_fd);
      cat_fd = -1;
      continue;
    }

    if (c == ';') {
      buf[buf_len] = '\0';
      cat_handle_command(buf);
      buf_len = 0;
    } else if (c != '\r' && c != '\n' && buf_len + 1 < sizeof(buf)) {
      buf[buf_len++] = c;
    }
    // else: stray CR/LF between commands, or a line too long - drop it
    // silently until the next ';'.
  }

  if (cat_fd >= 0) {
    close(cat_fd);
    cat_fd = -1;
  }
  return NULL;
}

int cat_init(void) {
  cat_running = 1;
  if (pthread_create(&cat_thread_tid, NULL, cat_thread_fn, NULL) != 0) {
    fprintf(stderr, "cat: failed to start reader thread\n");
    cat_running = 0;
    return -1;
  }
  // Detached, not joined - see cat_stop()'s comment; joining this
  // thread was tried and reintroduced §14's unkillable shutdown hang.
  pthread_detach(cat_thread_tid);
  printf("init: CAT (Kenwood TS-480 subset) listening on %s\n", CAT_TTY_PATH);
  return 0;
}

void cat_stop(void) {
  cat_running = 0;
  if (cat_fd >= 0) {
    // Unblock the reader thread's blocking read() - same idea as
    // hamlib_stop() closing listen_fd to unblock accept().
    close(cat_fd);
    cat_fd = -1;
  }

  // Deliberately does NOT pthread_join() the reader thread - an
  // earlier version did, and bench-confirmed it turns into the same
  // unkillable shutdown hang as §14 of docs/dsp_design_notes/usb_gadget_OS_setup.md,
  // just relocated into cat_stop(). Leaving it detached means that
  // hang, if it recurs, hangs alone rather than taking shutdown down
  // with it; §14's rmdir() workaround doesn't depend on this thread
  // having fully exited first, so nothing downstream needs that
  // guarantee.
}
