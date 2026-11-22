// si5351_test.c
//
// Bench check of si5351v2.c's frequency arithmetic, with the I2C writes
// captured rather than sent:
//
//   A. every clk1 and clk2 frequency the sBitx uses lands within half a
//      step of the one asked for (the PLL fraction rounded, not truncated),
//      and the registers AN619 asks for give back exactly that fraction;
//   B. the errors average to zero, where truncating would leave them all
//      below;
//   C. a fraction that rounds up to a whole one carries into the multiplier;
//   D. si5351bx_setfreq() writes PLL B with the 20-bit denominator and the
//      fraction si5351_pll_for() worked out.
//
//   make test-si5351 && ./test-si5351
//
// Links si5351v2.c with i2c.h's calls stubbed below; no hardware.

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "i2c.h"
#include "si5351.h"

#define XTAL_CENTER 40012400u
#define CAL 24999986u // Mike's sBitx

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

// ---- I2C, captured ------------------------------------------------------------

static uint8_t reg[256];
int32_t i2c_write_byte_data(uint8_t address, uint8_t command, uint8_t value) {
  (void)address;
  reg[command] = value;
  return 0;
}
int i2c_find_bus(int preferred, uint8_t address) {
  (void)address;
  return preferred;
}
void i2c_init(int bus) { (void)bus; }

// ---- helpers ------------------------------------------------------------------

// The output divider si5351bx_setfreq() picks for clk2: the smallest even
// one putting the PLL above 650 MHz.
static uint32_t divider_for(uint32_t f) {
  uint32_t d = 650000000u / f + 1;
  return (d & 1) ? d + 1 : d;
}

// a + b/c back from AN619's registers.
static double from_registers(uint32_t p1, uint32_t p2, uint32_t p3) {
  return (p1 + 512.0 + (double)p2 / p3) / 128.0;
}

struct sweep {
  double worst_over_half_step; // largest |error| / half a step
  double mean_steps;           // mean error, in steps
  int bad_registers;
  long n;
};

static struct sweep sweep(uint32_t lo, uint32_t hi, uint32_t stride, uint32_t ref) {
  struct sweep s = {0};
  double sum = 0;
  for (uint32_t f = lo; f <= hi; f += stride) {
    uint32_t div = divider_for(f), a, b, p1, p2, p3;
    si5351_pll_for(f, div, ref, &a, &b);
    double step = (double)ref / SI5351_PLL_DENOM / div;
    double err = si5351_actual_hz(f, div, ref) - f;
    if (fabs(err) / (step / 2) > s.worst_over_half_step)
      s.worst_over_half_step = fabs(err) / (step / 2);
    sum += err / step;
    si5351_registers(a, b, SI5351_PLL_DENOM, &p1, &p2, &p3);
    double back = from_registers(p1, p2, p3);
    if (b >= SI5351_PLL_DENOM || (b && p2 >= p3) || p1 >= (1u << 18) ||
        fabs(back - (a + (double)b / SI5351_PLL_DENOM)) > 1e-12 * back)
      s.bad_registers++;
    s.n++;
  }
  s.mean_steps = sum / s.n;
  return s;
}

