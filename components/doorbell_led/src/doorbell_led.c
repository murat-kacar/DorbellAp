#include "doorbell_led.h"
#include "doorbell_config_checks.h"
#include "led_indicator.h"
#include "led_indicator_ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "doorbell_led";

/* "On" steps use LED_BLINK_BRIGHTNESS so these levels are honoured; LED_BLINK_HOLD would ignore them and drive max
 * duty. "Off" steps stay LED_BLINK_HOLD with LED_STATE_OFF (duty 0). Clamp BRIGHTNESS_DIM to >= 23 because
 * espressif/led_indicator's gamma=2.3 LUT maps inputs 0..22 to 0 (completely dark). */
#define BRIGHTNESS_FULL    ((uint32_t)((255U * CONFIG_DOORBELL_LED_FULL_BRIGHTNESS_PCT) / 100U))
#define BRIGHTNESS_DIM_RAW ((uint32_t)((255U * CONFIG_DOORBELL_LED_DIM_BRIGHTNESS_PCT) / 100U))
#define BRIGHTNESS_DIM     ((BRIGHTNESS_DIM_RAW < 23U) ? 23U : BRIGHTNESS_DIM_RAW)

#define FAST_HALF_MS    (CONFIG_DOORBELL_LED_FAST_PERIOD_MS / 2)
#define MED_HALF_MS     (CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS / 2)
#define SLOW_HALF_MS    (CONFIG_DOORBELL_LED_SLOW_PERIOD_MS / 2)
#define PULSE_MS        (CONFIG_DOORBELL_LED_MULTI_PULSE_MS)
#define GAP_MS          (CONFIG_DOORBELL_LED_MULTI_GAP_MS)
#define DOUBLE_REST_MS  (CONFIG_DOORBELL_LED_MULTI_PERIOD_MS - (2 * PULSE_MS + GAP_MS))
#define TRIPLE_REST_MS  (CONFIG_DOORBELL_LED_MULTI_PERIOD_MS - (3 * PULSE_MS + 2 * GAP_MS))
#define SPARSE_REST_MS  (CONFIG_DOORBELL_LED_SPARSE_PERIOD_MS - PULSE_MS)

/* Timed one-shot indications (error, no-response) loop their blink pattern and are stopped by a one-shot esp_timer
 * after the configured duration, so the duration Kconfig options are honoured independently of the blink period. */
static esp_timer_handle_t create_stop_timer(esp_timer_cb_t cb, void *arg, const char *name)
{
    const esp_timer_create_args_t args = {
        .callback = cb,
        .arg = arg,
        .name = name,
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED stop timer '%s'", name);
        return NULL;
    }
    return t;
}

/* Start a timed indication: cancel the previous stop timer FIRST, so a stop callback from the previous indication
 * cannot fire between led_indicator_start() and the re-arm and cut the new indication short. */
static void start_timed(led_indicator_handle_t led, int state, esp_timer_handle_t t, uint32_t duration_ms)
{
    esp_timer_stop(t); /* ESP_ERR_INVALID_STATE when not running is expected */
    led_indicator_start(led, state);
    esp_timer_start_once(t, (uint64_t)duration_ms * 1000ULL);
}

#if defined(CONFIG_DOORBELL_ROLE_PANEL)

static led_indicator_handle_t s_panel_led = NULL;
static esp_timer_handle_t s_panel_err_timer = NULL;
static esp_timer_handle_t s_panel_no_resp_timer = NULL;

static void panel_stop_timer_cb(void *arg)
{
    if (s_panel_led) {
        led_indicator_stop(s_panel_led, (int)(intptr_t)arg);
    }
}

static const blink_step_t s_panel_key_press[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, CONFIG_DOORBELL_LED_SHORT_FLASH_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0},
};

static const blink_step_t s_panel_door_open[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, CONFIG_DOORBELL_PANEL_DOOR_PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0},
};

static const blink_step_t s_panel_result_ok[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, CONFIG_DOORBELL_LED_OK_DURATION_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0},
};

