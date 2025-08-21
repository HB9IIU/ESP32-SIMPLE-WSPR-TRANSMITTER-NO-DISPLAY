#include <Arduino.h>
#include <Wire.h>
#include <si5351.h>
#include "driver/pcnt.h"

// ----- Pins -----
#define SI5351_SDA   25
#define SI5351_SCL   26
#define CLK1_PIN     34    // Si5351 CLK1 -> GPIO34 (input-only is fine)
#define PPS_PIN      27    // GPS timepulse at 100 Hz -> GPIO27

// ----- Si5351 -----
Si5351 si5351;
static int32_t cal_factor_ppb = 0;
static const uint64_t CLK1_cHz = 300000000ULL; // 1 MHz in centi-Hz

// ----- PCNT -----
#define PCNT_UNIT_USED    PCNT_UNIT_0
#define PCNT_CHANNEL_USED PCNT_CHANNEL_0

// shared between ISR and loop
volatile uint32_t last_count = 0;   // pulses in the last 10 ms gate
volatile bool gate_ready = false;

// 100 Hz timepulse ISR: snapshot & clear for the next 10 ms window
void IRAM_ATTR pps_isr() {
  int16_t c = 0;
  pcnt_get_counter_value(PCNT_UNIT_USED, &c);  // count since last clear
  pcnt_counter_clear(PCNT_UNIT_USED);          // start next 10 ms gate
  last_count = (uint16_t)c;                    // 0..32767 (we expect ~10000)
  gate_ready = true;
}

static void initSI5351() {
  Wire.begin(SI5351_SDA, SI5351_SCL);
  Wire.setClock(400000);

  // optional presence check
  Wire.beginTransmission(0x60);
  if (Wire.endTransmission() != 0) {
    // no prints requested; hang quietly if not found
    while (true) delay(1000);
  }

  si5351.reset();
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  si5351.set_correction(cal_factor_ppb, SI5351_PLL_INPUT_XO);

  // Set CLK1 to 1 MHz and enable (other clocks left off)
  si5351.set_freq(CLK1_cHz, SI5351_CLK1);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_8MA);
  si5351.set_clock_pwr(SI5351_CLK1, 1);
  si5351.set_clock_pwr(SI5351_CLK0, 0);
  si5351.set_clock_pwr(SI5351_CLK2, 0);
}

static void initPCNT() {
  pcnt_config_t cfg = {};
  cfg.pulse_gpio_num = CLK1_PIN;          // count the Si5351 CLK1 here
  cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
  cfg.unit           = PCNT_UNIT_USED;
  cfg.channel        = PCNT_CHANNEL_USED;
  cfg.pos_mode       = PCNT_COUNT_INC;    // rising edges increment
  cfg.neg_mode       = PCNT_COUNT_DIS;    // ignore falling edges
  cfg.lctrl_mode     = PCNT_MODE_KEEP;
  cfg.hctrl_mode     = PCNT_MODE_KEEP;
  cfg.counter_l_lim  = 0;
  cfg.counter_h_lim  = 32767;             // we won't reach this at 1 MHz / 10 ms

  pcnt_unit_config(&cfg);
  pcnt_filter_disable(PCNT_UNIT_USED);    // pass 1 MHz cleanly

  pcnt_counter_pause(PCNT_UNIT_USED);
  pcnt_counter_clear(PCNT_UNIT_USED);
  pcnt_counter_resume(PCNT_UNIT_USED);
}

void setup() {
  Serial.begin(115200);

  initSI5351();
  initPCNT();

  pinMode(PPS_PIN, INPUT);  // use INPUT_PULLUP if your timepulse is open-drain
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), pps_isr, RISING);
}

void loop() {
  if (!gate_ready) return;
  gate_ready = false;

  // Print just the counts per 10 ms gate (should be ~10000 for 1 MHz)
  Serial.println(last_count);
}
