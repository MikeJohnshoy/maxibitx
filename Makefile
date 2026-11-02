CC      := gcc
# maxibitx and its bench harnesses.
#
# -march=native builds for this machine's own CPU (NEON on the Pi): build
# maxibitx on the Pi it will run on, and don't copy the binary to a
# different Pi model (drop -march=native if that's ever needed). It is what
# lets the FIR loops (antialias.c, rx_audio.c's stage 1) vectorize.
# -Isrc/interfaces lets every file include the interface headers
# (src/interfaces/: hpsdr_p1, usb_gadget, iq_stream, hamlib, tci) and the
# core ones (src/) unqualified, wherever it sits. The split is where the
# files sit, not an architectural boundary
# (docs/04_remote_control_and_iq_output.md).
# -lfftw3f: single-precision FFTW, for fft_filter.c's transmit and receive
# filters (fft_filter.h says why single precision).
# The CW keyer: keyer.c offers every mode (straight, bug, ultimatic, iambic
# A/B); `make KEYER=keyer_straight` builds keyer_straight.c instead, the
# straight key alone - which is how the paddle keyer is removed, and the
# interface (keyer.h) a replacement has to meet. Run `make clean` when
# switching.
KEYER   ?= keyer
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces
# A 32-bit ARM OS (armv7l: a Pi 2, 3, 4 or Zero 2W running 32-bit) leaves
# NEON off by default. Every such Pi has NEON-VFPv4, which rx_audio.c's
# stage-1 filter needs to vectorize (docs/dsp_design_notes/zbitx_port_study.md §10).
ifeq ($(shell uname -m),armv7l)
CFLAGS += -mfpu=neon-vfpv4
endif
LDFLAGS := -lm -lasound -lpthread -ldl -lfftw3f
# 32-bit ARM: a 64-bit _Atomic (radio.c, knobs.c, sound.c) compiles to
# inline instructions only when the target is ARMv7 or later. The daemon
# gets that from -march=native; the test harnesses build without it, and
# Raspberry Pi OS's 32-bit gcc defaults to ARMv6, where those become calls
# into libatomic. Linking it costs nothing where it isn't needed.
TEST_LIBS :=
ifeq ($(shell uname -m),armv7l)
LDFLAGS += -latomic
TEST_LIBS += -latomic
endif
SRC := src/maxibitx.c src/radio.c src/radio_hw.c src/interfaces/hpsdr_p1.c src/interfaces/usb_gadget.c src/i2c.c \
     src/si5351v2.c src/sound.c src/vfo.c src/interfaces/hamlib.c src/hw_settings.c src/antialias.c src/decim48k.c src/cw.c \
     src/rx_audio.c src/gpio.c src/interfaces/iq_stream.c src/fft_filter.c src/tx_pipeline.c src/rx_filter.c src/upsample48k.c src/tone_gen.c \
     src/key_input.c src/encoder.c src/knobs.c src/$(KEYER).c src/morse.c src/interfaces/tci.c src/interfaces/tci_stream.c \
     src/interfaces/tci_ws.c src/interfaces/tci_cw.c src/cw_decoder.c src/cw_spot.c src/capture_dump.c src/codec_check.c
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

# Comment policy check (docs/code_comments.md): flags history-style
# comments in files changed vs origin/main, and any source file whose
# header names a different file. Advisory except for the header check.
check-comments:
	python3 tools/check_comments.py

check-comments-all:
	python3 tools/check_comments.py --all

# Verifies src/narrow_filter_bank.h still matches what
# tools/gen_narrow_filters.py designs, and that all 24 sets still pass
# their stability/shape/attack checks. A separate target because it needs
# scipy: a Pi building this needs the generated header, not the design tools.
check-filters:
	python3 tools/gen_narrow_filters.py --check

