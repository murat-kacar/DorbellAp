#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/constant_time.h"

#include "iot_button.h"
#include "button_gpio.h"

#include "doorbell_config_checks.h"
#include "doorbell_relay.h"
#include "doorbell_protocol.h"
#include "doorbell_storage.h"
#include "doorbell_led.h"
#include "doorbell_net.h"

static const char *TAG = "doorbell_unit";

/* Runtime state protected by s_state_mutex */
static SemaphoreHandle_t s_state_mutex = NULL;
static doorbell_unit_state_blob_t s_unit_state = {0};
static uint32_t s_runtime_tx_counter = 0;
static uint32_t s_persisted_rx_floor = 0;
static uint16_t s_next_seq_id = 1;
static uint32_t s_known_panel_boot_id = 0;
static doorbell_dedup_cache_t s_last_ring_ack = {0};

/* Telemetry counters (I-58) */
static uint16_t s_ring_count = 0;
static uint16_t s_unacked_open_count = 0;
static uint16_t s_rejected_count = 0;
static uint8_t  s_reset_reason = 0;

/* Coverage and heartbeat tracking (I-41, I-42, I-64, I-65) */
static volatile bool s_sta_has_ip = false;
static volatile uint32_t s_missed_heartbeats = 0;
static volatile int64_t s_next_heartbeat_us = 0;

/* Button & transaction state (I-63) */
static volatile bool s_open_door_pending = false;
static volatile bool s_open_door_in_flight = false;
static volatile bool s_pregen_x25519_requested = false;
static volatile bool s_pairing_requested = false;
static volatile bool s_pairing_active = false;
static volatile bool s_reset_requested = false;
static volatile bool s_pair_btn_down = false;
static volatile bool s_reset_btn_down = false;
static volatile bool s_combo_cancelled = false;

static doorbell_relay_t s_bell_relay = {0};
static doorbell_x25519_ctx_t s_pregen_x25519 = {0};
static esp_task_wdt_user_handle_t s_timer_wdt_user = NULL;
static esp_timer_handle_t s_wdt_periodic_timer = NULL;

static void timer_wdt_feed_cb(void *arg)
{
    (void)arg;
    if (s_timer_wdt_user) {
        esp_task_wdt_reset_user(s_timer_wdt_user);
    }
}

static void on_bell_relay_off(void *ctx)
{
    (void)ctx;
#if CONFIG_DOORBELL_UNIT_VISUAL_BELL
    doorbell_led_unit_stop(LED_UNIT_RINGING);
#endif
}

static void update_steady_led(void)
{
    if (!s_unit_state.paired) {
        doorbell_led_unit_stop(LED_UNIT_IN_COVERAGE);
        doorbell_led_unit_stop(LED_UNIT_OUT_OF_COV);
        doorbell_led_unit_start(LED_UNIT_UNPAIRED);
        return;
    }

    doorbell_led_unit_stop(LED_UNIT_UNPAIRED);
    if (s_sta_has_ip && s_missed_heartbeats < 3) {
        doorbell_led_unit_stop(LED_UNIT_OUT_OF_COV);
        doorbell_led_unit_start(LED_UNIT_IN_COVERAGE);
    } else {
        doorbell_led_unit_stop(LED_UNIT_IN_COVERAGE);
        doorbell_led_unit_start(LED_UNIT_OUT_OF_COV);
    }
}

static void schedule_next_heartbeat(bool initial_after_boot)
{
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
    uint32_t interval_s = s_unit_state.heartbeat_interval_s;
    if (interval_s < 30) {
        interval_s = CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S;
    }
    int64_t now_us = esp_timer_get_time();
    if (initial_after_boot) {
        /* Boot de-synchronization (I-64): uniform random delay in [5 s, interval_s] */
        uint32_t span_ms = (interval_s > 5) ? ((interval_s - 5U) * 1000U) : 5000U;
        uint32_t delay_ms = 5000U + (esp_random() % (span_ms + 1U));
        s_next_heartbeat_us = now_us + (int64_t)delay_ms * 1000LL;
    } else {
        /* Periodic interval +/- 10% random jitter (I-64) */
        int32_t base_ms = (int32_t)(interval_s * 1000U);
        int32_t jitter_span = base_ms / 10;
        int32_t jitter_ms = (jitter_span > 0)
                                ? ((int32_t)(esp_random() % (uint32_t)(2 * jitter_span)) - jitter_span)
                                : 0;
        s_next_heartbeat_us = now_us + (int64_t)(base_ms + jitter_ms) * 1000LL;
    }
#else
    (void)initial_after_boot;
#endif
}

static void on_net_state_changed(bool connected, uint32_t ipv4_host, void *ctx)
{
    (void)ipv4_host;
    (void)ctx;
    s_sta_has_ip = connected;
    if (connected && s_unit_state.paired && !s_pairing_active) {
        if (s_next_heartbeat_us == 0) {
            schedule_next_heartbeat(true);
        }
    }
    if (!s_pairing_active) {
        update_steady_led();
    }
}

/* ---- Button Callbacks (I-63) ---- */

static void door_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    button_handle_t btn = (button_handle_t)button_handle;
    uint32_t held_ms = iot_button_get_pressed_time(btn);

    if (held_ms < CONFIG_DOORBELL_UNIT_DOOR_BTN_MIN_MS ||
        held_ms > (uint32_t)CONFIG_DOORBELL_UNIT_DOOR_BTN_MAX_S * 1000U) {
        return;
    }

    if (!s_unit_state.paired) {
        /* Unpaired door-open press immediately shows no-response blink (I-63) */
        doorbell_led_unit_start(LED_UNIT_NO_RESPONSE);
        return;
    }

    if (s_open_door_in_flight || s_open_door_pending) {
        /* Coalesce duplicate press while transaction is in flight (I-63) */
        return;
    }

    s_open_door_pending = true;
}

