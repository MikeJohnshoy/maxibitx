// hpsdr_p1.h - the openHPSDR Protocol 1 server (hpsdr_p1.c).

#ifndef HPSDR_P1_H
#define HPSDR_P1_H

#include <stdint.h>
#include "iface_status.h"

#define HPSDR_PORT 1024
#define HPSDR_PKT_SIZE 1032
#define SAMPLES_PER_PACKET 126

int  hpsdr_init(void);
void hpsdr_stop(void);
void hpsdr_send_iq(double *i_samples, double *q_samples, int n);
void hpsdr_poll(void);
int  hpsdr_is_connected(void);
// For rigctld's \interfaces: listening, or streaming to a client's address.
void hpsdr_status(struct iface_status *s);

#endif // HPSDR_P1_H
