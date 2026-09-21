// hamlib.h - minimal rigctld-compatible TCP control server.
//
// A small subset of Hamlib's plain-text rigctld protocol, one thread per
// client:
//    f/F            get/set frequency
//    t/T            get/set PTT
//    m/M            get/set mode (DIGITAL goes out as PKTUSB)
//    j/J            get/set RIT offset
//    l/L AF         get/set volume, 0.0-1.0
//    l STRENGTH     S-meter, dB relative to S9 (read-only)
//    l/L MICGAIN    get/set TX mic gain (extension, not a Hamlib level)
//    u/U NARROW     get/set the narrow RX filter (extension)
//    u/U FFTFILT    get/set its FFT implementation (extension)
//    v/V, chk_vfo   single VFO
//    dump_state
//    q/Q/quit       close the connection
// Anything else gets RPRT -1.

#ifndef HAMLIB_H
#define HAMLIB_H

// Starts the server on port (4532 is rigctld's standard port) in a
// background thread. Returns 0 on success, -1 on a bind/listen error -
// not fatal; maxibitx keeps running either way.
int hamlib_init(int port);

// Stops the server and closes the listening socket.
void hamlib_stop(void);

#endif /* HAMLIB_H */