static void check_simultaneous_cancel(void)
{
    if (s_pair_btn_down && s_reset_btn_down) {
        s_combo_cancelled = true;
        doorbell_led_unit_stop(LED_UNIT_PAIR_HOLD);
        doorbell_led_unit_stop(LED_UNIT_RESET_HOLD);
        doorbell_led_unit_start(LED_UNIT_BTN_CANCEL);
    }
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
    if (s_combo_cancelled || s_pairing_active) {
        return;
    }
    if (!s_unit_state.paired) {
        doorbell_led_unit_start(LED_UNIT_PAIR_HOLD);
        /* Request X25519 keypair pre-generation on worker task while button is held (I-26) */
        s_pregen_x25519_requested = true;
    }
}

static void pair_btn_max_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    /* Turn OFF hold indicator at 12 s to signal the 7-12 s window has passed (docs/protocol.md §7.2) */
    doorbell_led_unit_stop(LED_UNIT_PAIR_HOLD);
}

static void pair_btn_cancel_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_unit_stop(LED_UNIT_PAIR_HOLD);
    doorbell_led_unit_start(LED_UNIT_BTN_CANCEL);
}

static void pair_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    button_handle_t btn = (button_handle_t)button_handle;
    uint32_t held_ms = iot_button_get_pressed_time(btn);
    s_pair_btn_down = false;
    doorbell_led_unit_stop(LED_UNIT_PAIR_HOLD);

    if (s_combo_cancelled || s_pairing_active) {
        return;
    }

    uint32_t min_ms = (uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S * 1000U;
    uint32_t max_ms = (uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S * 1000U;

    if (held_ms >= min_ms && held_ms <= max_ms) {
        if (s_unit_state.paired) {
            /* Already paired: triple-flash cancel to remind user to reset first (I-63) */
            doorbell_led_unit_start(LED_UNIT_BTN_CANCEL);
        } else {
            s_pairing_requested = true;
        }
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
    if (s_combo_cancelled || s_pairing_active) {
        return;
    }
    doorbell_led_unit_start(LED_UNIT_RESET_HOLD);
}

static void reset_btn_max_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_unit_stop(LED_UNIT_RESET_HOLD);
}

static void reset_btn_cancel_hold_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    doorbell_led_unit_stop(LED_UNIT_RESET_HOLD);
    doorbell_led_unit_start(LED_UNIT_BTN_CANCEL);
}