# Every bench harness, built and run, one line each, with a summary; exits
# non-zero if any fails. First it links the daemon with each keyer, so a
# change to keyer.h that one keyer no longer meets is caught too. Each
# harness's output goes to test-logs/<name>.log, and a failure prints the
# end of its log. Run it before uploading a change; `make` itself doesn't.
TESTS := test-fft-filter test-tx-pipeline test-rx-filter test-rx-audio test-rx-audio-impulse \
	test-upsample48k test-cw test-key-input test-radio test-encoder test-keyer \
	test-keyer-straight test-radio-hw test-tci-ws test-tci test-cw-decoder test-cw-spot test-capture-dump \
	test-codec-check test-i2c test-cw-shape
KEYERS := keyer keyer_straight
OBJ_NO_KEYER := $(filter-out src/$(KEYER).o,$(OBJ))

.PHONY: test
test:
	@$(MAKE) --no-print-directory -s $(TESTS) $(OBJ_NO_KEYER) $(KEYERS:%=src/%.o)
	@mkdir -p test-logs; fail=0; \
	for k in $(KEYERS); do \
		if $(CC) $(OBJ_NO_KEYER) src/$$k.o -o test-logs/maxibitx-$$k $(LDFLAGS) \
			> test-logs/link-$$k.log 2>&1; then r=PASS; else r=FAIL; fail=$$((fail + 1)); fi; \
		printf '  %s  %-24s\n' $$r "link with $$k"; \
		if [ $$r = FAIL ]; then tail -n 15 test-logs/link-$$k.log | sed 's/^/        /'; fi; \
	done; \
	for t in $(TESTS); do \
		start=$$(date +%s%N); \
		if ./$$t > test-logs/$$t.log 2>&1; then r=PASS; else r=FAIL; fail=$$((fail + 1)); fi; \
		tenths=$$(( ($$(date +%s%N) - start) / 100000000 )); \
		printf '  %s  %-24s %3d.%d s\n' $$r $$t $$((tenths / 10)) $$((tenths % 10)); \
		if [ $$r = FAIL ]; then tail -n 15 test-logs/$$t.log | sed 's/^/        /'; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "all passed"; else echo "$$fail FAILED - see test-logs/"; exit 1; fi

clean:
	rm -f $(OBJ) maxibitx test-cw-shape test-fft-filter test-tx-pipeline test-rx-filter test-rx-audio test-rx-audio-impulse test-upsample48k test-cw test-key-input test-keyer test-keyer-straight test-tci-ws test-tci test-radio-hw test-encoder test-radio test-cw-decoder test-cw-spot test-capture-dump test-codec-check test-i2c \
		src/fft_filter.o src/fft_filter_test.o src/tx_pipeline.o src/tx_pipeline_test.o \
		src/rx_filter.o src/rx_filter_test.o src/rx_audio_test.o src/rx_audio_impulse_test.o src/upsample48k_test.o \
		$(KEYERS:%=src/%.o)
	rm -rf test-logs

# fft_filter.c's bench harness (fft_filter_test.c), on synthetic tones.
# Like every test- target below, a program of its own that the daemon
# doesn't link, needing no radio. docs/ARCHITECTURE.md §10 step 2.
test-fft-filter: src/fft_filter.c src/fft_filter_test.c src/fft_filter.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/fft_filter_test.c -o $@ -lfftw3f -lm $(TEST_LIBS)

# tx_pipeline.c's bench harness (tx_pipeline_test.c), with a synthetic
# stand-in for cw.c's sidetone and tone_gen.c's tones. cw.h is for
# CW_PITCH_HZ only, not cw.c (tx_pipeline_test.c says why).
# docs/ARCHITECTURE.md §10 step 4.
test-tx-pipeline: src/tx_pipeline.c src/tx_pipeline_test.c src/tx_pipeline.h src/fft_filter.c src/fft_filter.h src/cw.h src/tone_gen.c src/tone_gen.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/tx_pipeline.c src/tone_gen.c src/tx_pipeline_test.c -o $@ -lfftw3f -lm $(TEST_LIBS)

