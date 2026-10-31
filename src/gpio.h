// gpio.h
//
// Direct-to-kernel GPIO access via the Linux GPIO character-device ioctl
// API (uAPI v2, <linux/gpio.h>): the kernel's own supported interface, no
// vendor library (wiringPi, which maxibitx once used, is unmaintained) -
// as i2c.c talks to /dev/i2c-N directly.
//
// One line request (and one open file descriptor) per GPIO line, each
// pin its own handle. The exception is the key jack: key_input.c takes
// both of its contacts in a single request with edge events
// (gpio_request_edge_inputs()), so every edge on either contact arrives,
// kernel-timestamped, on one fd.
//
// All offsets passed in here are BCM GPIO numbers - the kernel's own
// numbering, and what /dev/gpiochip0 expects - not wiringPi's pin numbers.
// radio_hw.h's BCM numbers come from the boards' wiring
// (docs/01_hardware_init_and_control.md, "Migrating off wiringPi: BCM pin
// mapping"); another board's need deriving the same way, not assuming.

#ifndef GPIO_H
#define GPIO_H

// Requests bcm_gpio as an output line, driven immediately to
// initial_value (0 or nonzero) as part of the same request - there's no
// separate "set mode, then write" step the way wiringPi needed.
// consumer_label shows up in `gpioinfo` and /sys/kernel/debug/gpio, so a
// pin can be identified without cross-referencing this source.
//
// Returns an opaque line handle (an fd, but callers should treat it as
// opaque) to pass to gpio_write(), or -1 on failure (already logged its
// own reason - most commonly /dev/gpiochip0 missing, or the offset
// already in use by something else).
int gpio_request_output(unsigned int bcm_gpio, int initial_value,
                         const char *consumer_label);

// Requests bcm_gpio as an input line, with the SoC's internal pull-up
// enabled if pull_up is nonzero (matches wiringPi's PUD_UP). Returns an opaque line handle (>= 0), or
// -1 on failure (already logged).
int gpio_request_input(unsigned int bcm_gpio, int pull_up,
                        const char *consumer_label);

// Requests n input lines (BCM numbers in bcm_gpio[]) in one request, pull-ups
// on, reporting both edges on each as events: read() the returned fd for
// struct gpio_v2_line_event (<linux/gpio.h>), each stamped by the kernel
// with CLOCK_MONOTONIC at the interrupt. No kernel debounce - its debounced
// events carry the time the debounce period ended, not the edge's
// (key_input.c debounces instead). event_buffer is the kernel's queue depth
// in events. Returns the fd, or -1 on failure (already logged).
int gpio_request_edge_inputs(const unsigned int *bcm_gpio, int n, int event_buffer,
                             const char *consumer_label);

// Reads every line of a gpio_request_edge_inputs() request at once, as a
// bitmask: bit i is line i's level, in the order they were requested.
// Returns -1 on failure.
int gpio_read_lines(int line, int n);

// Drives a line previously returned by gpio_request_output() high
// (nonzero) or low (0). Returns 0 on success, -1 on failure (logs its
// own reason; a line handle from a failed request, or an otherwise
// invalid one, safely returns -1 rather than touching an arbitrary fd).
int gpio_write(int line, int value);

// Reads a line's current level (0 or 1). Returns -1 on failure, same
// invalid-handle safety as gpio_write().
int gpio_read(int line);

#endif /* GPIO_H */
