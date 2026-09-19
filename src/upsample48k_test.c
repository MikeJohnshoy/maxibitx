// upsample48k_test.c
//
// Standalone bench harness for upsample48k.c/.h - the first either
// direction (decimation or interpolation) of the 48kHz<->96kHz rate
// converters has had (decim48k.c has none of its own - see
// docs/dsp_design_notes/usb_uac_decimation_design.md). First real
// consumer is sound.c's RADIO_MODE_DIGITAL TX branch (WSJT-X's own
// generated tone, pulled from usb_gadget.c at 48kHz, upsampled here to
// this project's native 96kHz before reaching tx_pipeline.c) - so what
// matters is exactly what a classic zero-stuff-then-lowpass L=2
// interpolator is supposed to get right: passband gain (a steady input
// should reappear at the same amplitude, not attenuated or doubled by
// the zero-stuffing/gain-correction interaction) and imaging rejection
// (the spectral copy zero-stuffing creates near the new Nyquist edge
// should be well attenuated, not passed through into what's now audible
// 96kHz-rate bandwidth).
//
//   make test-upsample48k && ./test-upsample48k
//
// No hardware deps (no ALSA/GPIO/I2C), same "measure before trusting"
// precedent as this project's other bench harnesses.
//
// Case A: DC-equivalent settling - a steady x=1.0 fed into
//   upsample48k_apply() call after call is, from the filter's own point
//   of view, the classic zero-stuffed alternating-impulse-train input
//   (1, 0, 1, 0, ...) an L=2 interpolator always sees for a steady
//   input - after the filter settles, both interleaved output slots
//   should read close to the original 1.0, not attenuated (a design
//   with no gain correction settles near 0.5) or doubled (a design that
//   applies the correction twice settles near 2.0).
//
// Case B: tone reconstruction + imaging rejection - a steady sine tone
//   sampled at the 48kHz input rate should reappear at its own
//   frequency, at roughly unity amplitude, once upsampled to 96kHz; the
//   zero-stuffing image that same tone creates near 48kHz - f should be
//   well attenuated (Goertzel magnitude at both frequencies, measured
//   over a settled window).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "upsample48k.h"

#define FS_OUT 96000.0
#define FS_IN 48000.0

// Single-frequency Goertzel magnitude over an interleaved-stereo-free,
// plain real sample buffer sampled at FS_OUT - used to measure both the
// fundamental and the image frequency in Case B without pulling in a
// full FFT dependency (this harness has no FFTW link, deliberately -
// see the Makefile's test-upsample48k target comment).
static double goertzel_mag(const double *x, int n, double freq_hz, double fs) {
    double w = 2.0 * M_PI * freq_hz / fs;
    double coeff = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double real = s1 - s2 * cos(w);
    double imag = s2 * sin(w);
    return 2.0 * sqrt(real * real + imag * imag) / n;
}

int main(void) {
    printf("upsample48k.c bench harness - 48kHz->96kHz interpolation\n\n");
    int fail = 0;

    // --- Case A: DC-equivalent settling --------------------------------
    {
        struct upsample48k_state f = {0};
        double out[2];
        int blocks = 400; // settles in ~UPSAMPLE48K_TAPS output samples;
                           // 400*2=800 output samples is generous margin
        for (int b = 0; b < blocks; b++)
            upsample48k_apply(&f, 1.0, out);

        printf("A. Steady x=1.0, after settling: out[0]=%.4f out[1]=%.4f "
               "(want both close to 1.0)\n", out[0], out[1]);
        if (fabs(out[0] - 1.0) > 0.03 || fabs(out[1] - 1.0) > 0.03) {
            fprintf(stderr, "   FAIL: not settled near unity gain\n");
            fail = 1;
        }
    }

    // --- Case B: tone reconstruction + imaging rejection ----------------
    {
        double freq_hz = 3000.0;     // an FT8-audio-range tone
        double image_hz = FS_IN - freq_hz; // 45000 Hz - the zero-stuffing image
        int settle_blocks = 200;     // discarded - filter group-delay settle
        int measure_blocks = 4096;   // measured window (48kHz-rate blocks)
        int n_out = measure_blocks * 2;

        struct upsample48k_state f = {0};
        double out[2];
        double phase = 0.0;
        double phase_inc = 2.0 * M_PI * freq_hz / FS_IN;

        for (int b = 0; b < settle_blocks; b++) {
            upsample48k_apply(&f, sin(phase), out);
            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        double *buf = malloc((size_t)n_out * sizeof(double));
        for (int b = 0; b < measure_blocks; b++) {
            upsample48k_apply(&f, sin(phase), out);
            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            buf[b * 2] = out[0];
            buf[b * 2 + 1] = out[1];
        }

        double mag_tone = goertzel_mag(buf, n_out, freq_hz, FS_OUT);
        double mag_image = goertzel_mag(buf, n_out, image_hz, FS_OUT);
        double rejection_db = 20.0 * log10(mag_tone / (mag_image > 1e-12 ? mag_image : 1e-12));

        printf("\nB. %g Hz tone (in at 48kHz, measured at 96kHz):\n"
               "   fundamental magnitude = %.4f (want close to 1.0)\n"
               "   image (%g Hz) magnitude = %.6f\n"
               "   image rejection = %.1f dB (want > 20 dB)\n",
               freq_hz, mag_tone, image_hz, mag_image, rejection_db);

        if (fabs(mag_tone - 1.0) > 0.1) {
            fprintf(stderr, "   FAIL: fundamental not close to unity amplitude\n");
            fail = 1;
        }
        if (rejection_db < 20.0) {
            fprintf(stderr, "   FAIL: insufficient imaging rejection\n");
            fail = 1;
        }

        free(buf);
    }

    if (!fail)
        printf("\nAll cases passed.\n");
    return fail;
}
