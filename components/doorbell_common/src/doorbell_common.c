#include "doorbell_config_checks.h"
#include "doorbell_relay.h"
#include "esp_log.h"

static const char *TAG = "doorbell_relay";

static void relay_timer_cb(void *arg)
{
    doorbell_relay_t *relay = (doorbell_relay_t *)arg;
    gpio_set_level(relay->gpio_num, relay->inactive_level);
    relay->is_active = false;
    relay->deadline_us = 0;
    if (relay->on_deenergize) {
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

    /* Boot-safe rule (I-22): write inactive output latch level BEFORE configuring GPIO output driver */
    ESP_ERROR_CHECK(gpio_set_level(gpio_num, relay->inactive_level));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(gpio_set_level(gpio_num, relay->inactive_level));

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

    if (relay->is_active) {
        if (already_active_out) {
            *already_active_out = true;
        }
        return ESP_OK;
    }

    if (already_active_out) {
        *already_active_out = false;
    }

    int64_t now_us = esp_timer_get_time();
    uint64_t duration_us = (uint64_t)relay->pulse_ms * 1000ULL;
    relay->deadline_us = now_us + (int64_t)duration_us;
    relay->is_active = true;

    gpio_set_level(relay->gpio_num, relay->active_level);

    esp_err_t err = esp_timer_start_once(relay->timer, duration_us);
    if (err != ESP_OK) {
        gpio_set_level(relay->gpio_num, relay->inactive_level);
        relay->is_active = false;
        relay->deadline_us = 0;
        ESP_LOGE(TAG, "Failed to start relay timer on GPIO %d: %s", relay->gpio_num, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

void doorbell_relay_force_off(doorbell_relay_t *relay)
{
    if (!relay) {
        return;
    }
    esp_timer_stop(relay->timer);
    gpio_set_level(relay->gpio_num, relay->inactive_level);
    bool was_active = relay->is_active;
    relay->is_active = false;
    relay->deadline_us = 0;
    if (was_active && relay->on_deenergize) {
        relay->on_deenergize(relay->cb_ctx);
    }
}

void doorbell_relay_safety_check(doorbell_relay_t *relay)
{
    if (!relay || !relay->is_active) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    /* Independent safety clamp with 100 ms grace margin (I-22) */
    if (relay->deadline_us > 0 && now_us > (relay->deadline_us + 100000LL)) {
        ESP_LOGW(TAG, "Safety clamp forced relay off on GPIO %d", relay->gpio_num);
        doorbell_relay_force_off(relay);
    }
}

