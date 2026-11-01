// si5351v2.c
//
// The si5351 clock generator: clk2 (the first LO, dial + xtal_filter_center)
// on PLL A, clk1 (the second LO in receive, bfo_freq in transmit) on
// PLL B; clk0 is off. Each output runs on an even integer divider, its
// PLL a 25MHz multiple plus a fraction in 1/2^19 steps
// (set_freq_fixeddiv()). Register formulas: SiLabs AN619.

#include <stdio.h>
#include <linux/types.h>
#include <stdint.h>
#include <unistd.h>     // usleep()
#include "i2c.h"
#include "si5351.h"

// The I2C bus is found by si5351bx_init(), starting from the board's
// (radio_hw_i2c_bus()).

#define SI_CLK0_CONTROL  16      // Register definitions
#define SI_CLK1_CONTROL 17
#define SI_CLK2_CONTROL 18
#define SI_SYNTH_PLL_A  26
#define SI_SYNTH_PLL_B  34
#define SI_SYNTH_MS_0   42
#define SI_SYNTH_MS_1   50
#define SI_SYNTH_MS_2   58
#define SI_PLL_RESET    177

#define SI_R_DIV_1    0b00000000      // R-division ratio definitions
#define SI_R_DIV_2    0b00010000
#define SI_R_DIV_4    0b00100000
#define SI_R_DIV_8    0b00110000
#define SI_R_DIV_16   0b01000000
#define SI_R_DIV_32   0b01010000
#define SI_R_DIV_64   0b01100000
#define SI_R_DIV_128    0b01110000

#define SI_CLK_SRC_PLL_A  0b00000000
#define SI_CLK_SRC_PLL_B  0b00100000

#define SI5351_CLK_PLL_SELECT_B (1<<5) 
#define SI5351_CLK_INTEGER_MODE (1<<6)   
#define SI5351_CLK_INPUT_MULTISYNTH_N  (3<<2)

#define SI5351_CLK_DRIVE_STRENGTH_MASK    (3<<0)
#define SI5351_CLK_DRIVE_STRENGTH_2MA   (0<<0)
#define SI5351_CLK_DRIVE_STRENGTH_4MA   (1<<0)
#define SI5351_CLK_DRIVE_STRENGTH_6MA   (2<<0)
#define SI5351_CLK_DRIVE_STRENGTH_8MA   (3<<0)

#define PLL_N 35
#define PLLFREQ (xtal_freq_calibrated * PLL_N)
int xtal_freq_calibrated = 25000000; // the TCXO's frequency, Hz (si5351_set_calibration())

uint32_t plla_freq, pllb_freq;

static int i2c_error_count = 0;       // counts I2C Errors

// Every clock's drive strength, written with its control register by
// setup_multisynth().
static uint8_t drive_strength = SI5351_CLK_DRIVE_STRENGTH_8MA;

// clk1's output divider, 0 for the automatic one (si5351.h).
static int clk1_divider = 0;

#define SI_VCO_MIN_HZ 600000000u
#define SI_VCO_MAX_HZ 900000000u

#define SI5351_ADDR 0x60              // I2C address of Si5351   (typical)

#define I2C_SEND_MAX_ATTEMPTS 20   // ~20ms worst case (1ms delay/attempt) before giving up

void i2cSendRegister(uint8_t reg, uint8_t val){
  int attempts = 0;
  while (i2c_write_byte_data(SI5351_ADDR, reg, val) < 0)
  {
    printf("Repeating I2C #%d\n",i2c_error_count++);  // reports number of I2C repeats caused by errors
    attempts++;
    if (attempts >= I2C_SEND_MAX_ATTEMPTS) {
      printf("i2cSendRegister: giving up on si5351 reg 0x%02x after %d attempts "
             "- check SDA/SCL pull-ups and wiring\n", reg, attempts);
      return;   // give up on this write rather than hang forever
    }
    usleep(1000);
  }
}

// Resets both PLLs (register 177), so they relock to their settings.
void si5351_reset(){
  i2cSendRegister(SI_PLL_RESET, 0xA0);  
}

void si5351a_clkoff(uint8_t clk)
{
  i2cSendRegister(clk, 0x80);   // Refer to SiLabs AN619 to see bit values - 0x80 turns off the output stage
}

