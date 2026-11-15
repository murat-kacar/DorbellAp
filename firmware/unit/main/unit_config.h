#pragma once

/* Apartment module configuration: Kconfig bools as 0/1 constants and compile-time checks of
 * firmware/unit/main/Kconfig.projbuild. The shared checks are in doorbell_config_checks.h. */
#include "doorbell_config_checks.h"

#if CONFIG_DOORBELL_UNIT_BTNS_ACTIVE_LOW
#define DOORBELL_UNIT_BTNS_ACTIVE_LOW 1
#else
#define DOORBELL_UNIT_BTNS_ACTIVE_LOW 0
#endif

#if CONFIG_DOORBELL_UNIT_BTNS_INTERNAL_PULL
#define DOORBELL_UNIT_BTNS_INTERNAL_PULL 1
#else
#define DOORBELL_UNIT_BTNS_INTERNAL_PULL 0
#endif

#if CONFIG_DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH
#define DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH 1
#else
#define DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH 0
#endif

static_assert(CONFIG_DOORBELL_UNIT_DOOR_BTN_MIN_MS < (CONFIG_DOORBELL_UNIT_DOOR_BTN_MAX_S * 1000),
              "Door button minimum press must be less than maximum press");
static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S < CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S,
              "Pairing/reset button minimum hold must be strictly less than maximum hold");
static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S < CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S,
              "Pairing/reset button maximum hold must be strictly less than cancel-after threshold");
static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S <= 65,
              "Pairing/reset button cancel-after threshold must fit within uint16_t ms (<= 65 s)");
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_BELL_RELAY),
              "Unit bell relay GPIO must be a valid output GPIO");
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_STATUS_LED),
              "Unit status LED GPIO must be a valid output GPIO");
static_assert(GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_DOOR_BTN) &&
              GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_PAIR_BTN) &&
              GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_RESET_BTN),
              "Unit button GPIOs must be valid GPIOs");
static_assert(!DOORBELL_UNIT_BTNS_INTERNAL_PULL ||
              (GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_DOOR_BTN) &&
               GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_PAIR_BTN) &&
               GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_RESET_BTN)),
              "GPIO 34-39 have no internal pull resistor: disable the buttons' internal pull (fit external ones) or "
              "choose other GPIOs");
