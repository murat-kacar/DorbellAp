#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Boot-safe relay controller with one-shot timer and main-loop safety clamp (I-22).
 */
typedef struct {
    gpio_num_t gpio_num;
    int active_level;
    int inactive_level;
    uint32_t pulse_ms;
    volatile bool is_active;
    volatile int64_t deadline_us;
    esp_timer_handle_t timer;
    void (*on_deenergize)(void *ctx);
    void *cb_ctx;
} doorbell_relay_t;

/**
 * @brief Initialize relay GPIO safely (sets inactive output level before enabling output driver).
 */
esp_err_t doorbell_relay_init(doorbell_relay_t *relay,
                              gpio_num_t gpio_num,
                              bool active_high,
                              uint32_t pulse_ms,
                              void (*on_deenergize)(void *ctx),
                              void *cb_ctx);

/**
 * @brief Trigger the relay pulse.
 *
 * @param relay Relay instance.
 * @param already_active_out Set to true if the relay was already energized (no-op per STATUS_ALREADY_ACTIVE).
 * @return ESP_OK on success.
 */
esp_err_t doorbell_relay_trigger(doorbell_relay_t *relay, bool *already_active_out);

/**
 * @brief Force de-energize the relay immediately.
 */
void doorbell_relay_force_off(doorbell_relay_t *relay);

/**
 * @brief Independent safety clamp called periodically from a TWDT-monitored task loop (I-22).
 *        Forces the relay off if it has remained active past pulse_ms + 100 ms margin.
 */
void doorbell_relay_safety_check(doorbell_relay_t *relay);

/**
 * @brief Return a human-readable English description of an esp_reset_reason_t code (I-58, I-68).
 */
const char *doorbell_reset_reason_str(uint8_t reason);

#ifdef __cplusplus
}
#endif