# rx_filter.c's bench harness (rx_filter_test.c), on synthetic tones
# standing in for rx_audio.c stage 2's demodulated audio. cw.h is for
# CW_PITCH_HZ only. docs/ARCHITECTURE.md §10 step 6.
test-rx-filter: src/rx_filter.c src/rx_filter_test.c src/rx_filter.h src/fft_filter.c src/fft_filter.h src/cw.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/fft_filter.c src/rx_filter.c src/rx_filter_test.c -o $@ -lfftw3f -lm $(TEST_LIBS)

# rx_audio.c's own wiring (rx_audio_test.c): the stage-3 implementation
# selector, the bypass, the fallback for a block that isn't
# RX_FILTER_BLOCK_LEN long - which test-rx-filter, never touching
# rx_audio.c, can't see. vfo.c is linked as it is (no hardware); cw.h is for
# CW_PITCH_HZ only. docs/ARCHITECTURE.md §10 step 7.
test-rx-audio: src/rx_audio.c src/rx_audio_test.c src/rx_audio.h src/vfo.c src/vfo.h src/fft_filter.c src/rx_filter.c src/cw.h src/cw_decoder.c src/cw_decoder.h src/morse.c src/cw_spot.c src/cw_spot.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/rx_audio.c src/vfo.c src/fft_filter.c src/rx_filter.c src/cw_decoder.c src/morse.c src/cw_spot.c src/rx_audio_test.c -o $@ -lfftw3f -lm $(TEST_LIBS)

# Stage 3 and stage 4 together, which neither test-rx-filter (the filter
# alone) nor test-rx-audio (the plumbing) can see: rx_audio.c derives its AGC
# gain from the raw undelayed I/Q and applies it to filtered audio that is one
# group delay behind, so the gain runs ahead of what it modulates - by a
# different amount per filter realization. Measures what that does to an
# interferer the filter removes, and to a keyed CW element's own onset. See
# docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md.
test-rx-audio-impulse: src/rx_audio.c src/rx_audio_impulse_test.c src/rx_audio.h src/vfo.c src/vfo.h src/fft_filter.c src/rx_filter.c src/cw.h src/cw_decoder.c src/cw_decoder.h src/morse.c src/cw_spot.c src/cw_spot.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/rx_audio.c src/vfo.c src/fft_filter.c src/rx_filter.c src/cw_decoder.c src/morse.c src/cw_spot.c src/rx_audio_impulse_test.c -o $@ -lfftw3f -lm $(TEST_LIBS)

# upsample48k.c's bench harness (upsample48k_test.c), on synthetic tones:
# the 48 to 96kHz interpolation the gadget's and TCI's transmit audio go
# through. No hardware, no FFTW. (decim48k.c, its counterpart, has no
# harness: docs/dsp_design_notes/usb_uac_decimation_design.md checks it
# numerically.)
test-upsample48k: src/upsample48k.c src/upsample48k_test.c src/upsample48k.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/upsample48k.c src/upsample48k_test.c -o $@ -lm $(TEST_LIBS)

# cw.c's keying envelope and its 1:1 weighting correction, with the key's
# edges arriving the way they do on the radio: through key_input.c's queue
# at their own timestamps, placed at sample offsets by cw_poll_key(). Checks
# each mark and space at the envelope's 50% points against the time between
# the edges, and TX request and release. Links cw.c, key_input.c, gpio.c
# and vfo.c with the radio calls stubbed (cw_envelope_test.c) - the GPIO
# code is linked but never called, so no hardware or FFTW. See
# docs/dsp_design_notes/cw_keyer_design_study.md §16 and §17.
test-cw: src/cw.c src/cw.h src/cw_envelope_test.c src/key_input.c src/key_input.h src/keyer.c src/keyer.h src/morse.c src/morse.h src/gpio.c src/vfo.c src/vfo.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/cw.c src/key_input.c src/keyer.c src/morse.c src/gpio.c src/vfo.c src/cw_envelope_test.c -o $@ -lm -lpthread $(TEST_LIBS)