/*
  Follow the AN619 application note for the Si5351
  a = mult
  b = num
  c = denom
*/

static void setup_pll(uint8_t pll, uint8_t mult, uint32_t num, uint32_t denom)
{
  uint32_t P1;          // PLL config register P1
  uint32_t P2;          // PLL config register P2
  uint32_t P3;          // PLL config register P3

  if (num == 0){
    P1 = 128 * mult - 512;
    P2 = 0;
    P3 = 1;    
  }
  else {
    P1 = (uint32_t)(128 * ((float)num / (float)denom));
    P1 = (uint32_t)(128 * (uint32_t)(mult) + P1 - 512);
    P2 = (uint32_t)(128 * ((float)num / (float)denom));
    P2 = (uint32_t)(128 * num - denom * P2);
    P3 = denom;
  }
  i2cSendRegister(pll + 0, (P3 & 0x0000FF00) >> 8);
  i2cSendRegister(pll + 1, (P3 & 0x000000FF));
  i2cSendRegister(pll + 2, (P1 & 0x00030000) >> 16);
  i2cSendRegister(pll + 3, (P1 & 0x0000FF00) >> 8);
  i2cSendRegister(pll + 4, (P1 & 0x000000FF));
  i2cSendRegister(pll + 5, ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16));
  i2cSendRegister(pll + 6, (P2 & 0x0000FF00) >> 8);
  i2cSendRegister(pll + 7, (P2 & 0x000000FF));
}

static void setup_multisynth(uint8_t clk, uint8_t pllSource, uint32_t divider,  uint32_t num, uint32_t denom, uint32_t rdiv,  uint8_t drive_strength){
  uint8_t synth, control;

  switch (clk){
    case 0:
      synth = 42;
      control = 16;
      break;
    case 1:
      synth = 50;
      control =17;
      break;
    //clock 2
    default:
      synth = 58;
      control = 18;
      break;   
  }
  
  uint8_t dat;

  uint32_t P1;
  uint32_t P2;
  uint32_t P3;
  uint32_t div4 = 0;


  /* Output Multisynth Divider Equations
   * where: a = div, b = num and c = denom
   * P1 register is an 18-bit value using following formula:
   *  P1[17:0] = 128 * a + floor(128*(b/c)) - 512
   * P2 register is a 20-bit value using the following formula:
   *  P2[19:0] = 128 * b - c * floor(128*(b/c))
   * P3 register is a 20-bit value using the following formula:
   *  P3[19:0] = c
   */
  /* Set the main PLL config registers */
 #define SI5351_DIVBY4     (3<<2) 
  
  if (divider == 4) {
    div4 = SI5351_DIVBY4;
    P1 = P2 = 0;
    P3 = 1;
  } else if (num == 0) {
    /* Integer mode */
    P1 = 128 * divider - 512;
    P2 = 0;
    P3 = 1;
  } else {
    /* Fractional mode */
    P1 = 128 * divider + ((128 * num) / denom) - 512;
    P2 = 128 * num - denom * ((128 * num) / denom);
    P3 = denom;
  }

  i2cSendRegister(synth + 0,   (P3 & 0x0000FF00) >> 8);
  i2cSendRegister(synth + 1,   (P3 & 0x000000FF));
  i2cSendRegister(synth + 2,   ((P1 & 0x00030000) >> 16) | div4 | rdiv);
  i2cSendRegister(synth + 3,   (P1 & 0x0000FF00) >> 8);
  i2cSendRegister(synth + 4,   (P1 & 0x000000FF));
  i2cSendRegister(synth + 5,   ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16));
  i2cSendRegister(synth + 6,   (P2 & 0x0000FF00) >> 8);
  i2cSendRegister(synth + 7,   (P2 & 0x000000FF));

/* clock control register
 *  |    7    |    6    |    5    |    4    |   3    |  2   |   1   |  0   |
 *   pwr off?   ms_int?   pll b?    o/p inv? <--  source --> <-- drive ---->
 */

  /* Configure the clk control and enable the output */
  dat = drive_strength | SI5351_CLK_INPUT_MULTISYNTH_N;
  if (pllSource == SI_SYNTH_PLL_B)
    dat |= SI_CLK_SRC_PLL_B;
  if (num == 0)
    dat |= SI5351_CLK_INTEGER_MODE;
  i2cSendRegister(control, dat);
}

