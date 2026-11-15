#pragma once

/* Checks of the settings shared by both roles (components/doorbell_common/Kconfig.projbuild); the role checks are in
 * firmware/<role>/main/<role>_config.h */

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"

/* DOORBELL_FW_VERSION_MAJOR / _MINOR are compile definitions derived from PROJECT_VER (firmware/version.txt) by
 * components/doorbell_common/CMakeLists.txt */
#if !defined(DOORBELL_FW_VERSION_MAJOR) || !defined(DOORBELL_FW_VERSION_MINOR)
#error "DOORBELL_FW_VERSION_MAJOR/MINOR missing: doorbell_common derives them from PROJECT_VER"
#endif

/* Tick duration in milliseconds from FreeRTOS configuration */
#define DOORBELL_TICK_MS (1000 / CONFIG_FREERTOS_HZ)

/* Bool Kconfig options as 0/1 C constants. sdkconfig.h omits a bool that is set to n, so a bare CONFIG_X used as
 * a C value fails to compile when the option is unchecked; only `#if CONFIG_X` treats a missing symbol as 0. */
#if CONFIG_DOORBELL_LED_ACTIVE_HIGH
#define DOORBELL_LED_ACTIVE_HIGH 1
#else
#define DOORBELL_LED_ACTIVE_HIGH 0
#endif

/* Pairing completion budget: after PAIR_CONFIRM the module must join the building AP (all-channel scan,
 * association, DHCP) and still have time for 4 PAIR_COMPLETE attempts + 500 ms margin (unit main.c). Require at least
 * 5 s for the LAN join so a larger ACK timeout cannot silently make every pairing fail. */
#define DOORBELL_PAIR_LAN_JOIN_MIN_MS 5000
static_assert((CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000) >=
              (4 * CONFIG_DOORBELL_ACK_TIMEOUT_MS + 500 + DOORBELL_PAIR_LAN_JOIN_MIN_MS),
              "Pairing completion timeout must cover 5 s LAN join + 4 x ACK timeout + 500 ms");

/* Compile-time validation of shared LED pattern timing vs FreeRTOS tick */
static_assert(CONFIG_DOORBELL_LED_SHORT_FLASH_MS >= DOORBELL_TICK_MS,
              "Short flash duration must be at least one FreeRTOS tick");
static_assert((CONFIG_DOORBELL_LED_FAST_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
              "Fast blink half-period must be at least one FreeRTOS tick");
static_assert((CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
              "Medium blink half-period must be at least one FreeRTOS tick");
static_assert((CONFIG_DOORBELL_LED_SLOW_PERIOD_MS / 2) >= DOORBELL_TICK_MS,
              "Slow blink half-period must be at least one FreeRTOS tick");
static_assert(CONFIG_DOORBELL_LED_MULTI_PULSE_MS >= DOORBELL_TICK_MS,
              "Multi-flash pulse must be at least one FreeRTOS tick");
static_assert(CONFIG_DOORBELL_LED_MULTI_GAP_MS >= DOORBELL_TICK_MS,
              "Multi-flash gap must be at least one FreeRTOS tick");
static_assert(CONFIG_DOORBELL_LED_MULTI_PERIOD_MS >
              (3 * CONFIG_DOORBELL_LED_MULTI_PULSE_MS + 2 * CONFIG_DOORBELL_LED_MULTI_GAP_MS),
              "Multi-flash repeat period must exceed 3 pulses + 2 gaps");
static_assert(CONFIG_DOORBELL_LED_SPARSE_PERIOD_MS > CONFIG_DOORBELL_LED_MULTI_PULSE_MS,
              "Sparse flash period must exceed pulse duration");
static_assert(CONFIG_DOORBELL_LED_DIM_BRIGHTNESS_PCT < CONFIG_DOORBELL_LED_FULL_BRIGHTNESS_PCT,
              "Dim brightness must be strictly less than full brightness");
static_assert(CONFIG_DOORBELL_LED_ERR_DURATION_MS >= CONFIG_DOORBELL_LED_FAST_PERIOD_MS,
              "Error indication must last at least one fast blink period");
static_assert(CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS >= CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS,
              "No-response indication must last at least one medium blink period");
