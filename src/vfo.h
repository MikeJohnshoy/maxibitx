// vfo.h - table-driven numerically controlled oscillators (vfo.c).

#ifndef VFO_H
#define VFO_H

#define MAX_PHASE_COUNT (16385)

struct vfo {
    int freq_hz;
    int phase;
    int phase_increment;
};

void vfo_init_phase_table(void);
void vfo_start(struct vfo *v, int frequency_hz, int start_phase);
// The frequency vfo_start() actually sets for frequency_hz: its phase
// increment back in Hz, a step of sampling_freq/65536 at or below it.
double vfo_actual_hz(int frequency_hz);
int  vfo_read(struct vfo *v);
void vfo_read_iq(struct vfo *v, int *out_i, int *out_q);

#endif // VFO_H