/* Fast blink, stopped after CONFIG_DOORBELL_LED_ERR_DURATION_MS by s_panel_err_timer */
static const blink_step_t s_panel_result_err[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, FAST_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   FAST_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

/* Medium blink, stopped after CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS by s_panel_no_resp_timer */
static const blink_step_t s_panel_no_response[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, MED_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   MED_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_reset_hold[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, 900},
    {LED_BLINK_HOLD, LED_STATE_OFF,   100},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_typing[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, SLOW_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   SLOW_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_pairing[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   DOUBLE_REST_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_portal[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, SLOW_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   SLOW_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_sparse[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   SPARSE_REST_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_panel_idle[] = {
    {LED_BLINK_HOLD, LED_STATE_OFF, 1000},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t *s_panel_blink_lists[LED_PANEL_MAX] = {
    [LED_PANEL_KEY_PRESS]   = s_panel_key_press,
    [LED_PANEL_DOOR_OPEN]   = s_panel_door_open,
    [LED_PANEL_RESULT_OK]   = s_panel_result_ok,
    [LED_PANEL_RESULT_ERR]  = s_panel_result_err,
    [LED_PANEL_NO_RESPONSE] = s_panel_no_response,
    [LED_PANEL_RESET_HOLD]  = s_panel_reset_hold,
    [LED_PANEL_TYPING]      = s_panel_typing,
    [LED_PANEL_PAIRING]     = s_panel_pairing,
    [LED_PANEL_PORTAL]      = s_panel_portal,
    [LED_PANEL_LOCKOUT]     = s_panel_sparse,
    [LED_PANEL_NET_LOST]    = s_panel_sparse,
    [LED_PANEL_IDLE]        = s_panel_idle,
};

esp_err_t doorbell_led_panel_init(gpio_num_t gpio_num)
{
    led_indicator_ledc_config_t ledc_cfg = {
        .is_active_level_high = DOORBELL_LED_ACTIVE_HIGH,
        .timer_inited = false,
        .timer_num = LEDC_TIMER_0,
        .gpio_num = gpio_num,
        .channel = LEDC_CHANNEL_0,
    };
    led_indicator_config_t cfg = {
        .blink_lists = s_panel_blink_lists,
        .blink_list_num = LED_PANEL_MAX,
    };
    esp_err_t err = led_indicator_new_ledc_device(&cfg, &ledc_cfg, &s_panel_led);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init panel LED on GPIO %d: %s", gpio_num, esp_err_to_name(err));
        return err;
    }
    s_panel_err_timer = create_stop_timer(panel_stop_timer_cb, (void *)(intptr_t)LED_PANEL_RESULT_ERR, "led_err");
    s_panel_no_resp_timer = create_stop_timer(panel_stop_timer_cb, (void *)(intptr_t)LED_PANEL_NO_RESPONSE,
                                              "led_noresp");
    if (!s_panel_err_timer || !s_panel_no_resp_timer) {
        /* The error / no-response patterns loop until their stop timer fires: never run without it */
        return ESP_ERR_NO_MEM;
    }
    led_indicator_start(s_panel_led, LED_PANEL_IDLE);
    return ESP_OK;
}

void doorbell_led_panel_start(doorbell_panel_led_state_t state)
{
    if (s_panel_led && state < LED_PANEL_MAX) {
        if (state == LED_PANEL_RESULT_ERR) {
            start_timed(s_panel_led, state, s_panel_err_timer, CONFIG_DOORBELL_LED_ERR_DURATION_MS);
        } else if (state == LED_PANEL_NO_RESPONSE) {
            start_timed(s_panel_led, state, s_panel_no_resp_timer, CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS);
        } else {
            led_indicator_start(s_panel_led, state);
        }
    }
}

void doorbell_led_panel_stop(doorbell_panel_led_state_t state)
{
    if (s_panel_led && state < LED_PANEL_MAX) {
        if (state == LED_PANEL_RESULT_ERR) {
            esp_timer_stop(s_panel_err_timer);
        } else if (state == LED_PANEL_NO_RESPONSE) {
            esp_timer_stop(s_panel_no_resp_timer);
        }
        led_indicator_stop(s_panel_led, state);
    }
}

#endif /* CONFIG_DOORBELL_ROLE_PANEL */

#if defined(CONFIG_DOORBELL_ROLE_UNIT)

static led_indicator_handle_t s_unit_led = NULL;
static esp_timer_handle_t s_unit_no_resp_timer = NULL;

static void unit_stop_timer_cb(void *arg)
{
    if (s_unit_led) {
        led_indicator_stop(s_unit_led, (int)(intptr_t)arg);
    }
}

static const blink_step_t s_unit_ringing[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, FAST_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   FAST_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

/* Repeating triple flash for 2000 ms (2 cycles) */
static const blink_step_t s_unit_btn_cancel[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   TRIPLE_REST_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   TRIPLE_REST_MS},
    {LED_BLINK_STOP, 0, 0},
};

static const blink_step_t s_unit_pair_hold[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, MED_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   MED_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_unit_reset_hold[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, 900},
    {LED_BLINK_HOLD, LED_STATE_OFF,   100},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_unit_result_ok[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, CONFIG_DOORBELL_LED_OK_DURATION_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0},
};

/* Medium blink, stopped after CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS by s_unit_no_resp_timer */
static const blink_step_t s_unit_no_response[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, MED_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   MED_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_unit_pairing_search[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, SLOW_HALF_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   SLOW_HALF_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_unit_unpaired[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   GAP_MS},
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   DOUBLE_REST_MS},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_unit_out_of_cov[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, PULSE_MS},
    {LED_BLINK_HOLD, LED_STATE_OFF,   SPARSE_REST_MS},
    {LED_BLINK_LOOP, 0, 0},
};

/* Dim steady state. LED_BLINK_BRIGHTNESS applies the component's gamma table (CONFIG_USE_GAMMA_CORRECTION), so the
 * configured percentage is perceived brightness, not PWM duty: the 10 % default maps to ~0.4 % duty (docs/protocol.md
 * §7, I-67 — verify visibility on the bench). The hold time must stay non-zero: a zero-length step followed by LOOP
 * would spin inside the blink runner. */
static const blink_step_t s_unit_in_coverage[] = {
    {LED_BLINK_BRIGHTNESS, BRIGHTNESS_DIM, 1000},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t *s_unit_blink_lists[LED_UNIT_MAX] = {
    [LED_UNIT_RINGING]        = s_unit_ringing,
    [LED_UNIT_BTN_CANCEL]     = s_unit_btn_cancel,
    [LED_UNIT_PAIR_HOLD]      = s_unit_pair_hold,
    [LED_UNIT_RESET_HOLD]     = s_unit_reset_hold,
    [LED_UNIT_RESULT_OK]      = s_unit_result_ok,
    [LED_UNIT_NO_RESPONSE]    = s_unit_no_response,
    [LED_UNIT_PAIRING_SEARCH] = s_unit_pairing_search,
    [LED_UNIT_UNPAIRED]       = s_unit_unpaired,
    [LED_UNIT_OUT_OF_COV]     = s_unit_out_of_cov,
    [LED_UNIT_IN_COVERAGE]    = s_unit_in_coverage,
};

esp_err_t doorbell_led_unit_init(gpio_num_t gpio_num)
{
    led_indicator_ledc_config_t ledc_cfg = {
        .is_active_level_high = DOORBELL_LED_ACTIVE_HIGH,
        .timer_inited = false,
        .timer_num = LEDC_TIMER_0,
        .gpio_num = gpio_num,
        .channel = LEDC_CHANNEL_0,
    };
    led_indicator_config_t cfg = {
        .blink_lists = s_unit_blink_lists,
        .blink_list_num = LED_UNIT_MAX,
    };
    esp_err_t err = led_indicator_new_ledc_device(&cfg, &ledc_cfg, &s_unit_led);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init unit LED on GPIO %d: %s", gpio_num, esp_err_to_name(err));
        return err;
    }
    s_unit_no_resp_timer = create_stop_timer(unit_stop_timer_cb, (void *)(intptr_t)LED_UNIT_NO_RESPONSE, "led_noresp");
    if (!s_unit_no_resp_timer) {
        /* The no-response pattern loops until its stop timer fires: never run without it */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void doorbell_led_unit_start(doorbell_unit_led_state_t state)
{
    if (s_unit_led && state < LED_UNIT_MAX) {
        if (state == LED_UNIT_NO_RESPONSE) {
            start_timed(s_unit_led, state, s_unit_no_resp_timer, CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS);
        } else {
            led_indicator_start(s_unit_led, state);
        }
    }
}

void doorbell_led_unit_stop(doorbell_unit_led_state_t state)
{
    if (s_unit_led && state < LED_UNIT_MAX) {
        if (state == LED_UNIT_NO_RESPONSE) {
            esp_timer_stop(s_unit_no_resp_timer);
        }
        led_indicator_stop(s_unit_led, state);
    }
}

#endif /* CONFIG_DOORBELL_ROLE_UNIT */

