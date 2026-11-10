#include "panel_state.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/constant_time.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "button_matrix.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "panel_keypad";

#define KEYPAD_BUF_MAX 48

#define KEY_QUEUE_DEPTH 32

static char s_key_buf[KEYPAD_BUF_MAX];
static size_t s_key_len = 0;
static esp_timer_handle_t s_input_timeout_timer = NULL;
/* Button callbacks run in the esp_timer task. They only enqueue keys / set flags; all parsing, mutex waits and NVS
 * writes happen in panel_worker_task via panel_keypad_process() so the esp_timer task (which also drives the door
 * relay de-energize timer) is never blocked. */
static QueueHandle_t s_key_queue = NULL;
static _Atomic bool s_input_timeout_pending = false;
static _Atomic bool s_key_overflow_pending = false;
static _Atomic int64_t s_lockout_deadline_us = 0;
static uint32_t s_lockout_persist_divider = 0;

static void clear_keypad_buffer(void)
{
    memset(s_key_buf, 0, sizeof(s_key_buf));
    s_key_len = 0;
    if (s_input_timeout_timer) {
        esp_timer_stop(s_input_timeout_timer);
    }
    doorbell_led_panel_stop(LED_PANEL_TYPING);
}

static void input_timeout_cb(void *arg)
{
    (void)arg;
    s_input_timeout_pending = true;
}

bool panel_is_locked_out(void)
{
    if (s_lockout_deadline_us <= 0) {
        return false;
    }
    return esp_timer_get_time() < s_lockout_deadline_us;
}

static void apply_wrong_code_penalty_locked(void)
{
    if (g_panel_cfg.wrong_code_count < 0xFFFFU) {
        g_panel_cfg.wrong_code_count++;
    }

    if (g_panel_cfg.wrong_code_count >= CONFIG_DOORBELL_PANEL_LOCKOUT_WRONG_CODES) {
        uint32_t dur_s = CONFIG_DOORBELL_PANEL_LOCKOUT_FIRST_S;
        for (uint16_t i = 0; i < g_panel_cfg.lockout_level; i++) {
            uint64_t next_s = (uint64_t)dur_s * (uint64_t)CONFIG_DOORBELL_PANEL_LOCKOUT_MULTIPLIER;
            if (next_s >= CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S) {
                dur_s = CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S;
                break;
            }
            dur_s = (uint32_t)next_s;
        }
        if (dur_s > CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S) {
            dur_s = CONFIG_DOORBELL_PANEL_LOCKOUT_MAX_S;
        }
        if (g_panel_cfg.lockout_level < 32) {
            g_panel_cfg.lockout_level++;
        }
        g_panel_cfg.lockout_remaining_s = dur_s;
        s_lockout_deadline_us = esp_timer_get_time() + (int64_t)dur_s * 1000000LL;
        doorbell_led_panel_start(LED_PANEL_LOCKOUT);
        ESP_LOGW(TAG, "Keypad lockout activated for %lu s (level %u)",
                 (unsigned long)dur_s, (unsigned)g_panel_cfg.lockout_level);
    }

    doorbell_storage_panel_save_cfg(&g_panel_cfg);
}

static void reset_lockout_on_success_locked(void)
{
    if (g_panel_cfg.wrong_code_count != 0 ||
        g_panel_cfg.lockout_level != 0 ||
        g_panel_cfg.lockout_remaining_s != 0) {
        g_panel_cfg.wrong_code_count = 0;
        g_panel_cfg.lockout_level = 0;
        g_panel_cfg.lockout_remaining_s = 0;
        s_lockout_deadline_us = 0;
        doorbell_led_panel_stop(LED_PANEL_LOCKOUT);
        doorbell_storage_panel_save_cfg(&g_panel_cfg);
    }
}

void panel_lockout_clear_locked(void)
{
    /* Caller holds g_panel_mutex and resets the persisted counters (g_panel_cfg) itself */
    s_lockout_deadline_us = 0;
    s_lockout_persist_divider = 0;
    doorbell_led_panel_stop(LED_PANEL_LOCKOUT);
}

