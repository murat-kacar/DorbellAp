#include "panel_state.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/constant_time.h"
#include "doorbell_keypad.h"
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
    doorbell_led_stop(LED_PANEL_TYPING);
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
        doorbell_led_start(LED_PANEL_LOCKOUT);
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
        doorbell_led_stop(LED_PANEL_LOCKOUT);
        doorbell_storage_panel_save_cfg(&g_panel_cfg);
    }
}

void panel_lockout_clear_locked(void)
{
    PANEL_ASSERT_LOCKED();
    /* Caller holds g_panel_mutex and resets the persisted counters (g_panel_cfg) itself */
    s_lockout_deadline_us = 0;
    s_lockout_persist_divider = 0;
    doorbell_led_stop(LED_PANEL_LOCKOUT);
}

void panel_lockout_tick_1s(void)
{
    panel_lock();
    if (s_lockout_deadline_us > 0) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= s_lockout_deadline_us) {
            s_lockout_deadline_us = 0;
            g_panel_cfg.lockout_remaining_s = 0;
            g_panel_cfg.wrong_code_count = 0;
            doorbell_led_stop(LED_PANEL_LOCKOUT);
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
    panel_unlock();
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
    panel_lock();
    if (panel_is_locked_out()) {
        panel_unlock();
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
    panel_unlock();
    return ok;
}

static const doorbell_kp_limits_t s_kp_limits = {
    .min_apt_no = CONFIG_DOORBELL_PANEL_MIN_APT_NO,
    .max_apt_no = CONFIG_DOORBELL_PANEL_MAX_APT_NO,
    .user_code_len = CONFIG_DOORBELL_PANEL_USER_CODE_LEN,
    .admin_code_len = CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN,
};

/* Commit a new user or admin code: NVS first, then RAM, so a failed save leaves the active code unchanged */
static void commit_new_code_locked(bool admin, const doorbell_kp_token_t *new_code)
{
    /* Code separation: the new code must differ from the other code (only possible at equal lengths) */
    const char *other = admin ? g_panel_cfg.user_code : g_panel_cfg.admin_code;
    if (strlen(other) == new_code->len && mbedtls_ct_memcmp(new_code->ptr, other, new_code->len) == 0) {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }
    doorbell_panel_cfg_blob_t next = g_panel_cfg;
    char *field = admin ? next.admin_code : next.user_code;
    memset(field, 0, sizeof(next.admin_code));
    memcpy(field, new_code->ptr, new_code->len);
    if (doorbell_storage_panel_save_cfg(&next) == ESP_OK) {
        g_panel_cfg = next;
        ESP_LOGI(TAG, "%s code updated via keypad", admin ? "Admin" : "User door-open");
        doorbell_led_start(LED_PANEL_RESULT_OK);
    } else {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
    mbedtls_platform_zeroize(&next, sizeof(next));
}

/**
 * @brief Evaluate completed '#' sequence in strict deterministic order (docs/protocol.md §6.2, I-20): syntax
 *        (doorbell_keypad_parse), mode check, lockout gate, then constant-time code verification and execution.
 */
static void evaluate_sequence_locked(const char *seq, size_t len)
{
    doorbell_kp_cmd_t cmd;
    doorbell_keypad_parse(seq, len, &s_kp_limits, &cmd);

    /* Step 1: cancel ("*#") */
    if (cmd.kind == DOORBELL_KP_CANCEL) {
        if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PORTAL) {
            panel_action_cancel_active_mode();
        }
        return;
    }

    /* Visitor ring candidate (does not start with '*'): always allowed even during lockout */
    if (!cmd.starred) {
        if (cmd.kind == DOORBELL_KP_RING) {
            panel_action_ring_apartment_locked(cmd.apt_no);
        } else {
            doorbell_led_start(LED_PANEL_RESULT_ERR);
        }
        return;
    }

    /* Mode check: in MODE_PAIR_DISCOVER, MODE_PAIR_COMPLETE, or MODE_PORTAL, '*' commands other than '*#' are rejected.
     * Step 2: lockout gate — reject '*' commands during lockout without inspecting digits (I-20).
     * Step 3: a malformed command template is an error without incrementing wrong_code_count (I-20). */
    if (g_panel_mode != MODE_IDLE || panel_is_locked_out() || cmd.kind == DOORBELL_KP_MALFORMED) {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Case A: user door-open code */
    if (cmd.kind == DOORBELL_KP_USER_CODE) {
        if (ct_equal_code(cmd.code.ptr, cmd.code.len, g_panel_cfg.user_code, CONFIG_DOORBELL_PANEL_USER_CODE_LEN)) {
            reset_lockout_on_success_locked();
            panel_action_open_door_local_locked();
        } else {
            apply_wrong_code_penalty_locked();
            doorbell_led_start(LED_PANEL_RESULT_ERR);
        }
        return;
    }

    /* Case B, step 4: constant-time admin credential verification; building reset needs the admin code twice */
    bool admin_ok = ct_equal_code(cmd.code.ptr, cmd.code.len, g_panel_cfg.admin_code,
                                  CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
    if (admin_ok && cmd.cmd == DOORBELL_KP_CMD_BUILDING_RESET) {
        admin_ok = ct_equal_code(cmd.arg.ptr, cmd.arg.len, g_panel_cfg.admin_code,
                                 CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN);
    }
    if (!admin_ok) {
        apply_wrong_code_penalty_locked();
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Admin authentication passed: reset lockout counters (I-19) */
    reset_lockout_on_success_locked();

    /* Step 5: command execution */
    switch (cmd.cmd) {
    case DOORBELL_KP_CMD_PAIR:
        panel_action_start_pairing(cmd.apt_no);
        break;
    case DOORBELL_KP_CMD_DELETE:
        panel_action_delete_apartment_locked(cmd.apt_no);
        break;
    case DOORBELL_KP_CMD_SET_USER:
        commit_new_code_locked(false, &cmd.arg);
        break;
    case DOORBELL_KP_CMD_SET_ADMIN:
        commit_new_code_locked(true, &cmd.arg);
        break;
    case DOORBELL_KP_CMD_PORTAL:
        panel_action_start_portal();
        break;
    case DOORBELL_KP_CMD_BUILDING_RESET:
        panel_action_building_reset();
        break;
    default:
        doorbell_led_start(LED_PANEL_RESULT_ERR);
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

    doorbell_led_start(LED_PANEL_KEY_PRESS);

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
            doorbell_led_start(LED_PANEL_TYPING);
            esp_timer_start_once(s_input_timeout_timer,
                                 (uint64_t)CONFIG_DOORBELL_PANEL_INPUT_TIMEOUT_S * 1000000ULL);
        } else {
            /* Buffer overflow protection: clear and flash error */
            clear_keypad_buffer();
            doorbell_led_start(LED_PANEL_RESULT_ERR);
        }
    }
}

void panel_keypad_process(void)
{
    if (!s_key_queue) {
        return;
    }

    if (atomic_exchange(&s_input_timeout_pending, false)) {
        panel_lock();
        clear_keypad_buffer();
        panel_unlock();
    }

    if (atomic_exchange(&s_key_overflow_pending, false)) {
        /* Keys were dropped: the buffered sequence is no longer what the user typed, so discard it */
        xQueueReset(s_key_queue);
        panel_lock();
        clear_keypad_buffer();
        panel_unlock();
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    char ch;
    while (xQueueReceive(s_key_queue, &ch, 0) == pdTRUE) {
        panel_lock();
        process_key_locked(ch);
        panel_unlock();
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
    doorbell_led_start(LED_PANEL_RESET_HOLD);
}

static void panel_reset_btn_max_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_stop(LED_PANEL_RESET_HOLD);
}

static void panel_reset_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    button_handle_t btn = (button_handle_t)button_handle;
    uint32_t held_ms = iot_button_get_pressed_time(btn);
    doorbell_led_stop(LED_PANEL_RESET_HOLD);

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
        doorbell_led_start(LED_PANEL_LOCKOUT);
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
    ESP_RETURN_ON_ERROR(esp_timer_create(&t_args, &s_input_timeout_timer), TAG, "keypad init");

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

    ESP_RETURN_ON_ERROR(iot_button_new_matrix_device(&btn_cfg, &mat_cfg, mat_btns, &mat_size), TAG, "keypad init");
    for (size_t i = 0; i < 12; i++) {
        ESP_RETURN_ON_ERROR(iot_button_register_cb(mat_btns[i], BUTTON_PRESS_DOWN, NULL, matrix_key_down_cb,
                                                   (void *)(uintptr_t)i), TAG, "keypad key %u", (unsigned)i);
    }

    /* Hidden factory reset button (GPIO 19) */
    button_config_t rst_cfg = {0};
    button_gpio_config_t rst_gpio = {
        .gpio_num = CONFIG_DOORBELL_PANEL_GPIO_RESET_BTN,
        .active_level = DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW ? 0 : 1,
        .disable_pull = !DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL,
    };
    button_handle_t rst_btn = NULL;
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&rst_cfg, &rst_gpio, &rst_btn), TAG, "keypad init");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(rst_btn, BUTTON_PRESS_DOWN, NULL, panel_reset_btn_down_cb, NULL), TAG,
                        "reset button down");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(rst_btn, BUTTON_PRESS_UP, NULL, panel_reset_btn_up_cb, NULL), TAG,
                        "reset button up");

    button_event_args_t min_args = {.long_press.press_time = CONFIG_DOORBELL_PANEL_RESET_MIN_HOLD_S * 1000};
    button_event_args_t max_args = {.long_press.press_time = CONFIG_DOORBELL_PANEL_RESET_MAX_HOLD_S * 1000};
    ESP_RETURN_ON_ERROR(iot_button_register_cb(rst_btn, BUTTON_LONG_PRESS_START, &min_args, panel_reset_btn_min_hold_cb,
                                               NULL), TAG, "reset button min hold");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(rst_btn, BUTTON_LONG_PRESS_START, &max_args, panel_reset_btn_max_hold_cb,
                                               NULL), TAG, "reset button max hold");

    return ESP_OK;
}
