// si5351.h - si5351 clock generator control (si5351v2.c).

#ifndef SI5351_H
#define SI5351_H

#include <stdint.h>

void si5351_set_calibration(int32_t cal);
// Brings up the I2C bus the si5351 is on, resets it and stops all three
// clocks.
void si5351bx_init(int i2c_bus);
void si5351bx_setfreq(uint8_t clknum, uint32_t fout);
void si5351_reset();
void si5351a_clkoff(uint8_t clk);

// The output drive strength of every clock from the next
// si5351bx_setfreq() on: 2, 4, 6 or 8 mA (8 at start-up). Weaker drive
// means slower edges and less of the clocks' harmonics, and less LO level
// at the mixers. Returns 0, or -1 for any other value, unchanged.
int si5351_set_drive_ma(int ma);
int si5351_get_drive_ma(void);

#endif /* SI5351_H */