# The transmitted CW's keying sidebands: a held dot paddle keyed through
# key_input.c, keyer.c and cw.c into tx_pipeline.c, at full and reduced
# power, its spectrum checked against the CSI mask at 40 WPM
# (cw_shape_test.c; docs/dsp_design_notes/tx_cw_keying_spectrum.md).
test-cw-shape: src/cw.c src/cw.h src/cw_shape_test.c src/key_input.c src/key_input.h src/keyer.c src/keyer.h src/morse.c src/morse.h src/gpio.c src/vfo.c src/vfo.h src/fft_filter.c src/fft_filter.h src/tx_pipeline.c src/tx_pipeline.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/cw.c src/key_input.c src/keyer.c src/morse.c src/gpio.c src/vfo.c src/fft_filter.c src/tx_pipeline.c src/cw_shape_test.c -o $@ -lfftw3f -lm -lpthread $(TEST_LIBS)

# key_input.c's input logic without the GPIO: synthetic edges, bounce and
# plug scenarios fed to the same steps its thread runs, checking debounce,
# mono-plug detection, paddle reversal, the TX-request hook, and how
# key_input_take() places edges in a block. See key_input_test.c.
test-key-input: src/key_input.c src/key_input.h src/key_input_test.c src/gpio.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/key_input.c src/gpio.c src/key_input_test.c -o $@ -lpthread $(TEST_LIBS)

# radio.c's RIT and XIT: what each puts on the si5351's clk2 in receive and
# in transmit, a change mid-transmission, a retune clearing both, and the
# [tx_band] check and the LPF taking the transmit frequency - with the clock
# generator, the board, the codec and the band table stubbed, and the TX
# worker thread running for real. See radio_test.c.
test-radio: src/radio.c src/radio.h src/radio_test.c src/vfo.c src/vfo.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces src/radio.c src/vfo.c src/radio_test.c -o $@ -lm -lpthread $(TEST_LIBS)

# The front-panel knobs: encoder.c's decoding fed synthetic edges - clicks,
# contact bounce, presses and their debounce - and knobs.c's handlers with
# the radio and the speaker stubbed: the tuning step, the speed-up, the
# push through the steps, no tuning in TX, volume and mute. gpio.c is
# linked but never called. See encoder_test.c.
test-encoder: src/encoder.c src/encoder.h src/knobs.c src/knobs.h src/encoder_test.c src/gpio.c src/radio_hw.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/encoder.c src/knobs.c src/gpio.c src/encoder_test.c -o $@ -lpthread $(TEST_LIBS)

# The CW decoder (cw_decoder.c) on synthetic keyed CW in noise: machine and
# hand-sent fists from 5 to 35 WPM, Farnsworth, 6 dB, noise alone, a second
# station after a pause, and the text reader. tools/cw_study/cw_bench.py is
# the full bench.
test-cw-decoder: src/cw_decoder.c src/cw_decoder.h src/cw_decoder_test.c src/morse.c src/morse.h
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/cw_decoder.c src/morse.c src/cw_decoder_test.c -o $@ -lm $(TEST_LIBS)

# SPOT's tone finder (cw_spot.c) on synthetic CW: steady and keyed tones,
# weak ones, two at once, one just outside the band, noise alone, and what
# a search costs.
test-cw-spot: src/cw_spot.c src/cw_spot.h src/cw_spot_test.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/cw_spot.c src/cw_spot_test.c -o $@ -lm $(TEST_LIBS)

# The raw-capture dump (capture_dump.c): SIGUSR1 to the process, a second
# of fed frames read back from the WAV file.
test-capture-dump: src/capture_dump.c src/capture_dump.h src/capture_dump_test.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/capture_dump.c src/capture_dump_test.c -o $@ -lpthread $(TEST_LIBS)

