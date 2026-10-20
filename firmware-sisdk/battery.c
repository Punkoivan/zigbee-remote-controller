#include "battery.h"
#include "sl_gpio.h"
#include "sl_clock_manager.h"
#include "sl_sleeptimer.h"
#include "em_iadc.h"

static const sl_gpio_t DIVIDER_ENABLE = { SL_GPIO_PORT_D, 3 };

// Typical Li-Po open-circuit discharge curve: {millivolts, percent}, descending
static const uint16_t CURVE[][2] = {
  { 4200, 100 }, { 4100, 90 }, { 4020, 80 }, { 3950, 70 }, { 3870, 60 }, { 3840, 50 },
  { 3800, 40 }, { 3770, 30 }, { 3730, 20 }, { 3690, 10 }, { 3610, 5 }, { 3300, 0 },
};

#define AVDD_MV     3300
#define ADC_FULL    4095
#define BURSTS      3
#define SAMPLES     8

void battery_init(void)
{
  sl_gpio_set_pin_mode(&DIVIDER_ENABLE, SL_GPIO_MODE_PUSH_PULL, false);
}

static uint32_t adc_burst(void)
{
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    IADC_command(IADC0, iadcCmdStartSingle);
    while ((IADC0->STATUS & IADC_STATUS_SINGLEFIFODV) == 0) {
    }
    sum += IADC_pullSingleFifoResult(IADC0).data;
  }
  return sum / SAMPLES;
}

uint16_t battery_read_mv(void)
{
  sl_gpio_set_pin(&DIVIDER_ENABLE);
  sl_clock_manager_enable_bus_clock(SL_BUS_CLOCK_IADC0);

  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t configs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t single = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t input = IADC_SINGLEINPUT_DEFAULT;
  init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 10000000, 0);
  configs.configs[0].reference = iadcCfgReferenceVddx;
  configs.configs[0].vRef = AVDD_MV;
  configs.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(IADC0, 10000000, 0, iadcCfgModeNormal,
                                                              init.srcClkPrescale);
  input.posInput = iadcPosInputPortDPin4;
  input.negInput = iadcNegInputGnd;
  GPIO->CDBUSALLOC |= GPIO_CDBUSALLOC_CDEVEN0_ADC0;
  IADC_init(IADC0, &init, &configs);
  IADC_initSingle(IADC0, &single, &input);

  sl_sleeptimer_delay_millisecond(10);  // divider settles
  uint32_t best = 0;
  for (int b = 0; b < BURSTS; b++) {
    uint32_t v = adc_burst();
    if (v > best) {
      best = v;
    }
    sl_sleeptimer_delay_millisecond(5);
  }

  IADC_reset(IADC0);
  GPIO->CDBUSALLOC &= ~GPIO_CDBUSALLOC_CDEVEN0_ADC0;
  sl_clock_manager_disable_bus_clock(SL_BUS_CLOCK_IADC0);
  sl_gpio_clear_pin(&DIVIDER_ENABLE);

  return (uint16_t)(best * 2 * AVDD_MV / ADC_FULL);
}

uint8_t battery_percent(uint16_t mv)
{
  const int n = sizeof(CURVE) / sizeof(CURVE[0]);
  if (mv >= CURVE[0][0]) {
    return 100;
  }
  for (int i = 1; i < n; i++) {
    if (mv >= CURVE[i][0]) {
      uint32_t v_hi = CURVE[i - 1][0], p_hi = CURVE[i - 1][1];
      uint32_t v_lo = CURVE[i][0], p_lo = CURVE[i][1];
      return (uint8_t)(p_lo + ((mv - v_lo) * (p_hi - p_lo) + (v_hi - v_lo) / 2) / (v_hi - v_lo));
    }
  }
  return 0;
}
