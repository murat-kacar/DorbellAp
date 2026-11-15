/* Apartment module buttons. The callbacks run in the esp_timer task: they only drive the LED and set request
 * flags; the work (pairing, door open, reset) runs in unit_worker_task. */
#include "unit_state.h"
#include <stdatomic.h>
#include <stdint.h>
#include "esp_check.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "unit_config.h"
#include "doorbell_led.h"

static const char *TAG = "unit_buttons";

#define HOLD_MIN_MS ((uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S * 1000U)
#define HOLD_MAX_MS ((uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S * 1000U)

static _Atomic bool s_pair_btn_down = false;
static _Atomic bool s_reset_btn_down = false;
static _Atomic bool s_combo_cancelled = false;

static void door_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    uint32_t held_ms = iot_button_get_pressed_time((button_handle_t)button_handle);
    if (held_ms < CONFIG_DOORBELL_UNIT_DOOR_BTN_MIN_MS ||
            held_ms > (uint32_t)CONFIG_DOORBELL_UNIT_DOOR_BTN_MAX_S * 1000U) {
        return;
    }
    if (!g_unit_paired) {
        /* Unpaired door-open press immediately shows no-response blink */
        doorbell_led_start(LED_UNIT_NO_RESPONSE);
        return;
    }
    if (!g_unit_open_door_in_flight) {
        /* A press while a transaction is in flight or already requested is coalesced */
        g_unit_open_door_requested = true;
    }
}

/* Pressing both hidden buttons together cancels both (triple flash) */
static void check_simultaneous_cancel(void)
{
    if (s_pair_btn_down && s_reset_btn_down) {
        s_combo_cancelled = true;
        doorbell_led_stop(LED_UNIT_PAIR_HOLD);
        doorbell_led_stop(LED_UNIT_RESET_HOLD);
        doorbell_led_start(LED_UNIT_BTN_CANCEL);
    }
}

/* A hidden-button release ends a valid 7-12 s hold unless it was cancelled or pairing is running */
static bool hold_release_valid(button_handle_t btn)
{
    uint32_t held_ms = iot_button_get_pressed_time(btn);
    return !s_combo_cancelled && !g_unit_pairing_active && held_ms >= HOLD_MIN_MS && held_ms <= HOLD_MAX_MS;
}

static void hold_btn_cancel_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    doorbell_led_stop((unit_led_state_t)(uintptr_t)usr_data);
    doorbell_led_start(LED_UNIT_BTN_CANCEL);
}

static void hold_btn_max_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    /* Turn OFF the hold indicator at 12 s: the 7-12 s window has passed (docs/protocol.md §7.2) */
    doorbell_led_stop((unit_led_state_t)(uintptr_t)usr_data);
}

static void pair_btn_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    s_pair_btn_down = true;
    if (!s_reset_btn_down) {
        s_combo_cancelled = false;
    }
    check_simultaneous_cancel();
}

static void pair_btn_min_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (s_combo_cancelled || g_unit_pairing_active || g_unit_paired) {
        return;
    }
    doorbell_led_start(LED_UNIT_PAIR_HOLD);
    /* Pre-generate the X25519 keypair on the worker task while the button is held */
    g_unit_pregen_x25519_requested = true;
}

static void pair_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    s_pair_btn_down = false;
    doorbell_led_stop(LED_UNIT_PAIR_HOLD);
    if (!hold_release_valid((button_handle_t)button_handle)) {
        return;
    }
    if (g_unit_paired) {
        /* Already paired: triple-flash cancel to remind the user to reset first */
        doorbell_led_start(LED_UNIT_BTN_CANCEL);
    } else {
        g_unit_pairing_requested = true;
    }
}

static void reset_btn_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    s_reset_btn_down = true;
    if (!s_pair_btn_down) {
        s_combo_cancelled = false;
    }
    check_simultaneous_cancel();
}

static void reset_btn_min_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (s_combo_cancelled || g_unit_pairing_active) {
        return;
    }
    doorbell_led_start(LED_UNIT_RESET_HOLD);
}

static void reset_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    s_reset_btn_down = false;
    doorbell_led_stop(LED_UNIT_RESET_HOLD);
    if (hold_release_valid((button_handle_t)button_handle)) {
        g_unit_reset_requested = true;
    }
}

static esp_err_t new_button(int gpio_num, button_handle_t *out)
{
    button_config_t cfg = {0};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio_num,
        .active_level = DOORBELL_UNIT_BTNS_ACTIVE_LOW ? 0 : 1,
        .disable_pull = !DOORBELL_UNIT_BTNS_INTERNAL_PULL,
    };
    return iot_button_new_gpio_device(&cfg, &gpio_cfg, out);
}

/* Hidden button with the 7 s / 12 s / 15 s hold thresholds (min hold, end of window, cancel) */
static esp_err_t register_hold_button(button_handle_t btn, button_cb_t down_cb, button_cb_t up_cb,
                                      button_cb_t min_hold_cb, unit_led_state_t hold_led)
{
    button_event_args_t min_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S * 1000};
    button_event_args_t max_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S * 1000};
    button_event_args_t cancel_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S * 1000};
    void *led = (void *)(uintptr_t)hold_led;

    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, down_cb, NULL), TAG, "press down");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, up_cb, NULL), TAG, "press up");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &min_args, min_hold_cb, NULL), TAG,
                        "min hold");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &max_args, hold_btn_max_cb, led), TAG,
                        "max hold");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &cancel_args, hold_btn_cancel_cb, led),
                        TAG, "cancel hold");
    return ESP_OK;
}

esp_err_t unit_buttons_init(void)
{
    button_handle_t door_btn = NULL;
    ESP_RETURN_ON_ERROR(new_button(CONFIG_DOORBELL_UNIT_GPIO_DOOR_BTN, &door_btn), TAG, "door button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(door_btn, BUTTON_PRESS_UP, NULL, door_btn_up_cb, NULL), TAG,
                        "door button up");

    button_handle_t pair_btn = NULL;
    ESP_RETURN_ON_ERROR(new_button(CONFIG_DOORBELL_UNIT_GPIO_PAIR_BTN, &pair_btn), TAG, "pairing button");
    ESP_RETURN_ON_ERROR(register_hold_button(pair_btn, pair_btn_down_cb, pair_btn_up_cb, pair_btn_min_hold_cb,
                                             LED_UNIT_PAIR_HOLD), TAG, "pairing button");

    button_handle_t reset_btn = NULL;
    ESP_RETURN_ON_ERROR(new_button(CONFIG_DOORBELL_UNIT_GPIO_RESET_BTN, &reset_btn), TAG, "reset button");
    ESP_RETURN_ON_ERROR(register_hold_button(reset_btn, reset_btn_down_cb, reset_btn_up_cb, reset_btn_min_hold_cb,
                                             LED_UNIT_RESET_HOLD), TAG, "reset button");
    return ESP_OK;
}