int main(void) {
  char msg[200];

  printf("A. every frequency within half a step, registers exact\n");
  // clk1 (receive and transmit, divider 18) every hertz over 40.00-40.10 MHz;
  // clk2 (dial + filter centre) every 7 Hz from 1.8 to 30 MHz dials.
  struct sweep c1 = sweep(40000000u, 40100000u, 1, CAL);
  struct sweep c2 = sweep(1800000u + XTAL_CENTER, 30000000u + XTAL_CENTER, 7, CAL);
  snprintf(msg, sizeof msg, "clk1, %ld settings: worst %.3f of half a step, registers all exact",
           c1.n, c1.worst_over_half_step);
  check(c1.worst_over_half_step <= 1.0 + 1e-6 && c1.bad_registers == 0, msg);
  snprintf(msg, sizeof msg, "clk2, %ld settings: worst %.3f of half a step, registers all exact",
           c2.n, c2.worst_over_half_step);
  check(c2.worst_over_half_step <= 1.0 + 1e-6 && c2.bad_registers == 0, msg);

  printf("B. errors centred on zero\n");
  snprintf(msg, sizeof msg, "mean error clk1 %+.4f, clk2 %+.4f steps (truncating gives -0.5)",
           c1.mean_steps, c2.mean_steps);
  check(fabs(c1.mean_steps) < 0.02 && fabs(c2.mean_steps) < 0.02, msg);

  printf("C. a fraction rounding up to a whole one\n");
  {
    // 69,999,999 x 10 is 10 Hz under 28 x 25 MHz: rounds up to exactly 28.
    uint32_t a, b;
    si5351_pll_for(69999999u, 10, 25000000u, &a, &b);
    check(a == 28 && b == 0, "carried into the multiplier: 28 + 0");
    check(fabs(si5351_actual_hz(69999999u, 10, 25000000u) - 70000000.0) < 1e-6,
          "comes out 1 Hz high, under half the 2.4 Hz step");
  }

  printf("D. what si5351bx_setfreq() writes\n");
  {
    si5351_set_calibration((int32_t)CAL);
    si5351bx_setfreq(1, 40036407u);
    // PLL B's registers from 34: P3[15:8], P3[7:0], P1[17:16], P1[15:8],
    // P1[7:0], P3[19:16]|P2[19:16], P2[15:8], P2[7:0].
    uint32_t p3 = ((uint32_t)(reg[39] >> 4) << 16) | ((uint32_t)reg[34] << 8) | reg[35];
    uint32_t p1 = ((uint32_t)(reg[36] & 3) << 16) | ((uint32_t)reg[37] << 8) | reg[38];
    uint32_t p2 = ((uint32_t)(reg[39] & 15) << 16) | ((uint32_t)reg[40] << 8) | reg[41];
    uint32_t a, b;
    si5351_pll_for(40036407u, (uint32_t)si5351_clk1_divider_for(40036407u), CAL, &a, &b);
    check(p3 == SI5351_PLL_DENOM, "PLL B's denominator is 1,048,575");
    check(fabs(from_registers(p1, p2, p3) - (a + (double)b / SI5351_PLL_DENOM)) < 1e-12,
          "its registers hold si5351_pll_for()'s fraction");
    double err = si5351_actual_hz(40036407u, 18, CAL) - 40036407.0;
    snprintf(msg, sizeof msg, "clk1 at 40,036,407 Hz comes out %+.2f Hz off (step %.2f Hz)", err,
             (double)CAL / SI5351_PLL_DENOM / 18);
    check(fabs(err) <= (double)CAL / SI5351_PLL_DENOM / 18 / 2, msg);
  }

  printf("\nworst error by band, clk2 at dial + %u Hz:\n", XTAL_CENTER);
  static const struct { const char *name; uint32_t lo, hi; } bands[] = {
      {"80 m", 3500000, 4000000},   {"40 m", 7000000, 7300000},   {"30 m", 10100000, 10150000},
      {"20 m", 14000000, 14350000}, {"17 m", 18068000, 18168000}, {"15 m", 21000000, 21450000},
      {"12 m", 24890000, 24990000}, {"10 m", 28000000, 29700000}};
  for (unsigned i = 0; i < sizeof bands / sizeof bands[0]; i++) {
    double worst = 0;
    for (uint32_t d = bands[i].lo; d <= bands[i].hi; d += 1) {
      uint32_t f = d + XTAL_CENTER;
      double e = fabs(si5351_actual_hz(f, divider_for(f), CAL) - f);
      if (e > worst)
        worst = e;
    }
    printf("  %-5s divider %2u: within %.2f Hz\n", bands[i].name,
           divider_for(bands[i].lo + XTAL_CENTER), worst);
  }

  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
