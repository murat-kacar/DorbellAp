#pragma once

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"

#define DOORBELL_FW_VERSION_MAJOR 1
#define DOORBELL_FW_VERSION_MINOR 0

/* Tick duration in milliseconds from FreeRTOS configuration */
#define DOORBELL_TICK_MS (1000 / CONFIG_FREERTOS_HZ)

/* Bool Kconfig options as 0/1 C constants. sdkconfig.h omits a bool that is set to n, so a bare CONFIG_X used as
 * a C value fails to compile when the option is unchecked; only `#if CONFIG_X` treats a missing symbol as 0. */
#if CONFIG_DOORBELL_LED_ACTIVE_HIGH
#define DOORBELL_LED_ACTIVE_HIGH 1
#else
#define DOORBELL_LED_ACTIVE_HIGH 0
#endif

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

/* Pairing completion budget (I-30, I-45): after PAIR_CONFIRM the module must join the building AP (all-channel scan,
 * association, DHCP) and still have time for 4 PAIR_COMPLETE attempts + 500 ms margin (unit main.c). Require at least
 * 5 s for the LAN join so a larger ACK timeout cannot silently make every pairing fail. */
#define DOORBELL_PAIR_LAN_JOIN_MIN_MS 5000
_Static_assert((CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000) >=
                   (4 * CONFIG_DOORBELL_ACK_TIMEOUT_MS + 500 + DOORBELL_PAIR_LAN_JOIN_MIN_MS),
               "Pairing completion timeout must cover 5 s LAN join + 4 x ACK timeout + 500 ms");

/* Compile-time validation of shared LED pattern timing vs FreeRTOS tick (I-66) */
_Static_assert(CONFIG_DOORBELL_LED_SHORT_FLASH_MS >= DOORBELL_TICK_MS,
               "Short flash duration must be at least one FreeRTOS tick");
_Static_assert((CONFIG_DOORBELL_LED_FAST_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
               "Fast blink half-period must be at least one FreeRTOS tick");
_Static_assert((CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
               "Medium blink half-period must be at least one FreeRTOS tick");
_Static_assert((CONFIG_DOORBELL_LED_SLOW_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
               "Slow blink half-period must be at least one FreeRTOS tick");
_Static_assert(CONFIG_DOORBELL_LED_MULTI_PULSE_MS >= DOORBELL_TICK_MS,
               "Multi-flash pulse must be at least one FreeRTOS tick");
_Static_assert(CONFIG_DOORBELL_LED_MULTI_GAP_MS >= DOORBELL_TICK_MS,
               "Multi-flash gap must be at least one FreeRTOS tick");
_Static_assert(CONFIG_DOORBELL_LED_MULTI_PERIOD_MS > (3 * CONFIG_DOORBELL_LED_MULTI_PULSE_MS + 2 * CONFIG_DOORBELL_LED_MULTI_GAP_MS),
               "Multi-flash repeat period must exceed 3 pulses + 2 gaps");
_Static_assert(CONFIG_DOORBELL_LED_SPARSE_PERIOD_MS > CONFIG_DOORBELL_LED_MULTI_PULSE_MS,
               "Sparse flash period must exceed pulse duration");
_Static_assert(CONFIG_DOORBELL_LED_DIM_BRIGHTNESS_PCT < CONFIG_DOORBELL_LED_FULL_BRIGHTNESS_PCT,
               "Dim brightness must be strictly less than full brightness");
_Static_assert(CONFIG_DOORBELL_LED_ERR_DURATION_MS >= CONFIG_DOORBELL_LED_FAST_PERIOD_MS,
               "Error indication must last at least one fast blink period");
_Static_assert(CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS >= CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS,
               "No-response indication must last at least one medium blink period");

/* Compile-time validation of Panel configuration constraints (I-49, I-66) */
#if defined(CONFIG_DOORBELL_ROLE_PANEL)
_Static_assert(CONFIG_DOORBELL_PANEL_MIN_APT_NO >= 1,
               "Lowest apartment number must be >= 1");
_Static_assert(CONFIG_DOORBELL_PANEL_MAX_APT_NO <= 99,
               "Highest apartment number must be <= 99 for 24 KB NVS budget");
_Static_assert(CONFIG_DOORBELL_PANEL_MIN_APT_NO <= CONFIG_DOORBELL_PANEL_MAX_APT_NO,
               "Lowest apartment number must be <= Highest apartment number");
_Static_assert(CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN >= 8 && CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN <= 10,
               "Admin code length must be between 8 and 10 digits (WPA3-SAE minimum is 8)");
_Static_assert(CONFIG_DOORBELL_PANEL_USER_CODE_LEN >= 4 && CONFIG_DOORBELL_PANEL_USER_CODE_LEN <= 8,
               "User code length must be between 4 and 8 digits");
_Static_assert(sizeof(CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE) - 1 == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN,
               "Default admin code must have exactly 'Admin code length' digits, or it can never authenticate");
_Static_assert(sizeof(CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE) - 1 == CONFIG_DOORBELL_PANEL_USER_CODE_LEN,
               "Default user code must have exactly 'User code length' digits, or it can never authenticate");
_Static_assert(CONFIG_DOORBELL_PANEL_LOCKOUT_FIRST_S <= CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S,
               "First lockout duration must be <= maximum lockout duration");
_Static_assert(CONFIG_DOORBELL_PANEL_RESET_MIN_HOLD_S < CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S,
               "Reset button minimum hold must be strictly less than maximum hold");
_Static_assert(CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S <= 65,
               "Panel reset button maximum hold must fit within uint16_t ms (<= 65 s)");
_Static_assert(sizeof(CONFIG_DOORBELL_PANEL_KEY_MAP) - 1 == 12,
               "Keypad key map must be exactly 12 characters");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_DOOR_RELAY),
               "Panel door relay GPIO must be a valid output GPIO");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_STATUS_LED),
               "Panel status LED GPIO must be a valid output GPIO");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW0) &&
               GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW1) &&
               GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW2) &&
               GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_PANEL_GPIO_ROW3),
               "Panel keypad row GPIOs must be valid output GPIOs");
_Static_assert(GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL0) &&
               GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL1) &&
               GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_COL2) &&
               GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_PANEL_GPIO_RESET_BTN),
               "Panel input GPIOs must be valid GPIOs");
#endif

/* Compile-time validation of Apartment Module configuration constraints (I-49, I-66) */
#if defined(CONFIG_DOORBELL_ROLE_UNIT)
_Static_assert(CONFIG_DOORBELL_UNIT_DOOR_BTN_MIN_MS < (CONFIG_DOORBELL_UNIT_DOOR_BTN_MAX_S * 1000),
               "Door button minimum press must be less than maximum press");
_Static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S < CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S,
               "Pairing/reset button minimum hold must be strictly less than maximum hold");
_Static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S < CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S,
               "Pairing/reset button maximum hold must be strictly less than cancel-after threshold");
_Static_assert(CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S <= 65,
               "Pairing/reset button cancel-after threshold must fit within uint16_t ms (<= 65 s)");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_BELL_RELAY),
               "Unit bell relay GPIO must be a valid output GPIO");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DOORBELL_UNIT_GPIO_STATUS_LED),
               "Unit status LED GPIO must be a valid output GPIO");
_Static_assert(GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_DOOR_BTN) &&
               GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_PAIR_BTN) &&
               GPIO_IS_VALID_GPIO(CONFIG_DOORBELL_UNIT_GPIO_RESET_BTN),
               "Unit button GPIOs must be valid GPIOs");
#endif

