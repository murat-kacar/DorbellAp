#include "panel_state.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "doorbell_twdt.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "doorbell_panel";

/* Shared global state */
SemaphoreHandle_t g_panel_mutex = NULL;
doorbell_panel_cfg_blob_t g_panel_cfg = {0};
panel_apt_slot_t g_apt_slots[DOORBELL_MAX_APT_SLOTS] = {0};
_Atomic panel_mode_t g_panel_mode = MODE_IDLE;
uint32_t g_panel_boot_id = 0;
doorbell_relay_t g_door_relay = {0};

/* Persistent silence-restart backoff in RTC_NOINIT memory protected by magic + CRC32 (I-52) */
#define RTC_SILENCE_MAGIC 0x44425254UL /* 'DBRT' */
typedef struct {
    uint32_t magic;
    uint32_t backoff_level;
    uint32_t crc32;
} rtc_silence_state_t;

RTC_NOINIT_ATTR static rtc_silence_state_t s_rtc_silence;
static int64_t s_last_any_module_rx_us = 0;

/* Requests from the keypad, portal, console and button tasks, executed by panel_worker_task. atomic_exchange() takes a
 * request in one step; rings are queued so a second ring entered before the first ran is not lost. */
#define RING_QUEUE_LEN 4
static QueueHandle_t s_ring_queue = NULL;
static atomic_uint_least8_t s_pending_pair_apt = 0;
static atomic_bool s_pending_start_portal = false;
static atomic_bool s_pending_cancel_mode = false;
static atomic_bool s_pending_building_reset = false;
static atomic_bool s_pending_factory_reset = false;

static uint16_t s_panel_next_seq_id = 1;
/* panel_cfg could not be loaded at boot: keypad codes are disabled until re-provisioned (fail closed, I-5) */
static bool s_panel_cfg_fault = false;
static uint32_t s_cfg_fault_led_divider = 0;
static uint32_t s_unregistered_packets = 0;
static uint32_t s_rejected_messages = 0;
/* Two-phase pairing server state (I-26, I-27, I-30, I-45) */
typedef struct {
    uint8_t apt_no;
    int softap_sock;
    uint32_t softap_ip_host;
    int64_t window_deadline_us;
    int64_t collision_deadline_us;
    bool has_candidate;
    bool challenge_sent;
    uint32_t candidate_ip_host;
    doorbell_pair_hello_t hello;
    uint8_t panel_mac[6];
    uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE];
    doorbell_x25519_ctx_t x25519;
    uint8_t k_pair[DOORBELL_KEY_SIZE];
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    /* Byte-identical PAIR_CHALLENGE kept for retransmission when the module repeats PAIR_HELLO */
    uint8_t challenge_pkt[DOORBELL_MAX_FRAME_SIZE];
    size_t challenge_len;
} panel_pair_session_t;

static panel_pair_session_t s_pair_sess = {.softap_sock = -1};

/* ---- RTC Silence Backoff Helpers (I-52) ---- */

static void rtc_silence_init_or_validate(void)
{
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&s_rtc_silence, offsetof(rtc_silence_state_t, crc32));
    if (s_rtc_silence.magic != RTC_SILENCE_MAGIC || s_rtc_silence.crc32 != crc || s_rtc_silence.backoff_level > 16) {
        s_rtc_silence.magic = RTC_SILENCE_MAGIC;
        s_rtc_silence.backoff_level = 0;
        s_rtc_silence.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&s_rtc_silence,
                                               offsetof(rtc_silence_state_t, crc32));
    }
    s_last_any_module_rx_us = esp_timer_get_time();
}

static void rtc_silence_set_level(uint32_t level)
{
    if (level > 16) {
        level = 16;
    }
    if (s_rtc_silence.backoff_level != level || s_rtc_silence.magic != RTC_SILENCE_MAGIC) {
        s_rtc_silence.magic = RTC_SILENCE_MAGIC;
        s_rtc_silence.backoff_level = level;
        s_rtc_silence.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&s_rtc_silence,
                                               offsetof(rtc_silence_state_t, crc32));
    }
}

/* ---- Network & Relay Callbacks ---- */

static void on_door_relay_off(void *ctx)
{
    (void)ctx;
    doorbell_led_panel_stop(LED_PANEL_DOOR_OPEN);
}

static void on_panel_net_state(bool connected, uint32_t ipv4_host, void *ctx)
{
    (void)ipv4_host;
    (void)ctx;
    if (connected) {
        doorbell_led_panel_stop(LED_PANEL_NET_LOST);
    } else {
        doorbell_led_panel_start(LED_PANEL_NET_LOST);
    }
}

/* ---- Pairing Session Cleanup ---- */

static void close_pair_softap_locked(void)
{
    if (s_pair_sess.softap_sock >= 0) {
        close(s_pair_sess.softap_sock);
        s_pair_sess.softap_sock = -1;
    }
    doorbell_net_stop_softap();
    doorbell_net_set_sta_reconnect_paused(false);
}

static void abort_pairing_session_locked(bool show_error_led)
{
    close_pair_softap_locked();
    doorbell_x25519_free(&s_pair_sess.x25519);
    memset(s_pair_sess.k_pair, 0, sizeof(s_pair_sess.k_pair));
    memset(s_pair_sess.k_apt, 0, sizeof(s_pair_sess.k_apt));
    memset(&s_pair_sess, 0, sizeof(s_pair_sess));
    s_pair_sess.softap_sock = -1;

    doorbell_led_panel_stop(LED_PANEL_PAIRING);
    if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PAIR_COMPLETE) {
        g_panel_mode = MODE_IDLE;
    }
    if (show_error_led) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    }
}

/* ---- Actions Invoked by Keypad / Portal / Console ---- */

void panel_action_open_door_local(void)
{
    bool already_active = false;
    if (doorbell_relay_trigger(&g_door_relay, &already_active) == ESP_OK) {
        doorbell_led_panel_start(LED_PANEL_DOOR_OPEN);
        ESP_LOGI(TAG, "Door unlocked via keypad user code");
    } else {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    }
}

void panel_action_ring_apartment(uint8_t apt_no)
{
    if (apt_no < CONFIG_DOORBELL_PANEL_MIN_APT_NO || apt_no > CONFIG_DOORBELL_PANEL_MAX_APT_NO) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }
    if (!g_apt_slots[apt_no].nvs.registered) {
        /* No registered apartment under this number: 1 s fast blink (kullanim-kilavuzu.md §2) */
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }
    if (xQueueSend(s_ring_queue, &apt_no, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Ring request for apartment %u dropped: %d rings already queued", (unsigned)apt_no,
                 RING_QUEUE_LEN);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    }
}