void panel_lockout_tick_1s(void)
{
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    if (s_lockout_deadline_us > 0) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= s_lockout_deadline_us) {
            s_lockout_deadline_us = 0;
            g_panel_cfg.lockout_remaining_s = 0;
            g_panel_cfg.wrong_code_count = 0;
            doorbell_led_panel_stop(LED_PANEL_LOCKOUT);
            doorbell_storage_panel_save_cfg(&g_panel_cfg);
            ESP_LOGI(TAG, "Keypad lockout expired");
        } else {
            uint32_t rem_s = (uint32_t)((s_lockout_deadline_us - now_us + 999999LL) / 1000000LL);
            g_panel_cfg.lockout_remaining_s = rem_s;
            s_lockout_persist_divider++;
            if (s_lockout_persist_divider >= 15) {
                s_lockout_persist_divider = 0;
                doorbell_storage_panel_save_cfg(&g_panel_cfg);
            }
        }
    }
    xSemaphoreGive(g_panel_mutex);
}

static bool ct_equal_code(const char *input, size_t input_len, const char *stored, size_t required_len)
{
    /* Pad both buffers to fixed 12 bytes and compare in constant time + constant length check */
    uint8_t a[12] = {0};
    uint8_t b[12] = {0};
    size_t copy_a = (input_len < sizeof(a)) ? input_len : sizeof(a);
    size_t stored_len = strlen(stored);
    size_t copy_b = (stored_len < sizeof(b)) ? stored_len : sizeof(b);

    memcpy(a, input, copy_a);
    memcpy(b, stored, copy_b);

    int diff = mbedtls_ct_memcmp(a, b, sizeof(a));
    bool len_ok = (input_len == required_len) && (stored_len == required_len);
    mbedtls_platform_zeroize(a, sizeof(a));
    mbedtls_platform_zeroize(b, sizeof(b));
    return (diff == 0) && len_ok;
}

