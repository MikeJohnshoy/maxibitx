/* Stand-in for wiringPi.h, so modem_cw.c builds off the radio. */
#pragma once
#define LOW 0
#define HIGH 1
static inline int digitalRead(int pin) { (void)pin; return HIGH; }
unsigned int millis(void);