static void execute_start_pairing_locked(uint8_t apt_no)
{
    /* Pre-check (I-31): STA must be connected with valid IPv4 lease and apartment slot must be free */
    uint32_t panel_lan_ip = 0;
    if (g_panel_mode != MODE_IDLE ||
        !doorbell_net_get_sta_ipv4(&panel_lan_ip) ||
        apt_no < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
        apt_no > CONFIG_DOORBELL_PANEL_MAX_APT_NO ||
        g_apt_slots[apt_no].nvs.registered) {
        ESP_LOGW(TAG, "Rejected pairing start for apt %u (mode=%d, lan_ip=0x%08lx, reg=%u)",
                 (unsigned)apt_no, (int)g_panel_mode, (unsigned long)panel_lan_ip,
                 (unsigned)g_apt_slots[apt_no].nvs.registered);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    abort_pairing_session_locked(false);

    uint8_t ap_mac[6] = {0};
    doorbell_net_get_ap_mac(ap_mac);

    char pair_ssid[32];
    snprintf(pair_ssid, sizeof(pair_ssid), "Doorbell-Pair-%02X%02X",
             (unsigned)ap_mac[4], (unsigned)ap_mac[5]);

    doorbell_net_set_sta_reconnect_paused(true);

    uint32_t softap_ip = 0;
    if (doorbell_net_start_softap(pair_ssid, NULL, 1, &softap_ip) != ESP_OK) {
        doorbell_net_set_sta_reconnect_paused(false);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Bind pairing listener strictly to the SoftAP: its IPv4 address AND its netif (SO_BINDTODEVICE), since lwIP's
     * weak host model would otherwise accept datagrams for the SoftAP address arriving from the building LAN (I-27) */
    int pair_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, softap_ip, 50);
    if (pair_sock >= 0 && doorbell_net_bind_socket_to_softap(pair_sock) != ESP_OK) {
        close(pair_sock);
        pair_sock = -1;
    }
    if (pair_sock < 0) {
        doorbell_net_stop_softap();
        doorbell_net_set_sta_reconnect_paused(false);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    s_pair_sess.apt_no = apt_no;
    s_pair_sess.softap_sock = pair_sock;
    s_pair_sess.softap_ip_host = softap_ip;
    s_pair_sess.window_deadline_us = esp_timer_get_time() +
                                     (int64_t)CONFIG_DOORBELL_PAIRING_WINDOW_S * 1000000LL;
    s_pair_sess.collision_deadline_us = 0;
    s_pair_sess.has_candidate = false;
    s_pair_sess.challenge_sent = false;
    memcpy(s_pair_sess.panel_mac, ap_mac, 6);

    g_panel_mode = MODE_PAIR_DISCOVER;
    doorbell_led_panel_start(LED_PANEL_PAIRING);
    ESP_LOGI(TAG, "Opened pairing discovery window for apartment %u on '%s' (%d s)",
             (unsigned)apt_no, pair_ssid, CONFIG_DOORBELL_PAIRING_WINDOW_S);
}

void panel_action_start_pairing(uint8_t apt_no)
{
    atomic_store(&s_pending_pair_apt, apt_no);
}

void panel_action_start_portal(void)
{
    atomic_store(&s_pending_start_portal, true);
}

void panel_action_cancel_active_mode(void)
{
    atomic_store(&s_pending_cancel_mode, true);
}

void panel_action_delete_apartment(uint8_t apt_no)
{
    if (apt_no < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
        apt_no > CONFIG_DOORBELL_PANEL_MAX_APT_NO ||
        !g_apt_slots[apt_no].nvs.registered) {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    if (doorbell_storage_panel_erase_apt(apt_no) == ESP_OK) {
        memset(&g_apt_slots[apt_no], 0, sizeof(g_apt_slots[apt_no]));
        ESP_LOGI(TAG, "Deleted registration for apartment %u", (unsigned)apt_no);
        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
    } else {
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    }
}

void panel_action_building_reset(void)
{
    g_panel_mode = MODE_RESETTING;
    atomic_store(&s_pending_building_reset, true);
}

void panel_action_factory_reset(void)
{
    atomic_store(&s_pending_factory_reset, true);
}

/* After a reset changed NVS: show the outcome for 2 s, then restart so RAM is rebuilt from what NVS actually holds */
[[noreturn]] static void show_result_and_restart(bool ok)
{
    doorbell_led_panel_start(ok ? LED_PANEL_RESULT_OK : LED_PANEL_RESULT_ERR);
    for (int i = 0; i < 20; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_restart();
}

static void execute_pending_panel_actions(void)
{
    if (atomic_exchange(&s_pending_building_reset, false)) {
        ESP_LOGW(TAG, "Executing full building reset (*<admin>*6*<admin>#)");
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        abort_pairing_session_locked(false);
        panel_portal_stop();
        /* Building reset erases apartment registrations and keys only; user/admin codes and building Wi-Fi
         * credentials are preserved (kullanim-kilavuzu.md §6.3) */
        esp_err_t err = doorbell_storage_panel_erase_all_apts(CONFIG_DOORBELL_PANEL_MIN_APT_NO,
                                                              CONFIG_DOORBELL_PANEL_MAX_APT_NO);
        memset(g_apt_slots, 0, sizeof(g_apt_slots));
        xSemaphoreGive(g_panel_mutex);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Building reset incomplete (%s); restarting to reload the stored registrations",
                     esp_err_to_name(err));
        }
        show_result_and_restart(err == ESP_OK);
    }

    if (atomic_exchange(&s_pending_factory_reset, false)) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        if (g_panel_mode == MODE_PAIR_DISCOVER) {
            abort_pairing_session_locked(false);
        } else if (g_panel_mode == MODE_PORTAL) {
            panel_portal_stop();
        }
        /* Back to the provisioned state (Burn / Flash): factory codes, factory building Wi-Fi, no lockout, no
         * apartment registrations. Storage commits panel_cfg first and erases registrations only after that */
        bool nvs_changed = false;
        esp_err_t err = doorbell_storage_panel_factory_reset(&g_panel_cfg, CONFIG_DOORBELL_PANEL_MIN_APT_NO,
                                                            CONFIG_DOORBELL_PANEL_MAX_APT_NO, &nvs_changed);
        if (nvs_changed) {
            panel_lockout_clear_locked();
            memset(g_apt_slots, 0, sizeof(g_apt_slots)); /* never serve a half-erased set until the restart */
        }
        xSemaphoreGive(g_panel_mutex);

        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Factory reset via hidden button: codes, building Wi-Fi and registrations restored to the "
                          "provisioned state; restarting");
        } else {
            ESP_LOGE(TAG, "Factory reset failed (%s)%s", esp_err_to_name(err),
                     nvs_changed ? "; restarting to reload the stored state (press the button again to retry)"
                                 : "; nothing changed");
        }
        if (nvs_changed) {
            show_result_and_restart(err == ESP_OK); /* the station reconnects with the factory Wi-Fi */
        } else {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
    }

    if (atomic_exchange(&s_pending_cancel_mode, false)) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        if (g_panel_mode == MODE_PAIR_DISCOVER) {
            ESP_LOGI(TAG, "Pairing discovery cancelled by user");
            abort_pairing_session_locked(false);
        } else if (g_panel_mode == MODE_PORTAL) {
            panel_portal_stop();
        }
        xSemaphoreGive(g_panel_mutex);
    }

    if (atomic_exchange(&s_pending_start_portal, false)) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        esp_err_t err = panel_portal_start();
        xSemaphoreGive(g_panel_mutex);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Setup portal could not start: %s", esp_err_to_name(err));
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
    }

    uint8_t pair_apt = atomic_exchange(&s_pending_pair_apt, 0);
    if (pair_apt != 0) {
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        execute_start_pairing_locked(pair_apt);
        xSemaphoreGive(g_panel_mutex);
    }
}