static void reset_btn_up_cb(void *button_handle, void *usr_data)
{
    (void)usr_data;
    button_handle_t btn = (button_handle_t)button_handle;
    uint32_t held_ms = iot_button_get_pressed_time(btn);
    s_reset_btn_down = false;
    doorbell_led_unit_stop(LED_UNIT_RESET_HOLD);

    if (s_combo_cancelled || s_pairing_active) {
        return;
    }

    uint32_t min_ms = (uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S * 1000U;
    uint32_t max_ms = (uint32_t)CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S * 1000U;

    if (held_ms >= min_ms && held_ms <= max_ms) {
        s_reset_requested = true;
    }
}

static esp_err_t init_unit_buttons(void)
{
    int active_level = CONFIG_DOORBELL_UNIT_BTNS_ACTIVE_LOW ? 0 : 1;
    bool disable_pull = !CONFIG_DOORBELL_UNIT_BTNS_INTERNAL_PULL;

    /* 1. Door-open button (GPIO 23) */
    button_config_t door_btn_cfg = {0};
    button_gpio_config_t door_gpio_cfg = {
        .gpio_num = CONFIG_DOORBELL_UNIT_GPIO_DOOR_BTN,
        .active_level = active_level,
        .disable_pull = disable_pull,
    };
    button_handle_t door_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&door_btn_cfg, &door_gpio_cfg, &door_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(door_btn, BUTTON_PRESS_UP, NULL, door_btn_up_cb, NULL));

    /* 2. Hidden pairing button (GPIO 18) */
    button_config_t pair_btn_cfg = {0};
    button_gpio_config_t pair_gpio_cfg = {
        .gpio_num = CONFIG_DOORBELL_UNIT_GPIO_PAIR_BTN,
        .active_level = active_level,
        .disable_pull = disable_pull,
    };
    button_handle_t pair_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&pair_btn_cfg, &pair_gpio_cfg, &pair_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(pair_btn, BUTTON_PRESS_DOWN, NULL, pair_btn_down_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(pair_btn, BUTTON_PRESS_UP, NULL, pair_btn_up_cb, NULL));

    button_event_args_t min_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_MIN_S * 1000};
    button_event_args_t max_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_MAX_S * 1000};
    button_event_args_t cancel_args = {.long_press.press_time = CONFIG_DOORBELL_UNIT_HOLD_BTN_CANCEL_S * 1000};

    ESP_ERROR_CHECK(iot_button_register_cb(pair_btn, BUTTON_LONG_PRESS_START, &min_args, pair_btn_min_hold_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(pair_btn, BUTTON_LONG_PRESS_START, &max_args, pair_btn_max_hold_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(pair_btn, BUTTON_LONG_PRESS_START, &cancel_args, pair_btn_cancel_hold_cb, NULL));

    /* 3. Hidden reset button (GPIO 19) */
    button_config_t reset_btn_cfg = {0};
    button_gpio_config_t reset_gpio_cfg = {
        .gpio_num = CONFIG_DOORBELL_UNIT_GPIO_RESET_BTN,
        .active_level = active_level,
        .disable_pull = disable_pull,
    };
    button_handle_t reset_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&reset_btn_cfg, &reset_gpio_cfg, &reset_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(reset_btn, BUTTON_PRESS_DOWN, NULL, reset_btn_down_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(reset_btn, BUTTON_PRESS_UP, NULL, reset_btn_up_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(reset_btn, BUTTON_LONG_PRESS_START, &min_args, reset_btn_min_hold_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(reset_btn, BUTTON_LONG_PRESS_START, &max_args, reset_btn_max_hold_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(reset_btn, BUTTON_LONG_PRESS_START, &cancel_args, reset_btn_cancel_hold_cb, NULL));

    return ESP_OK;
}

/* ---- Two-Phase Pairing Client (I-26, I-27, I-30, I-45) ---- */

static void execute_unit_reset(void)
{
    ESP_LOGI(TAG, "Resetting apartment module pairing state and restarting (kullanim-kilavuzu.md §6.2)");
    doorbell_net_disconnect_sta();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    doorbell_storage_unit_erase();
    memset(&s_unit_state, 0, sizeof(s_unit_state));
    memset(&s_last_ring_ack, 0, sizeof(s_last_ring_ack));
    s_runtime_tx_counter = 0;
    s_known_panel_boot_id = 0;
    s_missed_heartbeats = 0;
    s_next_heartbeat_us = 0;
    xSemaphoreGive(s_state_mutex);

    doorbell_net_set_hostname("doorbell-apt-unpaired");
    doorbell_led_unit_start(LED_UNIT_RESULT_OK);
    update_steady_led();
    for (int i = 0; i < 20; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_restart();
}

static void execute_unit_pairing(int udp_sock)
{
    s_pairing_active = true;
    doorbell_led_unit_start(LED_UNIT_PAIRING_SEARCH);
    ESP_LOGI(TAG, "Starting apartment module pairing flow");

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_pregen_x25519.initialized) {
        doorbell_x25519_generate(&s_pregen_x25519);
    }
    xSemaphoreGive(s_state_mutex);

    uint8_t unit_mac[6] = {0};
    doorbell_net_get_sta_mac(unit_mac);

    uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE] = {0};
    doorbell_crypto_random(unit_nonce, sizeof(unit_nonce));

    char pair_ssid[33] = {0};
    esp_err_t err = doorbell_net_scan_and_join_pair_ap(pair_ssid, 8000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not join Doorbell-Pair-* SoftAP: %s", esp_err_to_name(err));
        goto pair_fail;
    }

    uint32_t my_softap_ip = 0;
    if (!doorbell_net_get_sta_ipv4(&my_softap_ip)) {
        goto pair_fail;
    }
    uint32_t panel_softap_ip = (my_softap_ip & 0xFFFFFF00UL) | 1UL;

    doorbell_pair_hello_t hello = {
        .fw_version_major = DOORBELL_FW_VERSION_MAJOR,
        .fw_version_minor = DOORBELL_FW_VERSION_MINOR,
    };
    memcpy(hello.unit_mac, unit_mac, 6);
    memcpy(hello.unit_nonce, unit_nonce, sizeof(unit_nonce));
    memcpy(hello.unit_x25519_pub, s_pregen_x25519.pub_key, DOORBELL_X25519_PUB_SIZE);

    uint8_t hello_pkt[128];
    size_t hello_len = doorbell_pack_pair_hello(&hello, hello_pkt, sizeof(hello_pkt));

    doorbell_pair_challenge_t challenge = {0};
    doorbell_provision_blob_t prov = {0};
    uint8_t k_pair[DOORBELL_KEY_SIZE] = {0};
    bool got_challenge = false;

    int64_t deadline_us = esp_timer_get_time() + 6000000LL; /* 6 s on SoftAP */
    int64_t next_send_us = 0;

    while (esp_timer_get_time() < deadline_us) {
        esp_task_wdt_reset();
        doorbell_relay_safety_check(&s_bell_relay);

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_send_us) {
            doorbell_udp_send_direct(udp_sock, hello_pkt, hello_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
            next_send_us = now_us + 500000LL; /* Repeat every 500 ms */
        }

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) {
            continue;
        }

        if (doorbell_unpack_pair_challenge_header(rx_buf, (size_t)n, &challenge) == ESP_OK) {
            if (doorbell_x25519_derive_kpair(&s_pregen_x25519,
                                             challenge.panel_x25519_pub,
                                             challenge.panel_nonce,
                                             unit_nonce,
                                             challenge.panel_mac,
                                             unit_mac,
                                             challenge.apt_no,
                                             k_pair) == ESP_OK &&
                doorbell_decrypt_provision_blob(rx_buf, (size_t)n, k_pair, &prov) == ESP_OK) {
                got_challenge = true;
                break;
            }
        }
    }

    if (!got_challenge) {
        ESP_LOGW(TAG, "Timed out waiting for valid PAIR_CHALLENGE");
        memset(k_pair, 0, sizeof(k_pair));
        memset(&prov, 0, sizeof(prov));
        goto pair_fail;
    }

    /* Send PAIR_CONFIRM on SoftAP */
    doorbell_pair_confirm_t confirm = {
        .apt_no = challenge.apt_no,
        .confirm_status = DOORBELL_STATUS_OK,
    };
    memcpy(confirm.unit_mac, unit_mac, 6);

    uint8_t confirm_pkt[128];
    size_t confirm_len = 0;
    if (doorbell_pack_pair_confirm(&confirm, k_pair, confirm_pkt, sizeof(confirm_pkt), &confirm_len) == ESP_OK) {
        doorbell_udp_send_direct(udp_sock, confirm_pkt, confirm_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
        vTaskDelay(pdMS_TO_TICKS(60));
        doorbell_udp_send_direct(udp_sock, confirm_pkt, confirm_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
    }
    memset(k_pair, 0, sizeof(k_pair));

    /* Phase 2: Switch to Building AP and send PAIR_COMPLETE over LAN (I-30) */
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "doorbell-apt-%03u", (unsigned)challenge.apt_no);
    doorbell_net_set_hostname(hostname);

    ESP_LOGI(TAG, "Joining building AP '%s' to complete pairing for apartment %u",
             prov.wifi_ssid, (unsigned)challenge.apt_no);
    doorbell_net_connect_sta(prov.wifi_ssid, prov.wifi_pass);

    /* The panel starts its +PAIRING_COMPLETE_TIMEOUT window when it receives PAIR_CONFIRM. Reserve time for the
     * PAIR_COMPLETE send attempts below (4 x ACK timeout) plus a 500 ms safety margin, so a slow DHCP lease cannot
     * push PAIR_COMPLETE past the panel's deadline. */
    const int complete_attempts = 4;
    uint32_t reserve_ms = (uint32_t)complete_attempts * (uint32_t)CONFIG_DOORBELL_ACK_TIMEOUT_MS + 500U;
    uint32_t window_ms = (uint32_t)CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000U;
    uint32_t lan_wait_ms = (window_ms > reserve_ms + 1000U) ? (window_ms - reserve_ms) : 1000U;
    int64_t lan_deadline_us = esp_timer_get_time() + (int64_t)lan_wait_ms * 1000LL;
    uint32_t my_lan_ip = 0;
    while (esp_timer_get_time() < lan_deadline_us) {
        esp_task_wdt_reset();
        doorbell_relay_safety_check(&s_bell_relay);
        if (doorbell_net_wait_for_sta_ip(250) == ESP_OK && doorbell_net_get_sta_ipv4(&my_lan_ip)) {
            break;
        }
    }

    if (my_lan_ip == 0) {
        ESP_LOGW(TAG, "Failed to obtain DHCP lease on building AP within pairing completion window");
        memset(&prov, 0, sizeof(prov));
        goto pair_fail;
    }

    /* Construct PAIR_COMPLETE encrypted under K_apt (tx_counter = 1) */
    doorbell_pl_pair_complete_t pc_pl = {
        .sender_ipv4 = my_lan_ip,
        .fw_version_major = DOORBELL_FW_VERSION_MAJOR,
        .fw_version_minor = DOORBELL_FW_VERSION_MINOR,
    };
    memcpy(pc_pl.unit_mac, unit_mac, 6);

    uint8_t pc_plain[12];
    size_t pc_plain_len = doorbell_pack_pair_complete(&pc_pl, pc_plain);

    uint16_t seq_id = s_next_seq_id++;
    doorbell_hdr_t pc_hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_PAIR_COMPLETE,
        .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
        .src_id = challenge.apt_no,
        .dst_id = DOORBELL_ID_PANEL,
        .seq_id = seq_id,
        .tx_counter = 1,
    };

    uint8_t pc_frame[DOORBELL_MAX_FRAME_SIZE];
    size_t pc_frame_len = 0;
    if (doorbell_frame_encrypt(&pc_hdr, prov.k_apt, pc_plain, pc_plain_len,
                               pc_frame, sizeof(pc_frame), &pc_frame_len) != ESP_OK) {
        memset(&prov, 0, sizeof(prov));
        goto pair_fail;
    }

    bool got_complete_ack = false;
    uint32_t ack_rx_tx_counter = 0;
    for (int attempt = 0; attempt < complete_attempts && !got_complete_ack; attempt++) {
        esp_task_wdt_reset();
        doorbell_udp_send_frame(udp_sock, pc_frame, pc_frame_len, prov.panel_lan_ipv4,
                                CONFIG_DOORBELL_UDP_PORT, (attempt > 0));

        int64_t ack_deadline = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_ACK_TIMEOUT_MS * 1000LL;
        while (esp_timer_get_time() < ack_deadline) {
            esp_task_wdt_reset();
            uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                                 (struct sockaddr *)&from_addr, &from_len);
            if (n < (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
                continue;
            }
            doorbell_hdr_t rx_hdr;
            uint8_t rx_plain[64];
            size_t rx_plain_len = 0;
            if (doorbell_frame_decrypt(rx_buf, (size_t)n, prov.k_apt,
                                       &rx_hdr, rx_plain, sizeof(rx_plain), &rx_plain_len) == ESP_OK &&
                rx_hdr.msg_type == DOORBELL_MSG_PAIR_COMPLETE_ACK &&
                rx_hdr.src_id == DOORBELL_ID_PANEL &&
                rx_hdr.dst_id == challenge.apt_no) {
                doorbell_pl_pair_complete_ack_t ack_pl;
                if (doorbell_unpack_heartbeat_ack(rx_plain, rx_plain_len, &ack_pl) == ESP_OK &&
                    ack_pl.req_tx_counter == 1 &&
                    ack_pl.req_seq_id == seq_id &&
                    ack_pl.status == DOORBELL_STATUS_OK) {
                    s_known_panel_boot_id = ack_pl.panel_boot_id;
                    ack_rx_tx_counter = rx_hdr.tx_counter;
                    uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
                    if (ack_pl.sender_ipv4 == from_ip) {
                        prov.panel_lan_ipv4 = from_ip;
                    }
                    got_complete_ack = true;
                    break;
                }
            }
        }
    }

    if (!got_complete_ack) {
        ESP_LOGW(TAG, "Did not receive PAIR_COMPLETE_ACK from panel — rolling back (I-30)");
        memset(&prov, 0, sizeof(prov));
        goto pair_fail;
    }

    /* Two-Phase Commit succeeded: persist pairing state in NVS (I-30, I-50) */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(&s_unit_state, 0, sizeof(s_unit_state));
    s_unit_state.paired = 1;
    s_unit_state.apt_no = challenge.apt_no;
    memcpy(s_unit_state.panel_mac, challenge.panel_mac, 6);
    s_unit_state.ap_channel = prov.ap_channel;
    strncpy(s_unit_state.wifi_ssid, prov.wifi_ssid, sizeof(s_unit_state.wifi_ssid) - 1);
    strncpy(s_unit_state.wifi_pass, prov.wifi_pass, sizeof(s_unit_state.wifi_pass) - 1);
    s_unit_state.panel_ipv4 = prov.panel_lan_ipv4;
    memcpy(s_unit_state.k_apt, prov.k_apt, DOORBELL_KEY_SIZE);
    s_unit_state.heartbeat_interval_s = prov.heartbeat_interval_s;
    s_unit_state.tx_counter_ceiling = CONFIG_DOORBELL_NVS_COUNTER_BLOCK;
    s_unit_state.rx_floor = ack_rx_tx_counter;
    s_persisted_rx_floor = ack_rx_tx_counter;
    s_runtime_tx_counter = 1;

    esp_err_t save_err = doorbell_storage_unit_save(&s_unit_state);
    doorbell_x25519_free(&s_pregen_x25519);
    xSemaphoreGive(s_state_mutex);
    memset(&prov, 0, sizeof(prov));

    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save pairing state to NVS: %s", esp_err_to_name(save_err));
        goto pair_fail;
    }

    s_missed_heartbeats = 0;
    schedule_next_heartbeat(true);
    s_pairing_active = false;
    doorbell_led_unit_stop(LED_UNIT_PAIRING_SEARCH);
    doorbell_led_unit_start(LED_UNIT_RESULT_OK);
    doorbell_relay_trigger(&s_bell_relay, NULL); /* 1 bell ring on pairing success */
    update_steady_led();
    ESP_LOGI(TAG, "Successfully paired as apartment %u", (unsigned)s_unit_state.apt_no);
    return;

pair_fail:
    doorbell_net_disconnect_sta();
    doorbell_net_set_hostname("doorbell-apt-unpaired");
    s_pairing_active = false;
    doorbell_led_unit_stop(LED_UNIT_PAIRING_SEARCH);
    doorbell_led_unit_start(LED_UNIT_NO_RESPONSE);
    update_steady_led();
}

/* ---- Operational Packet Handling (RING_REQ, OPEN_DOOR_REQ, HEARTBEAT_REQ) ---- */

static void handle_incoming_frame(int udp_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host)
{
    if (!s_unit_state.paired || len < DOORBELL_MIN_FRAME_SIZE) {
        return;
    }

    doorbell_hdr_t clear_hdr;
    if (doorbell_hdr_parse(buf, len, &clear_hdr) != ESP_OK) {
        return;
    }

    /* Step 2: Pre-crypto destination filter (I-53) — ignore frames for other apartments without counting */
    if (clear_hdr.dst_id != s_unit_state.apt_no && clear_hdr.dst_id != DOORBELL_ID_BROADCAST) {
        return;
    }
    if (clear_hdr.src_id != DOORBELL_ID_PANEL || (clear_hdr.flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) != 0) {
        return;
    }

    /* Step 3: Deduplication check (I-29) — re-send cached ACK on duplicate RING_REQ */
    if (clear_hdr.msg_type == DOORBELL_MSG_RING_REQ &&
        s_last_ring_ack.valid &&
        s_last_ring_ack.tx_counter == clear_hdr.tx_counter &&
        s_last_ring_ack.seq_id == clear_hdr.seq_id &&
        mbedtls_ct_memcmp(s_last_ring_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE) == 0) {
        doorbell_udp_send_direct(udp_sock,
                                 s_last_ring_ack.frame,
                                 s_last_ring_ack.frame_len,
                                 from_ip_host,
                                 CONFIG_DOORBELL_UDP_PORT);
        return;
    }

    /* Step 4: Replay check */
    if (clear_hdr.tx_counter <= s_unit_state.rx_floor) {
        if (s_rejected_count < 0xFFFFU) {
            s_rejected_count++;
        }
        return;
    }

    /* Step 5: AEAD decryption and authentication */
    doorbell_hdr_t auth_hdr;
    uint8_t plain[128];
    size_t plain_len = 0;
    if (doorbell_frame_decrypt(buf, len, s_unit_state.k_apt,
                               &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK) {
        if (s_rejected_count < 0xFFFFU) {
            s_rejected_count++;
        }
        return;
    }

    if (auth_hdr.msg_type == DOORBELL_MSG_RING_REQ) {
        doorbell_pl_ring_req_t req;
        if (doorbell_unpack_ring_req(plain, plain_len, &req) != ESP_OK) {
            return;
        }

        /* Step 6: Commit state & authenticated IP update (I-62) */
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_unit_state.rx_floor = auth_hdr.tx_counter;
        if (req.sender_ipv4 == from_ip_host && from_ip_host != 0) {
            s_unit_state.panel_ipv4 = from_ip_host;
        }
        /* Persist the replay floor on EVERY accepted RING (rings are rare, so flash wear is negligible). Otherwise
         * after a module reboot the floor could lag by up to one counter block and a recorded RING_REQ could be
         * replayed to ring the bell again. */
        if (doorbell_storage_unit_save(&s_unit_state) == ESP_OK) {
            s_persisted_rx_floor = s_unit_state.rx_floor;
        }
        s_known_panel_boot_id = req.panel_boot_id;
        s_missed_heartbeats = 0;

        bool already_active = s_bell_relay.is_active;
        uint32_t my_ip = 0;
        doorbell_net_get_sta_ipv4(&my_ip);

        uint32_t ack_counter = 0;
        if (doorbell_storage_unit_next_tx_counter(&s_unit_state,
                                                  &s_runtime_tx_counter,
                                                  CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                  &ack_counter) == ESP_OK) {
            doorbell_pl_ring_ack_t ack_pl = {
                .req_tx_counter = auth_hdr.tx_counter,
                .req_seq_id = auth_hdr.seq_id,
                .status = already_active ? DOORBELL_STATUS_ALREADY_ACTIVE : DOORBELL_STATUS_OK,
                .sender_ipv4 = my_ip,
            };
            uint8_t ack_plain[11];
            size_t ack_plain_len = doorbell_pack_ring_ack(&ack_pl, ack_plain);

            doorbell_hdr_t ack_hdr = {
                .magic = DOORBELL_PROTO_MAGIC,
                .version = DOORBELL_PROTO_VERSION,
                .msg_type = DOORBELL_MSG_RING_ACK,
                .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
                .src_id = s_unit_state.apt_no,
                .dst_id = DOORBELL_ID_PANEL,
                .seq_id = auth_hdr.seq_id,
                .tx_counter = ack_counter,
            };

            if (doorbell_frame_encrypt(&ack_hdr, s_unit_state.k_apt, ack_plain, ack_plain_len,
                                       s_last_ring_ack.frame, sizeof(s_last_ring_ack.frame),
                                       &s_last_ring_ack.frame_len) == ESP_OK) {
                s_last_ring_ack.valid = true;
                s_last_ring_ack.src_id = auth_hdr.src_id;
                s_last_ring_ack.tx_counter = auth_hdr.tx_counter;
                s_last_ring_ack.seq_id = auth_hdr.seq_id;
                memcpy(s_last_ring_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);

                /* Send ACK BEFORE energizing bell relay (docs/protocol.md §4.1) */
                doorbell_udp_send_direct(udp_sock,
                                         s_last_ring_ack.frame,
                                         s_last_ring_ack.frame_len,
                                         from_ip_host,
                                         CONFIG_DOORBELL_UDP_PORT);
            }
        }
        xSemaphoreGive(s_state_mutex);

        /* Trigger bell relay and visual bell LED */
        bool was_active = false;
        if (doorbell_relay_trigger(&s_bell_relay, &was_active) == ESP_OK && !was_active) {
            if (s_ring_count < 0xFFFFU) {
                s_ring_count++;
            }
#if CONFIG_DOORBELL_UNIT_VISUAL_BELL
            doorbell_led_unit_start(LED_UNIT_RINGING);
#endif
        }
        update_steady_led();
    }
}

static void execute_open_door_transaction(int udp_sock)
{
    uint32_t my_ip = 0;
    if (!s_unit_state.paired || !doorbell_net_get_sta_ipv4(&my_ip)) {
        if (s_unacked_open_count < 0xFFFFU) {
            s_unacked_open_count++;
        }
        doorbell_led_unit_start(LED_UNIT_NO_RESPONSE);
        return;
    }

    s_open_door_in_flight = true;
    uint64_t echo_nonce = 0;
    bool success = false;

    /* Allow up to 2 rounds: Round 0 (initial) and Round 1 (transparent STATUS_CHALLENGE response per I-7, I-18) */
    for (int round = 0; round < 2 && !success; round++) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        uint32_t tx_counter = 0;
        esp_err_t cnt_err = doorbell_storage_unit_next_tx_counter(&s_unit_state,
                                                                  &s_runtime_tx_counter,
                                                                  CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                                  &tx_counter);
        uint16_t seq_id = s_next_seq_id++;
        uint32_t target_ip = s_unit_state.panel_ipv4;
        uint8_t k_apt[DOORBELL_KEY_SIZE];
        memcpy(k_apt, s_unit_state.k_apt, DOORBELL_KEY_SIZE);
        uint8_t apt_no = s_unit_state.apt_no;
        uint32_t boot_id = s_known_panel_boot_id;
        xSemaphoreGive(s_state_mutex);

        if (cnt_err != ESP_OK) {
            break;
        }

        doorbell_pl_open_door_req_t req_pl = {
            .sender_ipv4 = my_ip,
            .panel_boot_id = boot_id,
            .echo_nonce = echo_nonce,
        };
        uint8_t req_plain[16];
        size_t req_plain_len = doorbell_pack_open_door_req(&req_pl, req_plain);

        doorbell_hdr_t req_hdr = {
            .magic = DOORBELL_PROTO_MAGIC,
            .version = DOORBELL_PROTO_VERSION,
            .msg_type = DOORBELL_MSG_OPEN_DOOR_REQ,
            .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
            .src_id = apt_no,
            .dst_id = DOORBELL_ID_PANEL,
            .seq_id = seq_id,
            .tx_counter = tx_counter,
        };

        uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        if (doorbell_frame_encrypt(&req_hdr, k_apt, req_plain, req_plain_len,
                                   frame, sizeof(frame), &frame_len) != ESP_OK) {
            break;
        }

        bool got_challenge = false;
        int max_attempts = 1 + CONFIG_DOORBELL_UNIT_OPEN_DOOR_RETRIES;

        for (int attempt = 0; attempt < max_attempts && !success && !got_challenge; attempt++) {
            esp_task_wdt_reset();
            int64_t t_send_us = esp_timer_get_time();
            doorbell_udp_send_frame(udp_sock, frame, frame_len, target_ip,
                                    CONFIG_DOORBELL_UDP_PORT, (attempt > 0));

            int32_t timeout_ms = CONFIG_DOORBELL_ACK_TIMEOUT_MS;
            if (attempt > 0) {
                /* +/- 25 ms jitter on retry (docs/protocol.md §4.1) */
                timeout_ms += (int32_t)(esp_random() % 51U) - 25;
            }
            int64_t deadline_us = t_send_us + (int64_t)timeout_ms * 1000LL;

            while (esp_timer_get_time() < deadline_us) {
                esp_task_wdt_reset();
                doorbell_relay_safety_check(&s_bell_relay);

                uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
                struct sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                                     (struct sockaddr *)&from_addr, &from_len);
                if (n < (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
                    continue;
                }

                uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
                doorbell_hdr_t rx_hdr;
                if (doorbell_hdr_parse(rx_buf, (size_t)n, &rx_hdr) != ESP_OK) {
                    continue;
                }
                if (rx_hdr.msg_type == DOORBELL_MSG_RING_REQ) {
                    handle_incoming_frame(udp_sock, rx_buf, (size_t)n, from_ip);
                    continue;
                }
                if (rx_hdr.dst_id != apt_no || rx_hdr.msg_type != DOORBELL_MSG_OPEN_DOOR_ACK) {
                    continue;
                }
                if (rx_hdr.tx_counter <= s_unit_state.rx_floor) {
                    continue;
                }

                uint8_t rx_plain[64];
                size_t rx_plain_len = 0;
                if (doorbell_frame_decrypt(rx_buf, (size_t)n, k_apt,
                                           &rx_hdr, rx_plain, sizeof(rx_plain), &rx_plain_len) != ESP_OK) {
                    continue;
                }

                doorbell_pl_open_door_ack_t ack_pl;
                if (doorbell_unpack_open_door_ack(rx_plain, rx_plain_len, &ack_pl) != ESP_OK) {
                    continue;
                }
                if (ack_pl.req_tx_counter != tx_counter || ack_pl.req_seq_id != seq_id) {
                    continue;
                }

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_unit_state.rx_floor = rx_hdr.tx_counter;
                bool ip_changed = false;
                if (ack_pl.sender_ipv4 == from_ip && from_ip != 0 && s_unit_state.panel_ipv4 != from_ip) {
                    s_unit_state.panel_ipv4 = from_ip;
                    ip_changed = true;
                }
                if (ip_changed || (s_unit_state.rx_floor - s_persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
                    if (doorbell_storage_unit_save(&s_unit_state) == ESP_OK) {
                        s_persisted_rx_floor = s_unit_state.rx_floor;
                    }
                }
                s_known_panel_boot_id = ack_pl.panel_boot_id;
                s_missed_heartbeats = 0;
                xSemaphoreGive(s_state_mutex);

                if (ack_pl.status == DOORBELL_STATUS_CHALLENGE && round == 0) {
                    echo_nonce = ack_pl.challenge_nonce;
                    got_challenge = true;
                    break;
                } else if (ack_pl.status == DOORBELL_STATUS_OK ||
                           ack_pl.status == DOORBELL_STATUS_ALREADY_ACTIVE) {
                    int64_t rtt_ms = (esp_timer_get_time() - t_send_us) / 1000LL;
                    ESP_LOGI(TAG, "OPEN_DOOR acknowledged after %lld ms", (long long)rtt_ms);
                    success = true;
                    break;
                }
            }
        }

        if (!got_challenge) {
            break;
        }
    }

    s_open_door_in_flight = false;
    if (success) {
        doorbell_led_unit_start(LED_UNIT_RESULT_OK);
        update_steady_led();
    } else {
        if (s_unacked_open_count < 0xFFFFU) {
            s_unacked_open_count++;
        }
        doorbell_led_unit_start(LED_UNIT_NO_RESPONSE);
    }
}

#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
static void execute_heartbeat_transaction(int udp_sock)
{
    uint32_t my_ip = 0;
    if (!s_unit_state.paired || !doorbell_net_get_sta_ipv4(&my_ip)) {
        schedule_next_heartbeat(false);
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint32_t tx_counter = 0;
    esp_err_t cnt_err = doorbell_storage_unit_next_tx_counter(&s_unit_state,
                                                              &s_runtime_tx_counter,
                                                              CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                              &tx_counter);
    uint16_t seq_id = s_next_seq_id++;
    uint32_t target_ip = s_unit_state.panel_ipv4;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    memcpy(k_apt, s_unit_state.k_apt, DOORBELL_KEY_SIZE);
    uint8_t apt_no = s_unit_state.apt_no;
    uint16_t hb_interval_s = s_unit_state.heartbeat_interval_s;
    xSemaphoreGive(s_state_mutex);

    if (cnt_err != ESP_OK) {
        schedule_next_heartbeat(false);
        return;
    }

    int8_t rssi = -127;
    uint8_t ch = 0;
    uint8_t bssid[6] = {0};
    uint16_t disconn_cnt = 0;
    doorbell_net_get_sta_telemetry(&rssi, &ch, bssid, &disconn_cnt);

    doorbell_pl_heartbeat_req_t hb_pl = {
        .sender_ipv4 = my_ip,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .free_heap = (uint32_t)esp_get_free_heap_size(),
        .min_free_heap = (uint32_t)esp_get_minimum_free_heap_size(),
        .ring_count = s_ring_count,
        .unacked_open_count = s_unacked_open_count,
        .rejected_count = s_rejected_count,
        .disconnect_count = disconn_cnt,
        .reset_reason = s_reset_reason,
        .ap_rssi = rssi,
        .ap_channel = ch,
        .fw_version_major = DOORBELL_FW_VERSION_MAJOR,
        .fw_version_minor = DOORBELL_FW_VERSION_MINOR,
        .heartbeat_interval_s = hb_interval_s,
    };
    memcpy(hb_pl.ap_bssid, bssid, 6);

    uint8_t hb_plain[37];
    size_t hb_plain_len = doorbell_pack_heartbeat_req(&hb_pl, hb_plain);

    doorbell_hdr_t hb_hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_HEARTBEAT_REQ,
        .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
        .src_id = apt_no,
        .dst_id = DOORBELL_ID_PANEL,
        .seq_id = seq_id,
        .tx_counter = tx_counter,
    };

    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    if (doorbell_frame_encrypt(&hb_hdr, k_apt, hb_plain, hb_plain_len,
                               frame, sizeof(frame), &frame_len) != ESP_OK) {
        schedule_next_heartbeat(false);
        return;
    }

    bool acked = false;
    int max_attempts = 1 + CONFIG_DOORBELL_HEARTBEAT_RETRIES;

    for (int attempt = 0; attempt < max_attempts && !acked; attempt++) {
        esp_task_wdt_reset();
        doorbell_udp_send_frame(udp_sock, frame, frame_len, target_ip,
                                CONFIG_DOORBELL_UDP_PORT, (attempt > 0));

        int64_t deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_ACK_TIMEOUT_MS * 1000LL;
        while (esp_timer_get_time() < deadline_us) {
            esp_task_wdt_reset();
            doorbell_relay_safety_check(&s_bell_relay);

            uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                                 (struct sockaddr *)&from_addr, &from_len);
            if (n < (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
                continue;
            }

            uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
            doorbell_hdr_t rx_hdr;
            if (doorbell_hdr_parse(rx_buf, (size_t)n, &rx_hdr) != ESP_OK) {
                continue;
            }
            if (rx_hdr.msg_type == DOORBELL_MSG_RING_REQ) {
                handle_incoming_frame(udp_sock, rx_buf, (size_t)n, from_ip);
                continue;
            }
            if (rx_hdr.dst_id != apt_no || rx_hdr.msg_type != DOORBELL_MSG_HEARTBEAT_ACK) {
                continue;
            }
            if (rx_hdr.tx_counter <= s_unit_state.rx_floor) {
                continue;
            }

            uint8_t rx_plain[64];
            size_t rx_plain_len = 0;
            if (doorbell_frame_decrypt(rx_buf, (size_t)n, k_apt,
                                       &rx_hdr, rx_plain, sizeof(rx_plain), &rx_plain_len) != ESP_OK) {
                continue;
            }

            doorbell_pl_heartbeat_ack_t ack_pl;
            if (doorbell_unpack_heartbeat_ack(rx_plain, rx_plain_len, &ack_pl) == ESP_OK &&
                ack_pl.req_tx_counter == tx_counter &&
                ack_pl.req_seq_id == seq_id) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_unit_state.rx_floor = rx_hdr.tx_counter;
                bool ip_changed = false;
                if (ack_pl.sender_ipv4 == from_ip && from_ip != 0 && s_unit_state.panel_ipv4 != from_ip) {
                    s_unit_state.panel_ipv4 = from_ip;
                    ip_changed = true;
                }
                if (ip_changed || (s_unit_state.rx_floor - s_persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
                    if (doorbell_storage_unit_save(&s_unit_state) == ESP_OK) {
                        s_persisted_rx_floor = s_unit_state.rx_floor;
                    }
                }
                s_known_panel_boot_id = ack_pl.panel_boot_id;
                xSemaphoreGive(s_state_mutex);
                acked = true;
                break;
            }
        }
    }

    if (acked) {
        s_missed_heartbeats = 0;
    } else {
        s_missed_heartbeats++;
        ESP_LOGW(TAG, "Missed HEARTBEAT_ACK (consecutive=%lu)", (unsigned long)s_missed_heartbeats);
    }
    update_steady_led();
    schedule_next_heartbeat(false);
}
#endif /* CONFIG_DOORBELL_HEARTBEAT_ENABLED */

static void unit_worker_task(void *arg)
{
    (void)arg;
    /* Subscribe worker task to Task Watchdog (I-46) */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int udp_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, 0, 100);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket on port %d", CONFIG_DOORBELL_UDP_PORT);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        esp_task_wdt_reset();
        doorbell_relay_safety_check(&s_bell_relay);

        if (s_reset_requested) {
            s_reset_requested = false;
            execute_unit_reset();
        }

        if (s_pregen_x25519_requested) {
            s_pregen_x25519_requested = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            doorbell_x25519_generate(&s_pregen_x25519);
            xSemaphoreGive(s_state_mutex);
        }

        if (s_pairing_requested) {
            s_pairing_requested = false;
            execute_unit_pairing(udp_sock);
        }

        if (s_open_door_pending) {
            s_open_door_pending = false;
            execute_open_door_transaction(udp_sock);
        }

#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
        if (s_unit_state.paired && s_sta_has_ip && s_next_heartbeat_us > 0 &&
            esp_timer_get_time() >= s_next_heartbeat_us) {
            execute_heartbeat_transaction(udp_sock);
        }
#endif

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n >= (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
            uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
            handle_incoming_frame(udp_sock, rx_buf, (size_t)n, from_ip);
        }
    }
}

