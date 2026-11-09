#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register TWDT health users for both timer pipelines (I-46).
 *
 * A 1 s periodic esp_timer and a 1 s periodic FreeRTOS software timer each feed their own TWDT user, so a deadlock
 * in the esp_timer task (relay de-energize, LED stop timers) or in the FreeRTOS timer service task (led_indicator
 * blink runners) triggers a watchdog reset.
 *
 * @param esp_timer_user_name  TWDT user name for the esp_timer pipeline (must be a string literal).
 * @param rtos_timer_user_name TWDT user name for the FreeRTOS timer pipeline (must be a string literal).
 */
esp_err_t doorbell_twdt_register_timer_pipelines(const char *esp_timer_user_name, const char *rtos_timer_user_name);

#ifdef __cplusplus
}
#endif
