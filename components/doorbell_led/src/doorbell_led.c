#include "doorbell_led.h"
#include <stdlib.h>
#include "doorbell_config_checks.h"
#include "led_indicator.h"
#include "led_indicator_ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "doorbell_led";

/* "On" steps use LED_BLINK_BRIGHTNESS so these levels are honoured; LED_BLINK_HOLD would ignore them and drive max
 * duty. "Off" steps stay LED_BLINK_HOLD with LED_STATE_OFF (duty 0). Clamp BRIGHTNESS_DIM to >= 23 because
 * espressif/led_indicator's gamma=2.3 LUT maps inputs 0..22 to 0 (completely dark). LED_BLINK_BRIGHTNESS applies the
 * component's gamma table (CONFIG_USE_GAMMA_CORRECTION), so the dim percentage is perceived brightness, not PWM duty:
 * the 10 % default maps to ~0.4 % duty (docs/protocol.md §7). */
#define BRIGHTNESS_FULL    ((uint32_t)((255U * CONFIG_DOORBELL_LED_FULL_BRIGHTNESS_PCT) / 100U))
#define BRIGHTNESS_DIM_RAW ((uint32_t)((255U * CONFIG_DOORBELL_LED_DIM_BRIGHTNESS_PCT) / 100U))
#define BRIGHTNESS_DIM     ((BRIGHTNESS_DIM_RAW < 23U) ? 23U : BRIGHTNESS_DIM_RAW)

#define PULSE_MS       (CONFIG_DOORBELL_LED_MULTI_PULSE_MS)
#define GAP_MS         (CONFIG_DOORBELL_LED_MULTI_GAP_MS)
#define DOUBLE_REST_MS (CONFIG_DOORBELL_LED_MULTI_PERIOD_MS - (2 * PULSE_MS + GAP_MS))
#define TRIPLE_REST_MS (CONFIG_DOORBELL_LED_MULTI_PERIOD_MS - (3 * PULSE_MS + 2 * GAP_MS))
#define SPARSE_REST_MS (CONFIG_DOORBELL_LED_SPARSE_PERIOD_MS - PULSE_MS)

#define ON(ms)  {LED_BLINK_BRIGHTNESS, BRIGHTNESS_FULL, (ms)}
#define OFF(ms) {LED_BLINK_HOLD, LED_STATE_OFF, (ms)}
#define LOOP    {LED_BLINK_LOOP, 0, 0}
#define STOP    {LED_BLINK_STOP, 0, 0}

/* Looping patterns. A hold step followed by LOOP must have a non-zero time, or the blink runner would spin. */
static const blink_step_t s_off[] = {OFF(1000), LOOP};
static const blink_step_t s_solid[] = {ON(1000), LOOP};
static const blink_step_t s_dim[] = {{LED_BLINK_BRIGHTNESS, BRIGHTNESS_DIM, 1000}, LOOP};
static const blink_step_t s_short_flash[] = {ON(CONFIG_DOORBELL_LED_SHORT_FLASH_MS), OFF(0), STOP};
static const blink_step_t s_fast[] = {ON(CONFIG_DOORBELL_LED_FAST_PERIOD_MS / 2),
                                      OFF(CONFIG_DOORBELL_LED_FAST_PERIOD_MS / 2), LOOP
                                     };
static const blink_step_t s_medium[] = {ON(CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS / 2),
                                        OFF(CONFIG_DOORBELL_LED_MEDIUM_PERIOD_MS / 2), LOOP
                                       };
static const blink_step_t s_slow[] = {ON(CONFIG_DOORBELL_LED_SLOW_PERIOD_MS / 2),
                                      OFF(CONFIG_DOORBELL_LED_SLOW_PERIOD_MS / 2), LOOP
                                     };
static const blink_step_t s_double[] = {ON(PULSE_MS), OFF(GAP_MS), ON(PULSE_MS), OFF(DOUBLE_REST_MS), LOOP};
static const blink_step_t s_triple[] = {ON(PULSE_MS), OFF(GAP_MS), ON(PULSE_MS), OFF(GAP_MS), ON(PULSE_MS),
                                        OFF(TRIPLE_REST_MS), LOOP
                                       };
static const blink_step_t s_sparse[] = {ON(PULSE_MS), OFF(SPARSE_REST_MS), LOOP};
static const blink_step_t s_long_on[] = {ON(900), OFF(100), LOOP};

static const blink_step_t *const s_patterns[] = {
    [DOORBELL_LED_OFF] = s_off,
    [DOORBELL_LED_SOLID] = s_solid,
    [DOORBELL_LED_DIM] = s_dim,
    [DOORBELL_LED_SHORT_FLASH] = s_short_flash,
    [DOORBELL_LED_FAST_BLINK] = s_fast,
    [DOORBELL_LED_MEDIUM_BLINK] = s_medium,
    [DOORBELL_LED_SLOW_BLINK] = s_slow,
    [DOORBELL_LED_DOUBLE_FLASH] = s_double,
    [DOORBELL_LED_TRIPLE_FLASH] = s_triple,
    [DOORBELL_LED_SPARSE_FLASH] = s_sparse,
    [DOORBELL_LED_LONG_ON_SHORT_OFF] = s_long_on,
};