void app_main(void)
{
    s_reset_reason = (uint8_t)esp_reset_reason();
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex != NULL);

    /* 1. Boot-safe Bell Relay initialization BEFORE any other peripheral (I-22) */
    ESP_ERROR_CHECK(doorbell_relay_init(&s_bell_relay,
                                        CONFIG_DOORBELL_UNIT_GPIO_BELL_RELAY,
                                        CONFIG_DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH,
                                        CONFIG_DOORBELL_UNIT_BELL_PULSE_MS,
                                        &on_bell_relay_off,
                                        NULL));

    /* 2. Status LED initialization */
    ESP_ERROR_CHECK(doorbell_led_unit_init(CONFIG_DOORBELL_UNIT_GPIO_STATUS_LED));

    /* 3. NVS storage initialization (strictly no auto-erase on error per I-13) */
    esp_err_t nvs_err = doorbell_storage_init();
    if (nvs_err != ESP_OK) {
        /* Storage fault: never erase (I-13). Keep a persistent error indication instead of a one-shot pattern. */
        for (;;) {
            doorbell_led_unit_start(LED_UNIT_BTN_CANCEL);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    if (doorbell_storage_unit_load(&s_unit_state) == ESP_OK && s_unit_state.paired) {
        /* Resume monotonic counter from reserved ceiling (I-50) */
        s_runtime_tx_counter = s_unit_state.tx_counter_ceiling;
        s_persisted_rx_floor = s_unit_state.rx_floor;
        ESP_LOGI(TAG, "Loaded paired state for apartment %u (last restart: %s)",
                 (unsigned)s_unit_state.apt_no,
                 doorbell_reset_reason_str(s_reset_reason));
    } else {
        memset(&s_unit_state, 0, sizeof(s_unit_state));
        ESP_LOGI(TAG, "Apartment module is currently unpaired (last restart: %s)",
                 doorbell_reset_reason_str(s_reset_reason));
    }

    /* 4. Network & PSA Crypto initialization (I-26, I-38) */
    char hostname[32];
    if (s_unit_state.paired) {
        snprintf(hostname, sizeof(hostname), "doorbell-apt-%03u", (unsigned)s_unit_state.apt_no);
    } else {
        snprintf(hostname, sizeof(hostname), "doorbell-apt-unpaired");
    }
    ESP_ERROR_CHECK(doorbell_net_init(hostname, &on_net_state_changed, NULL));

    update_steady_led();

    if (s_unit_state.paired && s_unit_state.wifi_ssid[0] != '\0') {
        doorbell_net_connect_sta(s_unit_state.wifi_ssid, s_unit_state.wifi_pass);
    }

    /* 5. Buttons initialization */
    ESP_ERROR_CHECK(init_unit_buttons());

    /* 6. Register esp_timer pipeline health handle with TWDT (I-46) */
    ESP_ERROR_CHECK(esp_task_wdt_add_user("unit_esp_timer", &s_timer_wdt_user));
    const esp_timer_create_args_t wdt_timer_args = {
        .callback = &timer_wdt_feed_cb,
        .name = "unit_wdt_tmr",
    };
    ESP_ERROR_CHECK(esp_timer_create(&wdt_timer_args, &s_wdt_periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_wdt_periodic_timer, 1000000ULL));

    /* 7. Start TWDT-monitored UDP worker task */
    xTaskCreate(unit_worker_task, "unit_worker", 6144, NULL, 5, NULL);
}

