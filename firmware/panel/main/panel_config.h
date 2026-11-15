#pragma once

/* Entrance panel configuration: Kconfig bools as 0/1 constants and compile-time checks of
 * firmware/panel/main/Kconfig.projbuild (I-49, I-66). The shared checks are in doorbell_config_checks.h. */
#include "doorbell_config_checks.h"

#if CONFIG_DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW
#define DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW 1
#else
#define DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW 0
#endif

#if CONFIG_DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW
#define DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW 1
#else
#define DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW 0
#endif

#if CONFIG_DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL
#define DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL 1
#else
#define DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL 0
#endif

static_assert(CONFIG_DOORBELL_PANEL_MIN_APT_NO >= 1,
              "Lowest apartment number must be >= 1");
static_assert(CONFIG_DOORBELL_PANEL_MAX_APT_NO <= 99,
              "Highest apartment number must be <= 99 for 24 KB NVS budget");
static_assert(CONFIG_DOORBELL_PANEL_MIN_APT_NO <= CONFIG_DOORBELL_PANEL_MAX_APT_NO,
              "Lowest apartment number must be <= Highest apartment number");
static_assert(CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN >= 8 && CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN <= 10,
              "Admin code length must be between 8 and 10 digits (WPA3-SAE minimum is 8)");
static_assert(CONFIG_DOORBELL_PANEL_USER_CODE_LEN >= 4 && CONFIG_DOORBELL_PANEL_USER_CODE_LEN <= 8,
              "User code length must be between 4 and 8 digits");
static_assert(sizeof(CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE) - 1 == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN,
              "Default admin code must have exactly 'Admin code length' digits, or it can never authenticate");
static_assert(sizeof(CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE) - 1 == CONFIG_DOORBELL_PANEL_USER_CODE_LEN,
              "Default user code must have exactly 'User code length' digits, or it can never authenticate");
static_assert(CONFIG_DOORBELL_PANEL_LOCKOUT_FIRST_S <= CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S,
              "First lockout duration must be <= maximum lockout duration");
static_assert(CONFIG_DOORBELL_PANEL_RESET_MIN_HOLD_S < CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S,
              "Reset button minimum hold must be strictly less than maximum hold");
static_assert(CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S <= 65,
              "Panel reset button maximum hold must fit within uint16_t ms (<= 65 s)");
static_assert(sizeof(CONFIG_DOORBELL_PANEL_KEY_MAP) - 1 == 12,
              "Keypad key map must be exactly 12 characters");
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_DOOR_RELAY),
              "Panel door relay GPIO must be a valid output GPIO");
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_STATUS_LED),
              "Panel status LED GPIO must be a valid output GPIO");
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW0) &&
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW1) &&
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW2) &&
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW3),
              "Panel keypad row GPIOs must be valid output GPIOs");
static_assert(GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL0) &&
              GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL1) &&
              GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL2) &&
              GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_RESET_BTN),
              "Panel input GPIOs must be valid GPIOs");
/* Classic ESP32 GPIO 34-39 are input-only and have no internal pull-up/down (ESP32 datasheet). Every other valid
 * GPIO can drive an output, so GPIO_IS_VALID_OUTPUT_GPIO() tells whether the internal pull resistors exist. */
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL0) &&
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL1) &&
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL2),
              "Keypad column GPIOs need the internal pull-down that espressif/button's matrix enables: GPIO 34-39 "
              "have none");
static_assert(!DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL ||
              GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_RESET_BTN),
              "GPIO 34-39 have no internal pull resistor: disable the reset button's internal pull (fit an external "
              "one) or choose another GPIO");