typedef struct {
    int state;
    esp_timer_handle_t timer; /* stops a timed indication after its duration; NULL for untimed states */
    uint32_t duration_ms;
} led_state_t;

static led_indicator_handle_t s_led = NULL;
static led_state_t *s_states = NULL;
static size_t s_state_count = 0;

static void stop_timer_cb(void *arg)
{
    led_indicator_stop(s_led, ((const led_state_t *)arg)->state);
}

esp_err_t doorbell_led_init(gpio_num_t gpio_num, const doorbell_led_indication_t *table, size_t count,
                            int initial_state)
{
    if (!table || count == 0 || s_led) {
        return ESP_ERR_INVALID_ARG;
    }
    const blink_step_t **lists = calloc(count, sizeof(*lists));
    s_states = calloc(count, sizeof(*s_states));
    /* A timed SOLID is a one-shot: on for its duration, then off and stopped (no timer needed) */
    blink_step_t *one_shots = calloc(count, 3 * sizeof(blink_step_t));
    if (!lists || !s_states || !one_shots) {
        free(lists);
        free(one_shots);
        free(s_states);
        s_states = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_state_count = count;
    esp_err_t err = ESP_OK;

    for (size_t i = 0; i < count; i++) {
        doorbell_led_pattern_t pattern = table[i].pattern;
        if ((size_t)pattern >= sizeof(s_patterns) / sizeof(s_patterns[0])) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }
        s_states[i].state = (int)i;
        s_states[i].duration_ms = table[i].duration_ms;
        if (pattern == DOORBELL_LED_SOLID && table[i].duration_ms > 0) {
            blink_step_t *steps = &one_shots[3 * i];
            steps[0] = (blink_step_t)ON(table[i].duration_ms);
            steps[1] = (blink_step_t)OFF(0);
            steps[2] = (blink_step_t)STOP;
            lists[i] = steps;
            continue;
        }
        lists[i] = s_patterns[pattern];
        /* Looping patterns with a duration (error, no response, cancel) are stopped by a one-shot esp_timer, so the
         * duration options are honoured independently of the blink period */
        if (table[i].duration_ms > 0 && pattern != DOORBELL_LED_SHORT_FLASH) {
            const esp_timer_create_args_t args = {
                .callback = stop_timer_cb,
                .arg = &s_states[i],
                .name = "led_stop",
            };
            err = esp_timer_create(&args, &s_states[i].timer);
            if (err != ESP_OK) {
                /* A timed pattern loops until its stop timer fires: never run without it */
                ESP_LOGE(TAG, "Failed to create LED stop timer: %s", esp_err_to_name(err));
                goto fail;
            }
        }
    }

    led_indicator_ledc_config_t ledc_cfg = {
        .is_active_level_high = DOORBELL_LED_ACTIVE_HIGH,
        .timer_inited = false,
        .timer_num = LEDC_TIMER_0,
        .gpio_num = gpio_num,
        .channel = LEDC_CHANNEL_0,
    };
    led_indicator_config_t cfg = {
        .blink_lists = lists,
        .blink_list_num = (uint16_t)count,
    };
    err = led_indicator_new_ledc_device(&cfg, &ledc_cfg, &s_led);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init status LED on GPIO %d: %s", (int)gpio_num, esp_err_to_name(err));
        goto fail;
    }
    /* lists / one_shots stay allocated: led_indicator keeps the pointers for the device's lifetime */
    if (initial_state >= 0) {
        doorbell_led_start(initial_state);
    }
    return ESP_OK;

fail:
    for (size_t i = 0; i < count; i++) {
        if (s_states[i].timer) {
            esp_timer_delete(s_states[i].timer);
        }
    }
    free(lists);
    free(one_shots);
    free(s_states);
    s_states = NULL;
    s_state_count = 0;
    return err;
}

void doorbell_led_start(int state)
{
    if (!s_led || state < 0 || (size_t)state >= s_state_count) {
        return;
    }
    led_state_t *st = &s_states[state];
    if (st->timer) {
        /* Cancel the previous stop timer FIRST, so its callback cannot fire between led_indicator_start() and the
         * re-arm and cut the new indication short (ESP_ERR_INVALID_STATE when not running is expected) */
        esp_timer_stop(st->timer);
        led_indicator_start(s_led, state);
        esp_timer_start_once(st->timer, (uint64_t)st->duration_ms * 1000ULL);
    } else {
        led_indicator_start(s_led, state);
    }
}

void doorbell_led_stop(int state)
{
    if (!s_led || state < 0 || (size_t)state >= s_state_count) {
        return;
    }
    if (s_states[state].timer) {
        esp_timer_stop(s_states[state].timer);
    }
    led_indicator_stop(s_led, state);
}
