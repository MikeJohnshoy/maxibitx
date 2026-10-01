// tci_cw.h
//
// CW over TCI (tci_cw.c), and the two senders it borrows from tci.c. Every
// function here is called with tci.c's lock held.
// docs/dsp_design_notes/tci_design_study.md §8.

#ifndef TCI_CW_H
#define TCI_CW_H

// A command whose name (lowercase) is one of TCI's CW text commands:
// cw_macros, cw_msg, cw_macros_stop, cw_terminal, cw_macros_delay, keyer.
// args is everything after the ':' as the client sent it, or NULL. Returns
// 1 if it was one of them (handled), 0 otherwise.
int tci_cw_command(int slot, const char *name, const char *args);

// Every service tick: feeds a cw_msg's callsign to the keyer a character at
// a time, and sends callsign_send and cw_macros_empty when they fall due.
void tci_cw_service(void);

// A client has gone: terminal mode it set ends.
void tci_cw_client_closed(int slot);

// tci.c: send to one client, and to every client.
void tci_send_to(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tci_broadcast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* TCI_CW_H */
