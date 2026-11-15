#include "doorbell_relay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "doorbell_relay";

/* Guards is_active and deadline_us: relays are triggered, cleared and checked from more than one task (worker,
 * esp_timer) on both cores, and a 64-bit deadline read is not atomic on the 32-bit Xtensa core (AGENTS.md §7) */
static portMUX_TYPE s_relay_lock = portMUX_INITIALIZER_UNLOCKED;

/* Mark the relay inactive; returns whether it was active (the caller then runs on_deenergize outside the lock) */
static bool relay_clear_state(doorbell_relay_t *relay)
{
    taskENTER_CRITICAL(&s_relay_lock);
    bool was_active = relay->is_active;
    relay->is_active = false;
    relay->deadline_us = 0;
    taskEXIT_CRITICAL(&s_relay_lock);
    return was_active;
}

static void relay_timer_cb(void *arg)
{
    doorbell_relay_t *relay = (doorbell_relay_t *)arg;
    gpio_set_level(relay->gpio_num, relay->inactive_level);
    if (relay_clear_state(relay) && relay->on_deenergize) {
        relay->on_deenergize(relay->cb_ctx);
    }
}

esp_err_t doorbell_relay_init(doorbell_relay_t *relay,
                              gpio_num_t gpio_num,
                              bool active_high,
                              uint32_t pulse_ms,
                              void (*on_deenergize)(void *ctx),
                              void *cb_ctx)
{
    if (!relay || !GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    relay->gpio_num = gpio_num;
    relay->active_level = active_high ? 1 : 0;
    relay->inactive_level = active_high ? 0 : 1;
    relay->pulse_ms = pulse_ms;
    relay->is_active = false;
    relay->deadline_us = 0;
    relay->on_deenergize = on_deenergize;
    relay->cb_ctx = cb_ctx;

    /* Boot-safe rule: write inactive output latch level BEFORE configuring GPIO output driver */
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio_num, relay->inactive_level), TAG, "preset inactive level");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config(%d)", (int)gpio_num);
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio_num, relay->inactive_level), TAG, "set inactive level");

    const esp_timer_create_args_t timer_args = {
        .callback = &relay_timer_cb,
        .arg = relay,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "relay_pulse",
    };
    return esp_timer_create(&timer_args, &relay->timer);
}

esp_err_t doorbell_relay_trigger(doorbell_relay_t *relay, bool *already_active_out)
{
    if (!relay) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t duration_us = (uint64_t)relay->pulse_ms * 1000ULL;

    taskENTER_CRITICAL(&s_relay_lock);
    bool was_active = relay->is_active;
    if (!was_active) {
        relay->is_active = true;
        relay->deadline_us = esp_timer_get_time() + (int64_t)duration_us;
    }
    taskEXIT_CRITICAL(&s_relay_lock);

    if (already_active_out) {
        *already_active_out = was_active;
    }
    if (was_active) {
        return ESP_OK;
    }

    gpio_set_level(relay->gpio_num, relay->active_level);

    esp_err_t err = esp_timer_start_once(relay->timer, duration_us);
    if (err != ESP_OK) {
        gpio_set_level(relay->gpio_num, relay->inactive_level);
        relay_clear_state(relay);
        ESP_LOGE(TAG, "Failed to start relay timer on GPIO %d: %s", (int)relay->gpio_num, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

bool doorbell_relay_is_active(doorbell_relay_t *relay)
{
    if (!relay) {
        return false;
    }
    taskENTER_CRITICAL(&s_relay_lock);
    bool active = relay->is_active;
    taskEXIT_CRITICAL(&s_relay_lock);
    return active;
}

void doorbell_relay_force_off(doorbell_relay_t *relay)
{
    if (!relay) {
        return;
    }
    esp_timer_stop(relay->timer);
    gpio_set_level(relay->gpio_num, relay->inactive_level);
    if (relay_clear_state(relay) && relay->on_deenergize) {
        relay->on_deenergize(relay->cb_ctx);
    }
}

void doorbell_relay_safety_check(doorbell_relay_t *relay)
{
    if (!relay) {
        return;
    }
    taskENTER_CRITICAL(&s_relay_lock);
    bool active = relay->is_active;
    int64_t deadline_us = relay->deadline_us;
    taskEXIT_CRITICAL(&s_relay_lock);

    /* Independent safety clamp with 100 ms grace margin */
    if (active && deadline_us > 0 && esp_timer_get_time() > (deadline_us + 100000LL)) {
        ESP_LOGW(TAG, "Safety clamp forced relay off on GPIO %d", (int)relay->gpio_num);
        doorbell_relay_force_off(relay);
    }
}