bool panel_verify_admin_code_ct(const char *candidate)
{
    if (!candidate) {
        return false;
    }
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    if (panel_is_locked_out()) {
        xSemaphoreGive(g_panel_mutex);
        return false;
    }
    bool ok = ct_equal_code(candidate,
                            strlen(candidate),
                            g_panel_cfg.admin_code,
                            CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
    if (ok) {
        reset_lockout_on_success_locked();
    } else {
        apply_wrong_code_penalty_locked();
    }
    xSemaphoreGive(g_panel_mutex);
    return ok;
}

static bool is_all_digits(const char *s, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool parse_apt_number(const char *s, size_t len, uint8_t *out_apt)
{
    /* No leading zeros ("0", "01") and value within [MIN_APT_NO .. MAX_APT_NO] (docs/protocol.md §6.2 step 5) */
    if (len == 0 || len > 3 || !is_all_digits(s, len) || s[0] == '0') {
        return false;
    }
    unsigned long val = 0;
    for (size_t i = 0; i < len; i++) {
        val = val * 10UL + (unsigned long)(s[i] - '0');
    }
    if (val < CONFIG_DOORBELL_PANEL_MIN_APT_NO || val > CONFIG_DOORBELL_PANEL_MAX_APT_NO) {
        return false;
    }
    *out_apt = (uint8_t)val;
    return true;
}

/**
 * @brief Evaluate completed '#' sequence in strict deterministic order (docs/protocol.md §6.2, I-20).
 */
static void evaluate_sequence_locked(const char *seq, size_t len)
{
    /* Seq does not include the trailing '#' */
    /* Step 1: Cancel check ("*#") */
    if (len == 1 && seq[0] == '*') {
        if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PORTAL) {
            panel_action_cancel_active_mode();
        }
        return;
    }

    if (len == 0) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Visitor ring candidate (does not start with '*'): always allowed even during lockout */
    if (seq[0] != '*') {
        uint8_t apt_no = 0;
        if (!parse_apt_number(seq, len, &apt_no)) {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            return;
        }
        panel_action_ring_apartment(apt_no);
        return;
    }

    /* Mode check: in MODE_PAIR_DISCOVER, MODE_PAIR_COMPLETE, or MODE_PORTAL, '*' commands other than '*#' are rejected */
    if (g_panel_mode != MODE_IDLE) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Step 2: Lockout gate — reject '*' commands immediately during lockout without inspecting digits (I-20) */
    if (panel_is_locked_out()) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Step 3: Syntax & structure validation BEFORE any secret comparison (I-20)
     * Tokenize by '*' after the leading '*' */
    const char *tok[5] = {0};
    size_t tok_len[5] = {0};
    size_t num_tok = 0;

    const char *p = seq + 1;
    const char *end = seq + len;
    while (p <= end) {
        if (num_tok >= 5) {
            /* Too many '*' segments -> malformed */
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            return;
        }
        const char *star = memchr(p, '*', (size_t)(end - p));
        if (!star) {
            tok[num_tok] = p;
            tok_len[num_tok] = (size_t)(end - p);
            num_tok++;
            break;
        } else {
            tok[num_tok] = p;
            tok_len[num_tok] = (size_t)(star - p);
            num_tok++;
            p = star + 1;
        }
    }

    /* Every token must be non-empty digits */
    for (size_t i = 0; i < num_tok; i++) {
        if (!is_all_digits(tok[i], tok_len[i])) {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            return;
        }
    }

    /* Case A: User door-open candidate: * <digits> # (num_tok == 1) */
    if (num_tok == 1) {
        bool ok = ct_equal_code(tok[0],
                                tok_len[0],
                                g_panel_cfg.user_code,
                                CONFIG_DOORBELL_PANEL_USER_CODE_LEN);
        if (ok) {
            reset_lockout_on_success_locked();
            panel_action_open_door_local();
        } else {
            apply_wrong_code_penalty_locked();
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
        return;
    }

    /* Case B: Admin command candidate: * <admin_digits> * <cmd_id> [ * <arg1> [ * <arg2> ] ] # */
    if (tok_len[1] != 1) {
        /* Malformed cmd_id -> do not increment wrong_code_count */
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    char cmd_id = tok[1][0];
    bool shape_valid = false;
    uint8_t parsed_apt1 = 0;
    uint8_t parsed_apt2 = 0;

    switch (cmd_id) {
        case '1': /* * <admin> * 1 * <apt> # */
            shape_valid = (num_tok == 3) &&
                          parse_apt_number(tok[2], tok_len[2], &parsed_apt1);
            break;
        case '2': /* * <admin> * 2 * <apt1> * <apt2> # (requires apt1 == apt2) */
            shape_valid = (num_tok == 4) &&
                          parse_apt_number(tok[2], tok_len[2], &parsed_apt1) &&
                          parse_apt_number(tok[3], tok_len[3], &parsed_apt2) &&
                          (parsed_apt1 == parsed_apt2);
            break;
        case '3': /* * <admin> * 3 * <new_u1> * <new_u2> # (requires new_1 == new_2) */
            shape_valid = (num_tok == 4) &&
                          (tok_len[2] == CONFIG_DOORBELL_PANEL_USER_CODE_LEN) &&
                          (tok_len[3] == CONFIG_DOORBELL_PANEL_USER_CODE_LEN) &&
                          (mbedtls_ct_memcmp(tok[2], tok[3], CONFIG_DOORBELL_PANEL_USER_CODE_LEN) == 0);
            break;
        case '4': /* * <admin> * 4 * <new_a1> * <new_a2> # (requires new_1 == new_2) */
            shape_valid = (num_tok == 4) &&
                          (tok_len[2] == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN) &&
                          (tok_len[3] == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN) &&
                          (mbedtls_ct_memcmp(tok[2], tok[3], CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN) == 0);
            break;
        case '5': /* * <admin> * 5 # */
            shape_valid = (num_tok == 2);
            break;
        case '6': /* * <admin> * 6 * <admin2> # */
            shape_valid = (num_tok == 3);
            break;
        default:
            shape_valid = false;
            break;
    }

    if (!shape_valid) {
        /* Malformed command template -> error LED without incrementing wrong_code_count (I-20) */
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Step 4: Constant-time admin credential verification */
    bool admin_ok = ct_equal_code(tok[0],
                                  tok_len[0],
                                  g_panel_cfg.admin_code,
                                  CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
    if (!admin_ok) {
        apply_wrong_code_penalty_locked();
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* For command 6 (building reset), the second argument must also match admin_code in constant time */
    if (cmd_id == '6') {
        bool admin2_ok = ct_equal_code(tok[2],
                                       tok_len[2],
                                       g_panel_cfg.admin_code,
                                       CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
        if (!admin2_ok) {
            apply_wrong_code_penalty_locked();
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            return;
        }
    }

    /* Admin authentication passed: reset lockout counters (I-19) */
    reset_lockout_on_success_locked();

    /* Step 5: Code separation check and command execution */
    switch (cmd_id) {
        case '1':
            panel_action_start_pairing(parsed_apt1);
            break;
        case '2':
            panel_action_delete_apartment(parsed_apt1);
            break;
        case '3': {
            if (CONFIG_DOORBELL_PANEL_USER_CODE_LEN == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN &&
                mbedtls_ct_memcmp(tok[2], g_panel_cfg.admin_code, CONFIG_DOORBELL_PANEL_USER_CODE_LEN) == 0) {
                doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
                return;
            }
            /* Commit to NVS first, then to RAM: a failed save must leave the active code unchanged */
            doorbell_panel_cfg_blob_t next = g_panel_cfg;
            memset(next.user_code, 0, sizeof(next.user_code));
            memcpy(next.user_code, tok[2], CONFIG_DOORBELL_PANEL_USER_CODE_LEN);
            if (doorbell_storage_panel_save_cfg(&next) == ESP_OK) {
                g_panel_cfg = next;
                ESP_LOGI(TAG, "User door-open code updated via keypad");
                doorbell_led_panel_start(LED_PANEL_RESULT_OK);
            } else {
                doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            }
            mbedtls_platform_zeroize(&next, sizeof(next));
            break;
        }
        case '4': {
            if (CONFIG_DOORBELL_PANEL_USER_CODE_LEN == CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN &&
                mbedtls_ct_memcmp(tok[2], g_panel_cfg.user_code, CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN) == 0) {
                doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
                return;
            }
            /* Commit to NVS first, then to RAM: a failed save must leave the active code unchanged */
            doorbell_panel_cfg_blob_t next = g_panel_cfg;
            memset(next.admin_code, 0, sizeof(next.admin_code));
            memcpy(next.admin_code, tok[2], CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
            if (doorbell_storage_panel_save_cfg(&next) == ESP_OK) {
                g_panel_cfg = next;
                ESP_LOGI(TAG, "Admin code updated via keypad");
                doorbell_led_panel_start(LED_PANEL_RESULT_OK);
            } else {
                doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            }
            mbedtls_platform_zeroize(&next, sizeof(next));
            break;
        }
        case '5':
            panel_action_start_portal();
            break;
        case '6':
            panel_action_building_reset();
            break;
        default:
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            break;
    }
}

static void matrix_key_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    uintptr_t idx = (uintptr_t)usr_data;
    const char *key_map = CONFIG_DOORBELL_PANEL_KEY_MAP;
    if (idx >= 12) {
        return;
    }
    char ch = key_map[idx];

    doorbell_led_panel_start(LED_PANEL_KEY_PRESS);

    if (!s_key_queue || xQueueSend(s_key_queue, &ch, 0) != pdTRUE) {
        s_key_overflow_pending = true;
    }
}

/* Handles one key in panel_worker_task context (mutex held by caller) */
static void process_key_locked(char ch)
{
    if (g_panel_mode == MODE_RESETTING) {
        return;
    }

    esp_timer_stop(s_input_timeout_timer);

    if (ch == '#') {
        char local_seq[KEYPAD_BUF_MAX];
        size_t local_len = s_key_len;
        memcpy(local_seq, s_key_buf, local_len);
        local_seq[local_len] = '\0';
        clear_keypad_buffer();
        evaluate_sequence_locked(local_seq, local_len);
        mbedtls_platform_zeroize(local_seq, sizeof(local_seq));
    } else {
        if (s_key_len + 1 < KEYPAD_BUF_MAX) {
            s_key_buf[s_key_len++] = ch;
            s_key_buf[s_key_len] = '\0';
            doorbell_led_panel_start(LED_PANEL_TYPING);
            esp_timer_start_once(s_input_timeout_timer,
                                 (uint64_t)CONFIG_DOORBELL_PANEL_INPUT_TIMEOUT_S * 1000000ULL);
        } else {
            /* Buffer overflow protection: clear and flash error */
            clear_keypad_buffer();
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
    }
}

void panel_keypad_process(void)
{
    if (!s_key_queue) {
        return;
    }

    if (atomic_exchange(&s_input_timeout_pending, false)) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        clear_keypad_buffer();
        xSemaphoreGive(g_panel_mutex);
    }

    if (atomic_exchange(&s_key_overflow_pending, false)) {
        /* Keys were dropped: the buffered sequence is no longer what the user typed, so discard it */
        xQueueReset(s_key_queue);
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        clear_keypad_buffer();
        xSemaphoreGive(g_panel_mutex);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    char ch;
    while (xQueueReceive(s_key_queue, &ch, 0) == pdTRUE) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        process_key_locked(ch);
        xSemaphoreGive(g_panel_mutex);
    }
}

/* ---- Hidden Factory-Reset Button on Panel (GPIO 19, 10-60 s window) ---- */

static void panel_reset_btn_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (g_panel_mode == MODE_PORTAL) {
        /* Pressing hidden reset button while portal is open closes portal immediately (docs/protocol.md §5.1).
         * Only a flag is set here; the worker task performs the shutdown. */
        panel_action_cancel_active_mode();
    }
}

static void panel_reset_btn_min_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_panel_start(LED_PANEL_RESET_HOLD);
}

static void panel_reset_btn_max_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_panel_stop(LED_PANEL_RESET_HOLD);
}

static void panel_reset_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    button_handle_t btn = (button_handle_t)button_handle;
    uint32_t held_ms = iot_button_get_pressed_time(btn);
    doorbell_led_panel_stop(LED_PANEL_RESET_HOLD);

    uint32_t min_ms = (uint32_t)CONFIG_DOORBELL_PANEL_RESET_MIN_HOLD_S * 1000U;
    uint32_t max_ms = (uint32_t)CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S * 1000U;

    if (held_ms >= min_ms && held_ms <= max_ms) {
        panel_action_factory_reset();
    }
}

esp_err_t panel_keypad_init(void)
{
    /* Restore persisted lockout if power-cycled during active lockout (I-52) */
    if (g_panel_cfg.lockout_remaining_s > 0) {
        s_lockout_deadline_us = esp_timer_get_time() + (int64_t)g_panel_cfg.lockout_remaining_s * 1000000LL;
        doorbell_led_panel_start(LED_PANEL_LOCKOUT);
        ESP_LOGW(TAG, "Restored active keypad lockout (%lu s remaining)",
                 (unsigned long)g_panel_cfg.lockout_remaining_s);
    }

    s_key_queue = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(char));
    if (!s_key_queue) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t t_args = {
        .callback = &input_timeout_cb,
        .name = "kp_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &s_input_timeout_timer));

    /* 4x3 Matrix Keypad initialization via espressif/button */
    static int32_t row_gpios[4] = {
        CONFIG_DOORBELL_PANEL_GPIO_ROW0,
        CONFIG_DOORBELL_PANEL_GPIO_ROW1,
        CONFIG_DOORBELL_PANEL_GPIO_ROW2,
        CONFIG_DOORBELL_PANEL_GPIO_ROW3,
    };
    static int32_t col_gpios[3] = {
        CONFIG_DOORBELL_PANEL_GPIO_COL0,
        CONFIG_DOORBELL_PANEL_GPIO_COL1,
        CONFIG_DOORBELL_PANEL_GPIO_COL2,
    };

    button_config_t btn_cfg = {0};
    button_matrix_config_t mat_cfg = {
        .row_gpios = row_gpios,
        .col_gpios = col_gpios,
        .row_gpio_num = 4,
        .col_gpio_num = 3,
    };
    button_handle_t mat_btns[12] = {0};
    size_t mat_size = 12; /* Must equal row_gpio_num * col_gpio_num (12) */

    ESP_ERROR_CHECK(iot_button_new_matrix_device(&btn_cfg, &mat_cfg, mat_btns, &mat_size));
    for (size_t i = 0; i < 12; i++) {
        ESP_ERROR_CHECK(iot_button_register_cb(mat_btns[i],
                                               BUTTON_PRESS_DOWN,
                                               NULL,
                                               matrix_key_down_cb,
                                               (void *)(uintptr_t)i));
    }

    /* Hidden factory reset button (GPIO 19) */
    button_config_t rst_cfg = {0};
    button_gpio_config_t rst_gpio = {
        .gpio_num = CONFIG_DOORBELL_PANEL_GPIO_RESET_BTN,
        .active_level = DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW ? 0 : 1,
        .disable_pull = !DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL,
    };
    button_handle_t rst_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&rst_cfg, &rst_gpio, &rst_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(rst_btn, BUTTON_PRESS_DOWN, NULL, panel_reset_btn_down_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(rst_btn, BUTTON_PRESS_UP, NULL, panel_reset_btn_up_cb, NULL));

    button_event_args_t min_args = {.long_press.press_time = CONFIG_DOORBELL_PANEL_RESET_MIN_HOLD_S * 1000};
    button_event_args_t max_args = {.long_press.press_time = CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S * 1000};
    ESP_ERROR_CHECK(iot_button_register_cb(rst_btn, BUTTON_LONG_PRESS_START, &min_args, panel_reset_btn_min_hold_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(rst_btn, BUTTON_LONG_PRESS_START, &max_args, panel_reset_btn_max_hold_cb, NULL));

    return ESP_OK;
}

