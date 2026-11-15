#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status LED patterns (docs/protocol.md §7). Timings and brightness come from the shared "Doorbell status LED"
 *        Kconfig menu; each firmware maps its indication states to these patterns.
 */
typedef enum {
    DOORBELL_LED_OFF = 0,          /**< dark; for the lowest-priority idle state */
    DOORBELL_LED_SOLID,            /**< full brightness; one-shot with a duration, else held until stopped */
    DOORBELL_LED_DIM,              /**< dim brightness, held until stopped */
    DOORBELL_LED_SHORT_FLASH,      /**< one short flash (key press), one-shot */
    DOORBELL_LED_FAST_BLINK,       /**< fast blink period */
    DOORBELL_LED_MEDIUM_BLINK,     /**< medium blink period */
    DOORBELL_LED_SLOW_BLINK,       /**< slow blink period (1 Hz) */
    DOORBELL_LED_DOUBLE_FLASH,     /**< repeating double flash */
    DOORBELL_LED_TRIPLE_FLASH,     /**< repeating triple flash */
    DOORBELL_LED_SPARSE_FLASH,     /**< single flash every sparse period */
    DOORBELL_LED_LONG_ON_SHORT_OFF, /**< 900 ms on / 100 ms off (hidden-button hold) */
} doorbell_led_pattern_t;

/** One indication state: a pattern and, for timed one-shots, how long it lasts (0 = until stopped / natural end) */
typedef struct {
    doorbell_led_pattern_t pattern;
    uint32_t duration_ms;
} doorbell_led_indication_t;

/**
 * @brief Create the status LED (espressif/led_indicator on LEDC).
 *
 * @param table  indication states in priority order (index 0 = highest priority); copied, may be temporary
 * @param count  number of states
 * @param initial_state state started right away, or -1 for none
 */
esp_err_t doorbell_led_init(gpio_num_t gpio_num, const doorbell_led_indication_t *table, size_t count,
                            int initial_state);

/** Start indication @p state (an index into the init table). Safe from any task or esp_timer callback. */
void doorbell_led_start(int state);
/** Stop indication @p state; the highest-priority other active state shows. */
void doorbell_led_stop(int state);

#ifdef __cplusplus
}
#endif