# The codec's start-up sampling check (codec_check.c) against a simulated
# codec; the settle time shortened so the runs are quick.
test-codec-check: src/codec_check.c src/codec_check.h src/codec_check_test.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -DCODEC_CHECK_SETTLE_MS=100 src/codec_check.c src/codec_check_test.c -o $@ -lpthread -lm $(TEST_LIBS)

# i2c.c's bus search, with the device nodes faked and which bus answers
# decided by the harness: no I2C hardware. See i2c_test.c.
test-i2c: src/i2c.c src/i2c.h src/i2c_test.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc src/i2c.c src/i2c_test.c -o $@ $(TEST_LIBS)

# keyer.h, twice: test-keyer against keyer.c - the design study's golden
# cases, random paddle input checked sample for sample against the
# specification model (tools/keyer_study/keyer_spec_model.c, compiled into
# the harness), the speed/mode-change rules, and text (with morse.c) - and
# test-keyer-straight against keyer_straight.c, which must pass every case it
# offers and refuse the rest. See keyer_test.c.
test-keyer: src/keyer.c src/keyer.h src/morse.c src/morse.h src/keyer_test.c src/key_input.h tools/keyer_study/keyer_spec_model.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Itools/keyer_study src/keyer.c src/morse.c src/keyer_test.c -o $@ -lpthread $(TEST_LIBS)

test-keyer-straight: src/keyer_straight.c src/keyer.h src/morse.c src/morse.h src/keyer_test.c src/key_input.h tools/keyer_study/keyer_spec_model.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Itools/keyer_study src/keyer_straight.c src/morse.c src/keyer_test.c -o $@ -lpthread $(TEST_LIBS)

# The board layer: hw_settings.c's sbitx_version and i2c_bus keys, and
# radio_hw.c's board profiles - which pins each board claims and at what
# level, LPF selection (at every retune on the sBitx, at transmit on the
# zBitx) and the relay half of the T/R sequence - with gpio.c and i2c.c
# stubbed. See docs/dsp_design_notes/zbitx_port_study.md §5.
test-radio-hw: src/radio_hw.c src/radio_hw.h src/hw_settings.c src/hw_settings.h src/radio_hw_test.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces src/radio_hw.c src/hw_settings.c src/radio_hw_test.c -o $@ -lm -lpthread $(TEST_LIBS)

# The TCI server, twice, over loopback with no radio. test-tci-ws: tci_ws.c's
# WebSocket server alone - the RFC 6455 accept vector, handshake, framing,
# close, protocol errors, the client limit, the send queue's drop policy.
# test-tci: the whole server with the radio stubbed but the real keyer,
# driven by scripted client sessions (JTDX's and Hamlib's start-up and
# rules, echoes, PTT, every stream format, and CW text read back from what
# the keyer keys). `./test-tci --serve [port]` runs that stubbed server
# with a test tone, for trying a client without a radio. See
# docs/dsp_design_notes/tci_design_study.md §13.
TCI_TEST_HDRS := src/interfaces/tci.h src/interfaces/tci_ws.h src/interfaces/tci_stream.h src/interfaces/ws_test_client.h

test-tci-ws: src/interfaces/tci_ws.c src/interfaces/tci_ws_test.c $(TCI_TEST_HDRS)
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces src/interfaces/tci_ws.c src/interfaces/tci_ws_test.c -o $@ -lpthread $(TEST_LIBS)

test-tci: src/interfaces/tci.c src/interfaces/tci_stream.c src/interfaces/tci_ws.c src/interfaces/tci_cw.c src/interfaces/tci_test.c src/keyer.c src/morse.c src/keyer.h src/morse.h src/interfaces/tci_cw.h $(TCI_TEST_HDRS)
	$(CC) -O2 -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces src/interfaces/tci.c src/interfaces/tci_stream.c src/interfaces/tci_ws.c src/interfaces/tci_cw.c src/keyer.c src/morse.c src/interfaces/tci_test.c -o $@ -lpthread -lm $(TEST_LIBS)
