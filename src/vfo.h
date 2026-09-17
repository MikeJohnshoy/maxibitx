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
int  vfo_read(struct vfo *v);
void vfo_read_iq(struct vfo *v, int *out_i, int *out_q);

#endif // VFO_H