// frequency on clk, from pll at frequency x divider: the PLL a whole
// multiple of the TCXO plus num/2^19 of it, the output multisynth an
// integer divider.
static void set_freq_fixeddiv(int clk, int pll, uint32_t frequency, int divider,  uint8_t drive_strength){
  int32_t denom = 0x80000;
  int32_t pllfreq = frequency * divider;
  int32_t multi = pllfreq / xtal_freq_calibrated;
  int32_t num = ((uint64_t)(pllfreq % xtal_freq_calibrated) * 0x80000)/xtal_freq_calibrated;

  setup_pll(pll, multi, num, denom);
  setup_multisynth(clk, pll, divider, 0, 1, SI_R_DIV_1, drive_strength);
}

// The smallest even divider above 650 MHz / frequency, so the PLL runs
// above 650 MHz. Unsigned 32-bit arithmetic: 650,000,000 fits, and a long
// is only 32 bits on a 32-bit OS.
static uint32_t automatic_divider(uint32_t frequency){
  uint32_t pll_div = 650000000u / frequency + 1;
  if (pll_div & 1)
    pll_div++;
  return pll_div;
}

int si5351_clk1_divider_for(uint32_t frequency){
  if (clk1_divider) {
    uint64_t vco = (uint64_t)frequency * clk1_divider;
    if (vco >= SI_VCO_MIN_HZ && vco <= SI_VCO_MAX_HZ)
      return clk1_divider;
  }
  return automatic_divider(frequency);
}

void si5351bx_setfreq(uint8_t clk, uint32_t frequency){
  int pll;
  uint32_t pll_div;

  if (clk == 1) {
    pll = SI_SYNTH_PLL_B;
    pll_div = si5351_clk1_divider_for(frequency);
  } else {
    pll = SI_SYNTH_PLL_A;
    pll_div = automatic_divider(frequency);
  }

   set_freq_fixeddiv(clk, pll, frequency, pll_div, drive_strength);
}

int si5351_set_drive_ma(int ma){
  switch (ma) {
  case 2: drive_strength = SI5351_CLK_DRIVE_STRENGTH_2MA; return 0;
  case 4: drive_strength = SI5351_CLK_DRIVE_STRENGTH_4MA; return 0;
  case 6: drive_strength = SI5351_CLK_DRIVE_STRENGTH_6MA; return 0;
  case 8: drive_strength = SI5351_CLK_DRIVE_STRENGTH_8MA; return 0;
  default: return -1;
  }
}

int si5351_get_drive_ma(void){
  return 2 + 2 * (drive_strength & SI5351_CLK_DRIVE_STRENGTH_MASK);
}

int si5351_set_clk1_divider(int div){
  if (div != 0 && div != 16 && div != 18 && div != 20 && div != 22)
    return -1;
  clk1_divider = div;
  return 0;
}

int si5351_get_clk1_divider(void){
  return clk1_divider;
}

void si5351_set_calibration(int32_t cal){
    xtal_freq_calibrated = cal;
}

int si5351bx_init(int preferred_bus){
  int i2c_bus = i2c_find_bus(preferred_bus, SI5351_ADDR);
  if (i2c_bus < 0) {
    printf("init: no si5351 answers at 0x%02x on any I2C bus - trying bus %d anyway; "
           "check the board's I2C overlay (i2cdetect -l)\n", SI5351_ADDR, preferred_bus);
    i2c_bus = preferred_bus;
  } else if (i2c_bus != preferred_bus) {
    printf("init: no si5351 on I2C bus %d, but one answers on bus %d - using that "
           "(i2c_bus = %d in hw_settings.ini skips the search)\n",
           preferred_bus, i2c_bus, i2c_bus);
  }
  i2c_init(i2c_bus);
	usleep(10000);
  si5351_reset();
	usleep(10000);
  si5351a_clkoff(SI_CLK0_CONTROL);
  si5351a_clkoff(SI_CLK1_CONTROL);
  si5351a_clkoff(SI_CLK2_CONTROL);
  return i2c_bus;
}
