// capture_dump.h
//
// A diagnostic: on SIGUSR1 (`pkill -USR1 maxibitx`), one second of the
// codec's raw capture - both channels, exactly as snd_pcm_readi() delivered
// them, before any processing - is written to a WAV file, for looking at
// what the ADC actually gives when the receiver misbehaves.
// tools/capture_check.py reads it. Does nothing until asked; while a
// second is being recorded or written, further signals wait.

#ifndef CAPTURE_DUMP_H
#define CAPTURE_DUMP_H

#include <stdint.h>

#define CAPTURE_DUMP_SECONDS 1

// main(), first, before any thread starts: blocks SIGUSR1 in every thread,
// so it interrupts no system call anywhere (a read on a socket or the
// codec); capture_dump_init()'s thread takes it with sigwait() instead.
void capture_dump_block_signal(void);

// Starts the thread that waits for SIGUSR1 and writes the files into dir
// ("/tmp" in maxibitx). Returns 0, or -1 if it can't (the dump is then
// unavailable; nothing else is affected).
int capture_dump_init(const char *dir, int sample_rate, int channels);

// The audio thread, after every snd_pcm_readi(): frames interleaved
// frames of int32. Copies them while a recording is in progress; never
// blocks, allocates or does I/O.
void capture_dump_block(const int32_t *interleaved, int frames);

// What to say about the radio in the console line written with the file
// (sound.c's). Called on the writer thread. May be NULL.
void capture_dump_set_describer(void (*describe)(char *out, int max));

#endif /* CAPTURE_DUMP_H */
