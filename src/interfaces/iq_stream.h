// iq_stream.h
//
// A minimal, protocol-free UDP stream of raw baseband I/Q sample pairs,
// so a simple network client can get real I/Q without implementing
// openHPSDR Protocol 1 (hpsdr_p1.c) or TCI, with their discovery,
// start/stop and command machinery. Its clients are tools/rigctl_panel.py's
// spectrum and the measuring tools (xtal_sweep.py, lpf_sweep.py,
// spur_survey.py). iq_stream.c's file header has the wire format;
// docs/dsp_design_notes/iq_stream_design.md why it is a separate path.
//
// Like the other I/Q consumers, it is fed its own copy of the I/Q from
// sound_process() and knows nothing about them, so it runs alongside an
// SDR app on hpsdr_p1.c or a TCI client without disturbing either.

#ifndef IQ_STREAM_H
#define IQ_STREAM_H

#define IQ_STREAM_PORT 4536

// Call once at startup (after hpsdr_init(), order doesn't otherwise
// matter - the two are fully independent). Returns 0 on success, -1 if
// the UDP socket couldn't be bound (not fatal to the caller - same
// "keep running on whatever subset of control surfaces came up"
// convention every other optional surface here follows).
int iq_stream_init(void);

// Starts the listener thread (catches subscribe datagrams) and the
// pacer thread (paces outbound packets to each active subscriber).
// Mirrors hpsdr_poll()'s job for its own two threads.
void iq_stream_poll(void);

// Call once per audio-thread block from sound_process(), same samples
// handed to hpsdr_send_iq() - producer side only, appends to a lock-free
// ring buffer and returns immediately; a no-op the instant no subscriber
// is active. Must stay fast and never block - runs on the real-time
// audio thread.
void iq_stream_send(const double *i_samples, const double *q_samples, int n);

// Stops both threads and closes the socket. Call after sound_thread_stop()
// so nothing is still trying to produce samples into a torn-down stream,
// same ordering hpsdr_stop() already follows.
void iq_stream_stop(void);

#endif /* IQ_STREAM_H */
