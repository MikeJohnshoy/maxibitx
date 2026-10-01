// tci.h
//
// A TCI server (Expert Electronics' Transceiver Control Interface, v2.0):
// control, receive and transmit audio, and I/Q, over one WebSocket
// connection per client - for JTDX, WSJT-X Improved, MSHV, loggers and
// Hamlib's TCI backend. Several clients at once, kept in step: a change
// made by any of them, or anywhere else in maxibitx, is sent to all.
// docs/dsp_design_notes/tci_design_study.md.

#ifndef TCI_H
#define TCI_H

#define TCI_DEFAULT_PORT 50001 // Hamlib's and ExpertSDR3's default
#define TCI_DEFAULT_MAX_CLIENTS 4

// Starts the server on bind_addr (dotted IPv4, or "" for every interface)
// and port. Returns 0, or -1 if it can't listen - not fatal to maxibitx.
int tci_init(const char *bind_addr, int port, int max_clients);

// Disconnects every client (releasing a TX one of them holds) and stops.
// Call after sound_thread_stop().
void tci_stop(void);

// ---- Audio thread ---------------------------------------------------------
//
// None of these blocks, locks or allocates; each push returns at once when
// no client has started that stream.

// Demodulated receive audio at 48 kHz, in rx_audio.c's uac_out units (the
// samples uac_push_audio_rx() gets).
void tci_push_audio_rx(const double *samples, int n);

// The 96 kHz I/Q block hpsdr_send_iq() and iq_stream_send() get, as
// sound_process() makes it (spectrally inverted; TCI clients get it
// conjugated, the conventional way up).
void tci_push_iq(const double *i_samples, const double *q_samples, int n);

// Nonzero while a TCI client holds TX with its own audio (trx with source
// "tci"): the transmit audio then comes from tci_pull_audio_tx() rather
// than the mic or the USB gadget.
int tci_tx_audio_owned(void);

// Up to n transmit samples at 48 kHz, about +-1 full scale - the same
// contract as uac_pull_audio_tx(): fewer (0 if none) rather than waiting.
int tci_pull_audio_tx(double *out, int n);

// Discards queued transmit audio. The audio thread calls it on every block
// that isn't sending TCI audio, so a transmission never starts with audio
// left from an earlier one.
void tci_tx_audio_idle(void);

#endif /* TCI_H */
