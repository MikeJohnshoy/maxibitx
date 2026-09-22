// si5351.h - si5351 clock generator control (si5351v2.c).

#ifndef SI5351_H
#define SI5351_H

#include <stdint.h>

void si5351_set_calibration(int32_t cal);
void si5351bx_init(); 
void si5351bx_setfreq(uint8_t clknum, uint32_t fout);
void si5351_reset();
void si5351a_clkoff(uint8_t clk);

#endif /* SI5351_H */
