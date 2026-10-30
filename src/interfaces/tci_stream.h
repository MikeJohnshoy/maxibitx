// tci_stream.h
//
// tci_stream.c's side facing tci.c: per-client stream settings, the
// transmit-audio exchange (TX_CHRONO out, TX audio in), and the service
// step that turns the audio thread's rings into frames. tci.c serializes
// every call here with its own lock; the audio thread's side is tci.h.

#ifndef TCI_STREAM_H
#define TCI_STREAM_H

#include <stddef.h>
#include <stdint.h>

// Stream.type and Stream.format, TCI v2.0 §3.4.
enum tci_stream_type {
  TCI_IQ_STREAM = 0,
  TCI_RX_AUDIO_STREAM = 1,
  TCI_TX_AUDIO_STREAM = 2,
  TCI_TX_CHRONO = 3,
};
enum tci_sample_type { TCI_INT16 = 0, TCI_INT24 = 1, TCI_INT32 = 2, TCI_FLOAT32 = 3 };

#define TCI_HEADER_BYTES 64
#define TCI_CHRONO_FRAMES 1024 // stereo frames asked for per TX_CHRONO (length 2048)

struct tci_stream_cfg {
  int audio_on;
  int audio_rate;     // 8000, 12000, 24000 or 48000
  int audio_type;     // enum tci_sample_type
  int audio_channels; // 1 or 2 (the same sample on both)
  int audio_samples;  // Stream.length per frame, 100-2048; 0 = the rate's default
  int iq_on;
  int iq_rate;        // 48000 or 96000
};

void tci_stream_init(void);
void tci_stream_open(int slot, int ws_id);
void tci_stream_close(int slot);
void tci_stream_set(int slot, const struct tci_stream_cfg *cfg);

// TX audio from a client: begin makes it the source tci_pull_audio_tx()
// reads and starts sending it TX_CHRONO frames, keeping buffering_ms
// requested ahead (250 ms at least); end stops both. drain stops the
// requests and leaves what is queued for the audio thread to play, before
// an end; queued_frames is how much that is (48 kHz frames).
void tci_stream_tx_begin(int slot, int buffering_ms);
void tci_stream_tx_end(void);
void tci_stream_tx_drain(void);
long tci_stream_tx_queued_frames(void);

// How the last transmission's TCI audio kept up, filled in by
// tci_stream_tx_end(), which prints it when the audio ran dry. dry_events:
// times the audio thread found the ring short once audio had started and
// before it came back; silence_ms: what was played as silence then;
// max_gap_ms: the longest wait between two of the client's TX audio frames;
// first_audio_ms: from TX on to the first, -1 if none came; held_ms: how
// long TX stayed up after the client's trx false, playing out what was
// queued; unplayed_ms: what was still queued when TX dropped.
struct tci_tx_stats {
  unsigned long dry_events;
  double silence_ms, max_gap_ms, first_audio_ms, held_ms, unplayed_ms;
};
void tci_stream_tx_stats(struct tci_tx_stats *s);

// A binary frame from a client. TX audio from the client that holds TX is
// queued for the audio thread; anything else is dropped.
void tci_stream_tx_frame(int slot, const uint8_t *frame, size_t len);

// Called every few milliseconds: sends each client the audio and I/Q frames
// that have accumulated, and TX_CHRONO frames as the audio thread uses up
// transmit audio.
void tci_stream_service(void);

#endif /* TCI_STREAM_H */
