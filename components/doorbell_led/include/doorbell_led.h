#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Panel Status LED Priority States (docs/protocol.md §7.1, 0 = highest priority).
 */
typedef enum {
    LED_PANEL_KEY_PRESS = 0,   /* 50 ms single short flash (one-shot) */
    LED_PANEL_DOOR_OPEN,       /* Solid ON for door relay pulse (one-shot) */
    LED_PANEL_RESULT_OK,       /* Solid ON for 2000 ms (one-shot) */
    LED_PANEL_RESULT_ERR,      /* Fast blink 200 ms period for 1000 ms (one-shot) */
    LED_PANEL_NO_RESPONSE,     /* Medium blink 500 ms period for 3000 ms (one-shot) */
    LED_PANEL_RESET_HOLD,      /* 900 ms ON / 100 ms OFF while reset button held 10-60 s */
    LED_PANEL_TYPING,          /* Slow blink 1 Hz while keypad buffer non-empty */
    LED_PANEL_PAIRING,         /* Repeating double flash during pairing discovery/completion */
    LED_PANEL_PORTAL,          /* Slow blink 1 Hz while setup web portal is open */
    LED_PANEL_LOCKOUT,         /* Sparse single flash (3000 ms) during keypad lockout */
    LED_PANEL_NET_LOST,        /* Sparse single flash (3000 ms) when STA disconnected from AP */
    LED_PANEL_IDLE,            /* OFF (0%) steady state */
    LED_PANEL_MAX
} doorbell_panel_led_state_t;

/**
 * @brief Apartment Module Status LED Priority States (docs/protocol.md §7.2, 0 = highest priority).
 */
typedef enum {
    LED_UNIT_RINGING = 0,      /* Fast blink 200 ms period during bell relay pulse (one-shot) */
    LED_UNIT_BTN_CANCEL,       /* Repeating triple flash for 2000 ms on >= 15 s hold or invalid combo (one-shot) */
    LED_UNIT_PAIR_HOLD,        /* Medium blink 500 ms period while pairing button held 7-12 s */
    LED_UNIT_RESET_HOLD,       /* 900 ms ON / 100 ms OFF while reset button held 7-12 s */
    LED_UNIT_RESULT_OK,        /* Full brightness solid ON for 2000 ms (one-shot) */
    LED_UNIT_NO_RESPONSE,      /* Medium blink 500 ms period for 3000 ms (one-shot) */
    LED_UNIT_PAIRING_SEARCH,   /* Slow blink 1 Hz while searching/completing pairing */
    LED_UNIT_UNPAIRED,         /* Repeating double flash when unpaired */
    LED_UNIT_OUT_OF_COV,       /* Sparse single flash (3000 ms) when disconnected or 3 missed HEARTBEAT_ACKs */
    LED_UNIT_IN_COVERAGE,      /* Dim solid (10%) when paired, connected, and panel reachable */
    LED_UNIT_MAX
} doorbell_unit_led_state_t;

esp_err_t doorbell_led_panel_init(gpio_num_t gpio_num);
void doorbell_led_panel_start(doorbell_panel_led_state_t state);
void doorbell_led_panel_stop(doorbell_panel_led_state_t state);

esp_err_t doorbell_led_unit_init(gpio_num_t gpio_num);
void doorbell_led_unit_start(doorbell_unit_led_state_t state);
void doorbell_led_unit_stop(doorbell_unit_led_state_t state);

#ifdef __cplusplus
}
#endif

