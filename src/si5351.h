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

// clk1's output divider, from the next si5351bx_setfreq(1, ...) on. 0, the
// start-up setting, picks it as for every clock: the smallest even divider
// putting the PLL above 650 MHz - 18 for the sBitx's 40 MHz clk1, so PLL B
// runs at about 720 MHz, inside the 650-780 MHz PLL A (clk2's) covers over
// the bands, and the two PLLs' difference sweeps through HF. 16, 18, 20 or
// 22 fixes it, moving PLL B to about 640, 720, 800 or 880 MHz; one that
// would put PLL B outside the si5351's 600-900 MHz is passed over for the
// automatic one. Returns 0, or -1 for any other value, unchanged.
int si5351_set_clk1_divider(int div);
int si5351_get_clk1_divider(void); // 0 for automatic
// The divider si5351bx_setfreq() uses for clk1 at frequency.
int si5351_clk1_divider_for(uint32_t frequency);

#endif /* SI5351_H */
