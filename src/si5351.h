// si5351.h - si5351 clock generator control (si5351v2.c).

#ifndef SI5351_H
#define SI5351_H

#include <stdint.h>

void si5351_set_calibration(int32_t cal);
int32_t si5351_get_calibration(void); // the reference frequency in use, Hz
// Finds the si5351 - on preferred_bus, the board's usual bus or
// hw_settings.ini's i2c_bus, or failing that on whichever other bus it
// answers - brings that bus up, resets it and stops all three clocks.
// Returns the bus it is using; says on the console when that isn't
// preferred_bus, or when no bus has it (preferred_bus is used anyway, and
// every write to it will fail).
int si5351bx_init(int preferred_bus);
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

// ---- The arithmetic, for the bench harness (si5351_test.c) -----------------

// The PLL fraction's denominator, the largest the 20-bit P3 register holds.
// A PLL step is then the TCXO / SI5351_PLL_DENOM, about 23.8 Hz, and an
// output's that divided by its divider: 1.3 Hz for the sBitx's clk1 (18),
// 1.5 to 2.4 Hz for clk2 over the bands (dividers 16 to 10).
#define SI5351_PLL_DENOM 1048575u

// The PLL setting for frequency out of an output divider: frequency x
// divider = ref_hz x (mult + num / SI5351_PLL_DENOM), num rounded to the
// nearest, so the output lands within half a step of frequency, either side.
void si5351_pll_for(uint32_t frequency, uint32_t divider, uint32_t ref_hz, uint32_t *mult,
                    uint32_t *num);

// What frequency actually comes out as, from si5351_pll_for(), with the
// TCXO at ref_hz.
double si5351_actual_hz(uint32_t frequency, uint32_t divider, uint32_t ref_hz);

// AN619's register values for a + b/c (a PLL's multiplier or a
// multisynth's divider): P1 = 128a + floor(128b/c) - 512,
// P2 = 128b - c floor(128b/c), P3 = c; integer mode (P2 0, P3 1) for b 0.
void si5351_registers(uint32_t a, uint32_t b, uint32_t c, uint32_t *p1, uint32_t *p2,
                      uint32_t *p3);

#endif /* SI5351_H */