/* ---- Two-Phase Pairing State Machine Step (I-27, I-30, I-45) ---- */

static void step_pairing_discovery(void)
{
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);

    if (g_panel_mode == MODE_PAIR_COMPLETE) {
        if (esp_timer_get_time() >= s_pair_sess.window_deadline_us) {
            ESP_LOGW(TAG, "Timed out waiting for PAIR_COMPLETE on LAN for apartment %u — rolling back (I-30)",
                     (unsigned)s_pair_sess.apt_no);
            abort_pairing_session_locked(true);
        }
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    if (g_panel_mode != MODE_PAIR_DISCOVER) {
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us >= s_pair_sess.window_deadline_us) {
        ESP_LOGW(TAG, "Pairing discovery window expired for apartment %u", (unsigned)s_pair_sess.apt_no);
        abort_pairing_session_locked(true);
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    /* Multi-station collision check on SoftAP (I-30) */
    if (doorbell_net_get_softap_sta_count() > 1) {
        ESP_LOGW(TAG, "Multiple stations associated to pairing SoftAP — aborting pairing");
        abort_pairing_session_locked(true);
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    /* Receive packets on SoftAP-bound UDP socket */
    if (s_pair_sess.softap_sock >= 0) {
        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(s_pair_sess.softap_sock, rx_buf, sizeof(rx_buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n > 0) {
            uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
            /* Verify source IP belongs to SoftAP /24 subnet (I-27) */
            if ((from_ip & 0xFFFFFF00UL) == (s_pair_sess.softap_ip_host & 0xFFFFFF00UL)) {
                doorbell_pair_hello_t hello;
                if (doorbell_unpack_pair_hello(rx_buf, (size_t)n, &hello) == ESP_OK) {
                    if (!s_pair_sess.has_candidate) {
                        s_pair_sess.has_candidate = true;
                        s_pair_sess.candidate_ip_host = from_ip;
                        s_pair_sess.hello = hello;
                        s_pair_sess.collision_deadline_us = now_us + 1500000LL; /* 1.5 s collision window */
                        ESP_LOGI(TAG, "Received PAIR_HELLO candidate; waiting 1.5 s collision window");
                    } else if (memcmp(s_pair_sess.hello.unit_mac, hello.unit_mac, 6) != 0) {
                        ESP_LOGW(TAG, "Collision detected: second module sent PAIR_HELLO — aborting");
                        abort_pairing_session_locked(true);
                        xSemaphoreGive(g_panel_mutex);
                        return;
                    } else {
                        s_pair_sess.candidate_ip_host = from_ip;
                        /* Same module repeated PAIR_HELLO after PAIR_CHALLENGE was sent: the challenge was lost,
                         * so retransmit the identical frame (the module keeps its keypair/nonce until timeout) */
                        if (s_pair_sess.challenge_sent && s_pair_sess.challenge_len > 0 &&
                            memcmp(s_pair_sess.hello.unit_nonce, hello.unit_nonce, DOORBELL_PAIR_NONCE_SIZE) == 0) {
                            doorbell_udp_send_direct(s_pair_sess.softap_sock,
                                                     s_pair_sess.challenge_pkt,
                                                     s_pair_sess.challenge_len,
                                                     from_ip,
                                                     CONFIG_DOORBELL_UDP_PORT);
                        }
                    }
                } else if (s_pair_sess.challenge_sent) {
                    doorbell_pair_confirm_t confirm;
                    if (doorbell_unpack_pair_confirm(rx_buf, (size_t)n, s_pair_sess.k_pair, &confirm) == ESP_OK &&
                        confirm.apt_no == s_pair_sess.apt_no &&
                        memcmp(confirm.unit_mac, s_pair_sess.hello.unit_mac, 6) == 0 &&
                        confirm.confirm_status == DOORBELL_STATUS_OK) {
                        /* Phase 1 complete: close SoftAP immediately and wait +10 s for PAIR_COMPLETE on LAN (I-30) */
                        close_pair_softap_locked();
                        s_pair_sess.window_deadline_us = esp_timer_get_time() +
                                                         (int64_t)CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000000LL;
                        g_panel_mode = MODE_PAIR_COMPLETE;
                        ESP_LOGI(TAG, "Received PAIR_CONFIRM for apt %u; waiting up to %d s for LAN PAIR_COMPLETE",
                                 (unsigned)s_pair_sess.apt_no, CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S);
                        xSemaphoreGive(g_panel_mutex);
                        return;
                    }
                }
            }
        }
    }

    /* Send PAIR_CHALLENGE after the 1.5 s collision window passes with a single module */
    if (s_pair_sess.has_candidate && !s_pair_sess.challenge_sent &&
        now_us >= s_pair_sess.collision_deadline_us) {
        uint32_t panel_lan_ip = 0;
        if (!doorbell_net_get_sta_ipv4(&panel_lan_ip)) {
            abort_pairing_session_locked(true);
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        if (doorbell_x25519_generate(&s_pair_sess.x25519) != ESP_OK ||
            doorbell_crypto_random(s_pair_sess.panel_nonce, sizeof(s_pair_sess.panel_nonce)) != ESP_OK ||
            doorbell_crypto_random(s_pair_sess.k_apt, sizeof(s_pair_sess.k_apt)) != ESP_OK ||
            doorbell_x25519_derive_kpair(&s_pair_sess.x25519,
                                         s_pair_sess.hello.unit_x25519_pub,
                                         s_pair_sess.panel_nonce,
                                         s_pair_sess.hello.unit_nonce,
                                         s_pair_sess.panel_mac,
                                         s_pair_sess.hello.unit_mac,
                                         s_pair_sess.apt_no,
                                         s_pair_sess.k_pair) != ESP_OK) {
            abort_pairing_session_locked(true);
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        int8_t rssi = 0;
        uint8_t ch = 1;
        uint8_t bssid[6] = {0};
        doorbell_net_get_sta_telemetry(&rssi, &ch, bssid, NULL);

        doorbell_provision_blob_t blob = {0};
        strncpy(blob.wifi_ssid, g_panel_cfg.wifi_ssid, sizeof(blob.wifi_ssid) - 1);
        strncpy(blob.wifi_pass, g_panel_cfg.wifi_pass, sizeof(blob.wifi_pass) - 1);
        blob.ap_channel = ch;
        blob.panel_lan_ipv4 = panel_lan_ip;
        memcpy(blob.k_apt, s_pair_sess.k_apt, DOORBELL_KEY_SIZE);
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
        blob.heartbeat_interval_s = CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S;
#else
        blob.heartbeat_interval_s = 300;
#endif

        uint8_t ch_pkt[DOORBELL_MAX_FRAME_SIZE];
        size_t ch_len = 0;
        esp_err_t pack_err = doorbell_pack_pair_challenge(s_pair_sess.panel_mac,
                                                          s_pair_sess.panel_nonce,
                                                          s_pair_sess.x25519.pub_key,
                                                          s_pair_sess.apt_no,
                                                          &blob,
                                                          s_pair_sess.k_pair,
                                                          ch_pkt,
                                                          sizeof(ch_pkt),
                                                          &ch_len);
        mbedtls_platform_zeroize(&blob, sizeof(blob));

        if (pack_err == ESP_OK) {
            memcpy(s_pair_sess.challenge_pkt, ch_pkt, ch_len);
            s_pair_sess.challenge_len = ch_len;
            doorbell_udp_send_direct(s_pair_sess.softap_sock,
                                     ch_pkt,
                                     ch_len,
                                     s_pair_sess.candidate_ip_host,
                                     CONFIG_DOORBELL_UDP_PORT);
            s_pair_sess.challenge_sent = true;
            ESP_LOGI(TAG, "Sent PAIR_CHALLENGE to candidate module for apartment %u",
                     (unsigned)s_pair_sess.apt_no);
        }
    }

    xSemaphoreGive(g_panel_mutex);
}

/* ---- Module frames: dispatch under g_panel_mutex, side effects (relay, logging) after releasing it ---- */

/* Work of a handled frame that must run after g_panel_mutex is released */
typedef struct {
    bool open_door;
    bool heartbeat_valid;
    doorbell_pl_heartbeat_req_t heartbeat;
} lan_followup_t;

/* Accepted authenticated message: advance the replay floor, learn a changed module IP and persist both in
 * write-ahead blocks (I-50). Caller holds g_panel_mutex. */
static void slot_accept_rx_locked(panel_apt_slot_t *slot, uint32_t rx_counter, uint32_t sender_ipv4,
                                  uint32_t from_ip_host)
{
    slot->nvs.rx_floor = rx_counter;
    bool ip_changed = false;
    if (sender_ipv4 == from_ip_host && from_ip_host != 0 && slot->nvs.last_ipv4 != from_ip_host) {
        slot->nvs.last_ipv4 = from_ip_host;
        ip_changed = true;
    }
    if (ip_changed || (slot->nvs.rx_floor - slot->persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
        if (doorbell_storage_panel_save_apt(&slot->nvs) == ESP_OK) {
            slot->persisted_rx_floor = slot->nvs.rx_floor;
        }
    }
}

/* Encrypt the ACK for @p req_hdr into the slot's deduplication cache (I-29) and send it. The cache keeps the request's
 * CCM tag so only a byte-identical retry is answered from it. Caller holds g_panel_mutex. */
static void send_ack_locked(int lan_sock, panel_apt_slot_t *slot, uint8_t apt_no, uint8_t ack_msg_type,
                            const doorbell_hdr_t *req_hdr, const uint8_t *req_frame, size_t req_len,
                            uint32_t ack_counter, const uint8_t *ack_plain, size_t ack_plain_len,
                            uint32_t to_ip_host)
{
    doorbell_hdr_t ack_hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = ack_msg_type,
        .flags = 0,
        .src_id = DOORBELL_ID_PANEL,
        .dst_id = apt_no,
        .seq_id = req_hdr->seq_id,
        .tx_counter = ack_counter,
    };
    if (doorbell_frame_encrypt(&ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len,
                               slot->last_ack.frame, sizeof(slot->last_ack.frame),
                               &slot->last_ack.frame_len) != ESP_OK) {
        return;
    }
    slot->last_ack.valid = true;
    slot->last_ack.src_id = apt_no;
    slot->last_ack.tx_counter = req_hdr->tx_counter;
    slot->last_ack.seq_id = req_hdr->seq_id;
    memcpy(slot->last_ack.req_tag, req_frame + req_len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);
    doorbell_udp_send_direct(lan_sock, slot->last_ack.frame, slot->last_ack.frame_len, to_ip_host,
                             CONFIG_DOORBELL_UDP_PORT);
}

/* PAIR_COMPLETE from the module being paired: two-phase commit of the new registration (I-30, I-45, I-50).
 * Caller holds g_panel_mutex. */
static void handle_pair_complete_locked(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host,
                                        uint8_t apt_no)
{
    doorbell_hdr_t auth_hdr;
    uint8_t plain[64];
    size_t plain_len = 0;
    if (doorbell_frame_decrypt(buf, len, s_pair_sess.k_apt, &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK) {
        return;
    }
    doorbell_pl_pair_complete_t pc;
    if (doorbell_unpack_pair_complete(plain, plain_len, &pc) != ESP_OK ||
        memcmp(pc.unit_mac, s_pair_sess.hello.unit_mac, 6) != 0) {
        return;
    }

    /* Build the record in a scratch copy and publish it to g_apt_slots only after nvs_commit() succeeds, so a failed
     * write never leaves a RAM-only registration behind */
    doorbell_panel_apt_blob_t new_rec = {0};
    new_rec.apt_no = apt_no;
    new_rec.registered = 1;
    memcpy(new_rec.unit_mac, pc.unit_mac, 6);
    memcpy(new_rec.k_apt, s_pair_sess.k_apt, DOORBELL_KEY_SIZE);
    new_rec.last_ipv4 = (pc.sender_ipv4 == from_ip_host) ? from_ip_host : 0;
    new_rec.tx_counter_ceiling = CONFIG_DOORBELL_NVS_COUNTER_BLOCK;
    new_rec.rx_floor = auth_hdr.tx_counter;

    esp_err_t commit_err = doorbell_storage_panel_save_apt(&new_rec);
    if (commit_err != ESP_OK) {
        mbedtls_platform_zeroize(&new_rec, sizeof(new_rec));
        ESP_LOGE(TAG, "NVS commit of apartment %u failed (%s) — pairing rolled back",
                 (unsigned)apt_no, esp_err_to_name(commit_err));
        abort_pairing_session_locked(true);
        return;
    }

    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    memset(slot, 0, sizeof(*slot));
    slot->nvs = new_rec;
    mbedtls_platform_zeroize(&new_rec, sizeof(new_rec));
    slot->persisted_rx_floor = auth_hdr.tx_counter;
    slot->runtime_tx_counter = 0;
    slot->last_heartbeat_us = esp_timer_get_time();

    /* Erase any older apartment slot sharing the same unit_mac ONLY after the new commit succeeded (I-30, I-45) */
    for (uint8_t old_apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; old_apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; old_apt++) {
        if (old_apt != apt_no && g_apt_slots[old_apt].nvs.registered &&
            memcmp(g_apt_slots[old_apt].nvs.unit_mac, pc.unit_mac, 6) == 0) {
            doorbell_storage_panel_erase_apt(old_apt);
            memset(&g_apt_slots[old_apt], 0, sizeof(g_apt_slots[old_apt]));
            ESP_LOGI(TAG, "Removed previous registration of same unit_mac from apartment %u", (unsigned)old_apt);
        }
    }

    uint32_t panel_ip = 0;
    doorbell_net_get_sta_ipv4(&panel_ip);
    uint32_t ack_cnt = 0;
    if (doorbell_storage_panel_next_tx_counter(&slot->nvs, &slot->runtime_tx_counter,
                                               CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &ack_cnt) == ESP_OK) {
        doorbell_pl_pair_complete_ack_t ack_pl = {
            .req_tx_counter = auth_hdr.tx_counter,
            .req_seq_id = auth_hdr.seq_id,
            .status = DOORBELL_STATUS_OK,
            .sender_ipv4 = panel_ip,
            .panel_boot_id = g_panel_boot_id,
        };
        uint8_t ack_plain[15];
        size_t ack_plain_len = doorbell_pack_heartbeat_ack(&ack_pl, ack_plain); /* same payload layout */
        send_ack_locked(lan_sock, slot, apt_no, DOORBELL_MSG_PAIR_COMPLETE_ACK, &auth_hdr, buf, len, ack_cnt,
                        ack_plain, ack_plain_len, from_ip_host);
    }

    s_last_any_module_rx_us = esp_timer_get_time();
    rtc_silence_set_level(0);
    abort_pairing_session_locked(false);
    doorbell_led_panel_start(LED_PANEL_RESULT_OK);
    ESP_LOGI(TAG, "Completed two-phase pairing for apartment %u (fw v%u.%u)",
             (unsigned)apt_no, (unsigned)pc.fw_version_major, (unsigned)pc.fw_version_minor);
}

/* OPEN_DOOR_REQ with reboot-replay / withheld-packet freshness check (I-7, I-18): an unverified request gets a
 * STATUS_CHALLENGE ACK, a request echoing the live challenge opens the door. Caller holds g_panel_mutex.
 * @return true if the door relay must be energized (by the caller, after releasing the mutex) */
static bool handle_open_door_locked(int lan_sock, panel_apt_slot_t *slot, uint8_t apt_no,
                                    const doorbell_hdr_t *auth_hdr, const uint8_t *plain, size_t plain_len,
                                    const uint8_t *buf, size_t len, uint32_t from_ip_host, uint32_t panel_ip,
                                    int64_t now_us)
{
    doorbell_pl_open_door_req_t req;
    if (doorbell_unpack_open_door_req(plain, plain_len, &req) != ESP_OK) {
        return false;
    }
    slot_accept_rx_locked(slot, auth_hdr->tx_counter, req.sender_ipv4, from_ip_host);
    slot->last_heartbeat_us = now_us;
    slot->no_report_warned = false;

    bool challenge_verified = (req.panel_boot_id == g_panel_boot_id) &&
                              (req.echo_nonce != 0) &&
                              (req.echo_nonce == slot->active_challenge_nonce) &&
                              (now_us <= slot->challenge_deadline_us);

    uint32_t ack_cnt = 0;
    if (doorbell_storage_panel_next_tx_counter(&slot->nvs, &slot->runtime_tx_counter,
                                               CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &ack_cnt) != ESP_OK) {
        return false;
    }

    doorbell_pl_open_door_ack_t ack_pl = {
        .req_tx_counter = auth_hdr->tx_counter,
        .req_seq_id = auth_hdr->seq_id,
        .sender_ipv4 = panel_ip,
        .panel_boot_id = g_panel_boot_id,
        .challenge_nonce = 0,
    };
    bool open_door = false;
    if (!challenge_verified) {
        /* 64-bit challenge valid for 2000 ms; the door stays locked until it is echoed */
        uint64_t chal = 0;
        doorbell_crypto_random((uint8_t *)&chal, sizeof(chal));
        if (chal == 0) {
            chal = 1;
        }
        slot->active_challenge_nonce = chal;
        slot->challenge_deadline_us = now_us + 2000000LL;
        ack_pl.status = DOORBELL_STATUS_CHALLENGE;
        ack_pl.challenge_nonce = chal;
    } else {
        /* Challenge consumed; the ACK goes out before the relay is energized */
        slot->active_challenge_nonce = 0;
        slot->challenge_deadline_us = 0;
        ack_pl.status = g_door_relay.is_active ? DOORBELL_STATUS_ALREADY_ACTIVE : DOORBELL_STATUS_OK;
        open_door = true;
    }
    uint8_t ack_plain[23];
    size_t ack_plain_len = doorbell_pack_open_door_ack(&ack_pl, ack_plain);
    send_ack_locked(lan_sock, slot, apt_no, DOORBELL_MSG_OPEN_DOOR_ACK, auth_hdr, buf, len, ack_cnt,
                    ack_plain, ack_plain_len, from_ip_host);
    return open_door;
}

/* HEARTBEAT_REQ: liveness, replay floor, telemetry (I-52). Caller holds g_panel_mutex.
 * @return true if @p out_hb holds a valid report to log */
static bool handle_heartbeat_locked(int lan_sock, panel_apt_slot_t *slot, uint8_t apt_no,
                                    const doorbell_hdr_t *auth_hdr, const uint8_t *plain, size_t plain_len,
                                    const uint8_t *buf, size_t len, uint32_t from_ip_host, uint32_t panel_ip,
                                    int64_t now_us, doorbell_pl_heartbeat_req_t *out_hb)
{
    if (doorbell_unpack_heartbeat_req(plain, plain_len, out_hb) != ESP_OK) {
        return false;
    }
    slot_accept_rx_locked(slot, auth_hdr->tx_counter, out_hb->sender_ipv4, from_ip_host);
    slot->last_heartbeat_us = now_us;
    slot->no_report_warned = false;

    uint32_t ack_cnt = 0;
    if (doorbell_storage_panel_next_tx_counter(&slot->nvs, &slot->runtime_tx_counter,
                                               CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &ack_cnt) == ESP_OK) {
        doorbell_pl_heartbeat_ack_t ack_pl = {
            .req_tx_counter = auth_hdr->tx_counter,
            .req_seq_id = auth_hdr->seq_id,
            .status = DOORBELL_STATUS_OK,
            .sender_ipv4 = panel_ip,
            .panel_boot_id = g_panel_boot_id,
        };
        uint8_t ack_plain[15];
        size_t ack_plain_len = doorbell_pack_heartbeat_ack(&ack_pl, ack_plain);
        send_ack_locked(lan_sock, slot, apt_no, DOORBELL_MSG_HEARTBEAT_ACK, auth_hdr, buf, len, ack_cnt,
                        ack_plain, ack_plain_len, from_ip_host);
    }
    return true;
}

/* One module frame after the pre-crypto filter: pairing commit, deduplication (I-29), replay check, AEAD, then the
 * message handler. Caller holds g_panel_mutex; post-unlock work is returned in @p follow. */
static void handle_frame_locked(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host,
                                const doorbell_hdr_t *clear_hdr, lan_followup_t *follow)
{
    uint8_t apt_no = clear_hdr->src_id;

    /* PAIR_COMPLETE of the running pairing session (MODE_PAIR_COMPLETE) */
    if (clear_hdr->msg_type == DOORBELL_MSG_PAIR_COMPLETE && g_panel_mode == MODE_PAIR_COMPLETE &&
        s_pair_sess.apt_no == apt_no) {
        handle_pair_complete_locked(lan_sock, buf, len, from_ip_host, apt_no);
        return;
    }

    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    if (!slot->nvs.registered) {
        s_unregistered_packets++;
        if ((s_unregistered_packets & 0x1FU) == 1U) {
            ESP_LOGW(TAG, "Dropped packet from unregistered apartment %u (total=%lu)",
                     (unsigned)apt_no, (unsigned long)s_unregistered_packets);
        }
        return;
    }

    /* Duplicate request: re-send the cached ACK (I-29) */
    if (slot->last_ack.valid &&
        slot->last_ack.tx_counter == clear_hdr->tx_counter &&
        slot->last_ack.seq_id == clear_hdr->seq_id &&
        mbedtls_ct_memcmp(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE) == 0) {
        doorbell_udp_send_direct(lan_sock, slot->last_ack.frame, slot->last_ack.frame_len, from_ip_host,
                                 CONFIG_DOORBELL_UDP_PORT);
        return;
    }

    /* Replay check, then AEAD authentication */
    if (clear_hdr->tx_counter <= slot->nvs.rx_floor) {
        s_rejected_messages++;
        return;
    }
    doorbell_hdr_t auth_hdr;
    uint8_t plain[128];
    size_t plain_len = 0;
    if (doorbell_frame_decrypt(buf, len, slot->nvs.k_apt, &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK) {
        s_rejected_messages++;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    s_last_any_module_rx_us = now_us;
    rtc_silence_set_level(0);
    uint32_t panel_ip = 0;
    doorbell_net_get_sta_ipv4(&panel_ip);

    if (auth_hdr.msg_type == DOORBELL_MSG_OPEN_DOOR_REQ) {
        follow->open_door = handle_open_door_locked(lan_sock, slot, apt_no, &auth_hdr, plain, plain_len, buf, len,
                                                    from_ip_host, panel_ip, now_us);
    } else if (auth_hdr.msg_type == DOORBELL_MSG_HEARTBEAT_REQ) {
        follow->heartbeat_valid = handle_heartbeat_locked(lan_sock, slot, apt_no, &auth_hdr, plain, plain_len, buf,
                                                          len, from_ip_host, panel_ip, now_us, &follow->heartbeat);
    }
}

static void handle_lan_packet(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host)
{
    if (len < DOORBELL_MIN_FRAME_SIZE) {
        return;
    }
    doorbell_hdr_t clear_hdr;
    if (doorbell_hdr_parse(buf, len, &clear_hdr) != ESP_OK) {
        return;
    }
    /* Pre-crypto destination & direction filter (I-53) */
    if (clear_hdr.dst_id != DOORBELL_ID_PANEL ||
        (clear_hdr.flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) == 0 ||
        clear_hdr.src_id < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
        clear_hdr.src_id > CONFIG_DOORBELL_PANEL_MAX_APT_NO) {
        return;
    }

    lan_followup_t follow = {0};
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    handle_frame_locked(lan_sock, buf, len, from_ip_host, &clear_hdr, &follow);
    xSemaphoreGive(g_panel_mutex);

    uint8_t apt_no = clear_hdr.src_id;
    if (follow.open_door && doorbell_relay_trigger(&g_door_relay, NULL) == ESP_OK) {
        doorbell_led_panel_start(LED_PANEL_DOOR_OPEN);
        ESP_LOGI(TAG, "Door unlocked by apartment %u", (unsigned)apt_no);
    }
    if (follow.heartbeat_valid) {
        const doorbell_pl_heartbeat_req_t *hb = &follow.heartbeat;
        ESP_LOGI(TAG,
                 "Heartbeat from apt %u: up=%lus rssi=%ddBm ch=%u rings=%u unacked_open=%u rej=%u disconn=%u "
                 "last restart: %s (%u) heap=%lu/%luB fw=%u.%u",
                 (unsigned)apt_no,
                 (unsigned long)hb->uptime_s,
                 (int)hb->ap_rssi,
                 (unsigned)hb->ap_channel,
                 (unsigned)hb->ring_count,
                 (unsigned)hb->unacked_open_count,
                 (unsigned)hb->rejected_count,
                 (unsigned)hb->disconnect_count,
                 doorbell_reset_reason_str(hb->reset_reason),
                 (unsigned)hb->reset_reason,
                 (unsigned long)hb->free_heap,
                 (unsigned long)hb->min_free_heap,
                 (unsigned)hb->fw_version_major,
                 (unsigned)hb->fw_version_minor);
    }
}

static void execute_ring_transaction(int lan_sock, uint8_t apt_no)
{
    uint32_t panel_ip = 0;
    if (!doorbell_net_get_sta_ipv4(&panel_ip)) {
        doorbell_led_panel_start(LED_PANEL_NO_RESPONSE);
        return;
    }

    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    if (!slot->nvs.registered) {
        xSemaphoreGive(g_panel_mutex);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    uint32_t tx_counter = 0;
    esp_err_t cnt_err = doorbell_storage_panel_next_tx_counter(&slot->nvs,
                                                               &slot->runtime_tx_counter,
                                                               CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                               &tx_counter);
    uint16_t seq_id = s_panel_next_seq_id++;
    uint32_t peer_ip = slot->nvs.last_ipv4;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    memcpy(k_apt, slot->nvs.k_apt, DOORBELL_KEY_SIZE);
    xSemaphoreGive(g_panel_mutex);

    if (cnt_err != ESP_OK) {
        mbedtls_platform_zeroize(k_apt, sizeof(k_apt));
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    doorbell_pl_ring_req_t req_pl = {
        .sender_ipv4 = panel_ip,
        .panel_boot_id = g_panel_boot_id,
    };
    uint8_t req_plain[8];
    size_t req_plain_len = doorbell_pack_ring_req(&req_pl, req_plain);

    doorbell_hdr_t req_hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_RING_REQ,
        .flags = 0,
        .src_id = DOORBELL_ID_PANEL,
        .dst_id = apt_no,
        .seq_id = seq_id,
        .tx_counter = tx_counter,
    };

    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    if (doorbell_frame_encrypt(&req_hdr, k_apt, req_plain, req_plain_len,
                               frame, sizeof(frame), &frame_len) != ESP_OK) {
        mbedtls_platform_zeroize(k_apt, sizeof(k_apt));
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    bool acked = false;
    bool slot_gone = false;
    int max_attempts = 1 + CONFIG_DOORBELL_PANEL_RING_RETRIES;

    for (int attempt = 0; attempt < max_attempts && !acked && !slot_gone; attempt++) {
        esp_task_wdt_reset();
        int64_t t_send_us = esp_timer_get_time();
        doorbell_udp_send_frame(lan_sock, frame, frame_len, peer_ip,
                                CONFIG_DOORBELL_UDP_PORT, (attempt > 0));

        int32_t timeout_ms = CONFIG_DOORBELL_ACK_TIMEOUT_MS;
        if (attempt > 0) {
            timeout_ms += (int32_t)(esp_random() % 51U) - 25;
        }
        int64_t deadline_us = t_send_us + (int64_t)timeout_ms * 1000LL;

        while (esp_timer_get_time() < deadline_us) {
            esp_task_wdt_reset();
            doorbell_relay_safety_check(&g_door_relay);

            uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(lan_sock, rx_buf, sizeof(rx_buf), 0,
                                 (struct sockaddr *)&from_addr, &from_len);
            if (n < (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
                continue;
            }

            uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
            doorbell_hdr_t rx_hdr;
            if (doorbell_hdr_parse(rx_buf, (size_t)n, &rx_hdr) != ESP_OK) {
                continue;
            }

            if (rx_hdr.msg_type != DOORBELL_MSG_RING_ACK || rx_hdr.src_id != apt_no) {
                handle_lan_packet(lan_sock, rx_buf, (size_t)n, from_ip);
                continue;
            }

            xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
            /* handle_lan_packet() above may have removed this registration (re-pairing under another number) */
            if (!slot->nvs.registered) {
                xSemaphoreGive(g_panel_mutex);
                slot_gone = true;
                break;
            }
            if (rx_hdr.tx_counter <= slot->nvs.rx_floor) {
                xSemaphoreGive(g_panel_mutex);
                continue;
            }

            uint8_t rx_plain[32];
            size_t rx_plain_len = 0;
            if (doorbell_frame_decrypt(rx_buf, (size_t)n, k_apt,
                                       &rx_hdr, rx_plain, sizeof(rx_plain), &rx_plain_len) == ESP_OK) {
                doorbell_pl_ring_ack_t ack_pl;
                if (doorbell_unpack_ring_ack(rx_plain, rx_plain_len, &ack_pl) == ESP_OK &&
                    ack_pl.req_tx_counter == tx_counter &&
                    ack_pl.req_seq_id == seq_id &&
                    (ack_pl.status == DOORBELL_STATUS_OK ||
                     ack_pl.status == DOORBELL_STATUS_ALREADY_ACTIVE)) {
                    slot_accept_rx_locked(slot, rx_hdr.tx_counter, ack_pl.sender_ipv4, from_ip);
                    slot->last_heartbeat_us = esp_timer_get_time();
                    slot->no_report_warned = false;
                    s_last_any_module_rx_us = slot->last_heartbeat_us;
                    rtc_silence_set_level(0);
                    acked = true;
                    int64_t rtt_ms = (esp_timer_get_time() - t_send_us) / 1000LL;
                    ESP_LOGI(TAG, "RING to apt %u acknowledged after %lld ms",
                             (unsigned)apt_no, (long long)rtt_ms);
                }
            }
            xSemaphoreGive(g_panel_mutex);
            if (acked) {
                break;
            }
        }
    }

    mbedtls_platform_zeroize(k_apt, sizeof(k_apt));
    if (acked) {
        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
    } else if (slot_gone) {
        ESP_LOGW(TAG, "RING to apartment %u stopped: registration removed meanwhile", (unsigned)apt_no);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    } else {
        ESP_LOGW(TAG, "RING to apartment %u timed out without ACK", (unsigned)apt_no);
        doorbell_led_panel_start(LED_PANEL_NO_RESPONSE);
    }
}

/* ---- Heartbeat Liveness & Silence Restart Monitor (I-52) ---- */

static void check_module_health_1s(void)
{
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
    int64_t now_us = esp_timer_get_time();
    int64_t miss_threshold_us = (int64_t)CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S * 3LL * 1000000LL;
    uint32_t reg_count = 0;

    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    for (uint8_t apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; apt++) {
        panel_apt_slot_t *slot = &g_apt_slots[apt];
        if (!slot->nvs.registered) {
            continue;
        }
        reg_count++;
        if (slot->last_heartbeat_us == 0) {
            slot->last_heartbeat_us = now_us;
        }
        if (!slot->no_report_warned && (now_us - slot->last_heartbeat_us) >= miss_threshold_us) {
            slot->no_report_warned = true;
            ESP_LOGW(TAG, "No report from apartment %u for 3 heartbeat intervals", (unsigned)apt);
        }
    }
    xSemaphoreGive(g_panel_mutex);

#if CONFIG_DOORBELL_PANEL_SILENCE_RESTART
    if (reg_count > 0 && g_panel_mode == MODE_IDLE) {
        uint64_t base_s = (uint64_t)CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S * 3ULL;
        uint32_t shift = (s_rtc_silence.backoff_level < 12) ? s_rtc_silence.backoff_level : 12;
        uint64_t timeout_s = base_s << shift;
        if (timeout_s > 48ULL * 3600ULL) {
            timeout_s = 48ULL * 3600ULL;
        }
        if ((now_us - s_last_any_module_rx_us) >= (int64_t)(timeout_s * 1000000ULL)) {
            uint32_t next_level = s_rtc_silence.backoff_level + 1U;
            rtc_silence_set_level(next_level);
            ESP_LOGW(TAG, "No module reported for %llu s — restarting panel (next backoff level=%lu)",
                     (unsigned long long)timeout_s, (unsigned long)next_level);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
#endif
#endif
}

/* ---- Main TWDT-Monitored Worker Task (I-46) ---- */

static void panel_worker_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int lan_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, 0, 50);
    if (lan_sock < 0) {
        ESP_LOGE(TAG, "Failed to create panel LAN UDP socket");
        vTaskDelete(NULL);
        return;
    }

    int64_t last_1s_tick_us = esp_timer_get_time();

    for (;;) {
        esp_task_wdt_reset();
        doorbell_relay_safety_check(&g_door_relay);
        panel_keypad_process();
        execute_pending_panel_actions();

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_1s_tick_us) >= 1000000LL) {
            last_1s_tick_us = now_us;
            panel_lockout_tick_1s();
            panel_portal_tick();
            check_module_health_1s();
            /* Persistent storage-fault indication (I-13): repeat the error pattern every 10 s */
            if (s_panel_cfg_fault && (++s_cfg_fault_led_divider >= 10)) {
                s_cfg_fault_led_divider = 0;
                doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            }
        }

        if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PAIR_COMPLETE) {
            step_pairing_discovery();
        }

        uint8_t ring_apt = 0;
        if (xQueueReceive(s_ring_queue, &ring_apt, 0) == pdTRUE) {
            execute_ring_transaction(lan_sock, ring_apt);
        }

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(lan_sock, rx_buf, sizeof(rx_buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n >= (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
            uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
            handle_lan_packet(lan_sock, rx_buf, (size_t)n, from_ip);
        }
    }
}

void app_main(void)
{
    g_panel_mutex = xSemaphoreCreateMutex();
    configASSERT(g_panel_mutex != NULL);
    s_ring_queue = xQueueCreate(RING_QUEUE_LEN, sizeof(uint8_t));
    configASSERT(s_ring_queue != NULL);

    rtc_silence_init_or_validate();

    /* 1. Boot-safe Door Relay initialization BEFORE any other peripheral (I-22) */
    ESP_ERROR_CHECK(doorbell_relay_init(&g_door_relay,
                                        CONFIG_DOORBELL_PANEL_GPIO_DOOR_RELAY,
                                        !DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW,
                                        CONFIG_DOORBELL_PANEL_DOOR_PULSE_MS,
                                        &on_door_relay_off,
                                        NULL));

    /* 2. Status LED initialization */
    ESP_ERROR_CHECK(doorbell_led_panel_init(CONFIG_DOORBELL_PANEL_GPIO_STATUS_LED));
    doorbell_led_panel_start(LED_PANEL_NET_LOST);

    /* 3. NVS storage initialization — auto-erase strictly forbidden on panel (I-13) */
    esp_err_t nvs_err = doorbell_storage_init();
    if (nvs_err != ESP_OK) {
        /* Storage fault: never erase (I-13). Keep a persistent error indication instead of a one-shot blink. */
        for (;;) {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    esp_err_t cfg_err = doorbell_storage_panel_load_cfg(&g_panel_cfg);
    if (cfg_err != ESP_OK) {
        /* A corrupt or incompatible panel_cfg blob must not abort() into an endless boot loop that also disables the
         * keypad. Run with in-RAM Kconfig defaults; nothing is erased at boot (I-13). doorbell_storage refuses to save
         * panel_cfg in this state, so the stored record (with its unique factory codes, I-5) is never overwritten by
         * the shared defaults; recovery is a full re-flash with the flasher (I-14). */
        ESP_LOGE(TAG, "CRITICAL: panel_cfg unreadable (%s) — keypad codes disabled, stored blob left intact; "
                      "re-flash with the flasher", esp_err_to_name(cfg_err));
        /* Fail closed (I-5): do NOT fall back to the shared built-in codes, or anyone knowing them could open the door
         * or run building reset. Admin/user/factory codes stay empty, so ct_equal_code() rejects every keypad and
         * console code. Visitors can still ring, and residents can still open the door from their modules. */
        memset(&g_panel_cfg, 0, sizeof(g_panel_cfg));
        s_panel_cfg_fault = true;
        strncpy(g_panel_cfg.wifi_ssid, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_SSID, sizeof(g_panel_cfg.wifi_ssid) - 1);
        strncpy(g_panel_cfg.wifi_pass, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_PASS, sizeof(g_panel_cfg.wifi_pass) - 1);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    } else {
        /* First boot after Burn / Flash: remember the provisioned building Wi-Fi for the hidden-button factory
         * reset (no-op once recorded). Without it the factory reset refuses rather than guessing. */
        esp_err_t fw_err = doorbell_storage_panel_snapshot_factory_wifi(&g_panel_cfg);
        if (fw_err != ESP_OK) {
            ESP_LOGW(TAG, "Factory Wi-Fi record unavailable (%s): hidden-button factory reset will be refused",
                     esp_err_to_name(fw_err));
        }
    }

    uint32_t loaded_apts = 0;
    int64_t boot_us = esp_timer_get_time();
    for (uint8_t apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; apt++) {
        if (doorbell_storage_panel_load_apt(apt, &g_apt_slots[apt].nvs) == ESP_OK &&
            g_apt_slots[apt].nvs.registered) {
            /* Resume monotonic counter from reserved write-ahead ceiling (I-50) */
            g_apt_slots[apt].runtime_tx_counter = g_apt_slots[apt].nvs.tx_counter_ceiling;
            g_apt_slots[apt].persisted_rx_floor = g_apt_slots[apt].nvs.rx_floor;
            g_apt_slots[apt].last_heartbeat_us = boot_us;
            loaded_apts++;
        }
    }
    ESP_LOGI(TAG, "Loaded %lu registered apartment(s) from NVS (last restart: %s)",
             (unsigned long)loaded_apts,
             doorbell_reset_reason_str((uint8_t)esp_reset_reason()));

    /* 4. Network & PSA Crypto initialization (I-26, I-38) */
    ESP_ERROR_CHECK(doorbell_net_init("doorbell-panel", &on_panel_net_state, NULL));

    /* Generate non-zero random 32-bit panel_boot_id after RF entropy is initialized (I-7, I-26) */
    do {
        doorbell_crypto_random((uint8_t *)&g_panel_boot_id, sizeof(g_panel_boot_id));
    } while (g_panel_boot_id == 0);

    if (g_panel_cfg.wifi_ssid[0] != '\0') {
        doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
    } else {
        ESP_LOGW(TAG, "No building Wi-Fi SSID configured yet — use *<admin>*5# portal or UART console");
    }

    /* 5. Matrix keypad & hidden reset button initialization */
    ESP_ERROR_CHECK(panel_keypad_init());

    /* 6. Authenticated UART console initialization */
    panel_console_init();

    /* 7. Register esp_timer and FreeRTOS timer pipeline health handles with TWDT (I-46) */
    ESP_ERROR_CHECK(doorbell_twdt_register_timer_pipelines("panel_esp_timer", "panel_rtos_timer"));

    /* 8. Start TWDT-monitored panel worker task */
    xTaskCreate(panel_worker_task, "panel_worker", 8192, NULL, 5, NULL);
}

