// iface_status.h - one interface's state, as rigctld's \interfaces reports
// it (hamlib.c). Each interface fills one in about itself: tci.c,
// hpsdr_p1.c, iq_stream.c and usb_gadget.c.

#ifndef IFACE_STATUS_H
#define IFACE_STATUS_H

enum iface_state {
  IFACE_OFF,     // turned off by a setting
  IFACE_DOWN,    // meant to run, but didn't start
  IFACE_WAITING, // running, no client
  IFACE_ACTIVE,  // running, with a client
};

// detail: one line for a person - who is connected and what they're
// doing, or why it isn't running. It never contains ';' or '|', which
// \interfaces uses as separators.
#define IFACE_DETAIL_LEN 160
struct iface_status {
  enum iface_state state;
  char detail[IFACE_DETAIL_LEN];
};

#endif /* IFACE_STATUS_H */
