CC      := gcc
# -march=native detects this machine's actual CPU (NEON on the Pi) at
# build time - maxibitx should be built directly on the Pi
# it'll run on, but a binary built this way
# shouldn't be copied to a different Pi model; drop -march=native if
# that's ever needed. It's what lets antialias.c's branch-free FIR loop
# (see antialias.c) actually vectorize instead of just being eligible to.
# -Isrc/interfaces (alongside the implicit "search the including file's
# own directory" gcc already does for quoted #includes) is what lets
# src/interfaces/*.c keep saying #include "cw.h"/"rx_audio.h" etc.
# unqualified even though those headers live one directory up in plain
# src/ - and lets sound.c/maxibitx.c (in src/) keep saying
# #include "hpsdr_p1.h" etc. unqualified even though those headers moved
# into src/interfaces/. Added when hpsdr_p1.c/usb_gadget.c/iq_stream.c/
# hamlib.c (the four modules that talk to an external SDR app - HPSDR
# Protocol 1, the UAC2/CAT USB gadget, the lightweight I/Q telemetry
# stream, and the rigctld server) moved out of plain src/ into their own
# subfolder, to keep them visually separate from the DSP/radio-control
# core (sound.c, rx_audio.c, radio.c, cw.c, vfo.c, ...) - see
# docs/04_remote_control_and_iq_output.md's intro. Not a real
# architectural boundary (nothing stops a src/interfaces file from
# including a core header or vice versa, same as before) - just where
# the file sits.
#
# This Makefile, and everything under src/ it builds, was carried over
# unchanged from minibitx as maxibitx's step-1 bootstrap (repo skeleton
# only - see docs/ARCHITECTURE.md). The only maxibitx-specific change
# here is the output binary's name; src/maxibitx.c is minibitx's
# src/minibitx.c, renamed to match this project, not yet rewritten.
# -lfftw3f: single-precision FFTW, needed since docs/ARCHITECTURE.md
# build order step 5 wired src/tx_pipeline.c/src/fft_filter.c (the
# shared FFT TX pipeline, step 4) into sound.c for real, live CW TX -
# see fft_filter.h/tx_pipeline.h for why fftwf (not the double-precision
# fftw3) specifically.
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces
LDFLAGS := -lm -lasound -lpthread -ldl -lfftw3f
SRC := src/maxibitx.c src/radio.c src/radio_hw.c src/interfaces/hpsdr_p1.c src/interfaces/usb_gadget.c src/i2c.c \
     src/si5351v2.c src/sound.c src/vfo.c src/interfaces/hamlib.c src/hw_settings.c src/antialias.c src/decim48k.c src/cw.c \
     src/rx_audio.c src/gpio.c src/interfaces/iq_stream.c src/fft_filter.c src/tx_pipeline.c
OBJ := $(SRC:.c=.o)

all: maxibitx

maxibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)
	# Grant capabilities so maxibitx doesn't need to run as root:
	#  - cap_sys_nice:      lets the audio thread get SCHED_FIFO
	#  - cap_dac_override:  bypasses the normal file-permission check so
	#    usb_gadget.c can mkdir/write under the root-owned
	#    /sys/kernel/config/usb_gadget/ configfs tree
	-sudo setcap cap_sys_nice,cap_dac_override+ep $@

clean:
	rm -f $(OBJ) maxibitx test-fft-filter test-tx-pipeline test-rx-filter \
		src/fft_filter.o src/fft_filter_test.o src/tx_pipeline.o src/tx_pipeline_test.o \
		src/rx_filter.o src/rx_filter_test.o

# docs/ARCHITECTURE.md step 2: fft_filter.c/.h's own standalone bench
# harness (fft_filter_test.c) against synthetic tones - fft_filter.c
# itself is real, shipped code now (part of $(SRC)/$(OBJ) above, since
# step 5), but this harness stays separate, same "not part of the
# build" convention docs/dsp_design_notes/rx_audio_demod_design.md
# describes for test_rx_audio.c: a verification tool, not something the
# shipped maxibitx binary links.
test-fft-filter: src/fft_filter.c src/fft_filter_test.c src/fft_filter.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/fft_filter_test.c -o $@ -lfftw3f -lm

# docs/ARCHITECTURE.md step 4: tx_pipeline.c/.h's own standalone bench
# harness (tx_pipeline_test.c) against a synthetic stand-in for cw.c's
# sidetone - tx_pipeline.c itself is real, shipped code now (part of
# $(SRC)/$(OBJ) above, wired into sound.c/cw.c for live CW TX as of
# step 5), but this harness stays separate, same "not part of the
# build" convention as test-fft-filter above. Depends on fft_filter.c/.h
# (step 2) and cw.h (CW_PITCH_HZ only - NOT cw.c itself, see
# tx_pipeline_test.c's header comment for why).
test-tx-pipeline: src/tx_pipeline.c src/tx_pipeline_test.c src/tx_pipeline.h src/fft_filter.c src/fft_filter.h src/cw.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/tx_pipeline.c src/tx_pipeline_test.c -o $@ -lfftw3f -lm

# docs/ARCHITECTURE.md step 6: rx_filter.c/.h's own standalone bench
# harness (rx_filter_test.c) against synthetic tones, standing in for
# rx_audio.c stage 2's real demodulated audio - rx_filter.c is NOT yet
# real, shipped code (not part of $(SRC)/$(OBJ) above - unlike
# fft_filter.c/tx_pipeline.c, this hasn't had its "step 5"-equivalent
# live-wiring step yet, see ARCHITECTURE.md §10 step 6/7), so this stays
# a pure bench-only harness for now, same convention as the other two
# targets above. Depends on fft_filter.c/.h (step 2, including its new
# filter_tune_real()) and cw.h (CW_PITCH_HZ only - NOT cw.c/rx_audio.c
# themselves, same "no hardware deps in a bench test" precedent).
test-rx-filter: src/rx_filter.c src/rx_filter_test.c src/rx_filter.h src/fft_filter.c src/fft_filter.h src/cw.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/rx_filter.c src/rx_filter_test.c -o $@ -lfftw3f -lm
