#include "panel_state.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "doorbell_twdt.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "mbedtls/constant_time.h"

static const char *TAG = "doorbell_panel";

/* Shared global state */
SemaphoreHandle_t g_panel_mutex = NULL;
doorbell_panel_cfg_blob_t g_panel_cfg = {0};
panel_apt_slot_t g_apt_slots[DOORBELL_MAX_APT_SLOTS] = {0};
volatile panel_mode_t g_panel_mode = MODE_IDLE;
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

/* Pending asynchronous actions dispatched from keypad/button callbacks to panel_worker_task */
static volatile uint8_t s_pending_ring_apt = 0;
static volatile uint8_t s_pending_pair_apt = 0;
static volatile bool s_pending_start_portal = false;
static volatile bool s_pending_cancel_mode = false;
static volatile bool s_pending_building_reset = false;
static volatile bool s_pending_code_reset = false;

static uint16_t s_panel_next_seq_id = 1;
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
    s_pending_ring_apt = apt_no;
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
    s_pending_pair_apt = apt_no;
}

void panel_action_start_portal(void)
{
    s_pending_start_portal = true;
}

void panel_action_cancel_active_mode(void)
{
    s_pending_cancel_mode = true;
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
    s_pending_building_reset = true;
}

void panel_action_reset_codes_to_factory(void)
{
    s_pending_code_reset = true;
}

static void execute_pending_panel_actions(void)
{
    if (s_pending_building_reset) {
        s_pending_building_reset = false;
        ESP_LOGW(TAG, "Executing full building reset (*<admin>*6*<admin>#)");
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        abort_pairing_session_locked(false);
        panel_portal_stop();
        /* Building reset erases apartment registrations and keys only; user/admin codes and building Wi-Fi
         * credentials are preserved (kullanim-kilavuzu.md §6.3) */
        doorbell_storage_panel_erase_all_apts(CONFIG_DOORBELL_PANEL_MIN_APT_NO,
                                              CONFIG_DOORBELL_PANEL_MAX_APT_NO);
        memset(g_apt_slots, 0, sizeof(g_apt_slots));
        xSemaphoreGive(g_panel_mutex);
        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
        for (int i = 0; i < 20; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        esp_restart();
    }

    if (s_pending_code_reset) {
        s_pending_code_reset = false;
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        if (g_panel_mode == MODE_PAIR_DISCOVER) {
            abort_pairing_session_locked(false);
        } else if (g_panel_mode == MODE_PORTAL) {
            panel_portal_stop();
        }
        esp_err_t err = doorbell_storage_panel_reset_codes_to_factory(&g_panel_cfg);
        /* The storage call zeroes the persisted lockout counters; also drop the running in-RAM lockout, otherwise
         * the restored factory codes stay rejected and the 1 s tick writes the remaining time back to NVS */
        panel_lockout_clear_locked();
        xSemaphoreGive(g_panel_mutex);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Restored admin and user codes to factory defaults via hidden reset button");
            doorbell_led_panel_start(LED_PANEL_RESULT_OK);
        } else {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
    }

    if (s_pending_cancel_mode) {
        s_pending_cancel_mode = false;
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        if (g_panel_mode == MODE_PAIR_DISCOVER) {
            ESP_LOGI(TAG, "Pairing discovery cancelled by user");
            abort_pairing_session_locked(false);
        } else if (g_panel_mode == MODE_PORTAL) {
            panel_portal_stop();
        }
        xSemaphoreGive(g_panel_mutex);
    }

    if (s_pending_start_portal) {
        s_pending_start_portal = false;
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        esp_err_t err = panel_portal_start();
        xSemaphoreGive(g_panel_mutex);
        if (err != ESP_OK) {
            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        }
    }

    uint8_t pair_apt = s_pending_pair_apt;
    if (pair_apt != 0) {
        s_pending_pair_apt = 0;
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
        memset(&blob, 0, sizeof(blob));

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

/* ---- Operational LAN UDP Frame Handler ---- */

static void handle_lan_packet(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host)
{
    if (len < DOORBELL_MIN_FRAME_SIZE) {
        return;
    }

    doorbell_hdr_t clear_hdr;
    if (doorbell_hdr_parse(buf, len, &clear_hdr) != ESP_OK) {
        return;
    }

    /* Step 2: Pre-crypto destination & direction filter (I-53) */
    if (clear_hdr.dst_id != DOORBELL_ID_PANEL ||
        (clear_hdr.flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) == 0 ||
        clear_hdr.src_id < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
        clear_hdr.src_id > CONFIG_DOORBELL_PANEL_MAX_APT_NO) {
        return;
    }

    uint8_t apt_no = clear_hdr.src_id;

    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);

    /* Special case: PAIR_COMPLETE arriving during MODE_PAIR_COMPLETE (or idempotent retry right after) */
    if (clear_hdr.msg_type == DOORBELL_MSG_PAIR_COMPLETE) {
        if (g_panel_mode == MODE_PAIR_COMPLETE && s_pair_sess.apt_no == apt_no) {
            doorbell_hdr_t auth_hdr;
            uint8_t plain[64];
            size_t plain_len = 0;
            if (doorbell_frame_decrypt(buf, len, s_pair_sess.k_apt,
                                       &auth_hdr, plain, sizeof(plain), &plain_len) == ESP_OK) {
                doorbell_pl_pair_complete_t pc;
                if (doorbell_unpack_pair_complete(plain, plain_len, &pc) == ESP_OK &&
                    memcmp(pc.unit_mac, s_pair_sess.hello.unit_mac, 6) == 0) {
                    /* Atomic NVS Commit of new apartment record (I-30, I-50). Build the record in a scratch copy and
                     * publish it to g_apt_slots only after nvs_commit() succeeds, so a failed write never leaves a
                     * RAM-only registration behind. */
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
                        memset(&new_rec, 0, sizeof(new_rec));
                        ESP_LOGE(TAG, "NVS commit of apartment %u failed (%s) — pairing rolled back",
                                 (unsigned)apt_no, esp_err_to_name(commit_err));
                        abort_pairing_session_locked(true);
                    } else {
                        panel_apt_slot_t *slot = &g_apt_slots[apt_no];
                        memset(slot, 0, sizeof(*slot));
                        slot->nvs = new_rec;
                        memset(&new_rec, 0, sizeof(new_rec));
                        slot->persisted_rx_floor = auth_hdr.tx_counter;
                        slot->runtime_tx_counter = 0;
                        slot->last_heartbeat_us = esp_timer_get_time();
                        /* Erase any older apartment slot sharing the same unit_mac ONLY after new commit succeeds (I-30, I-45) */
                        for (uint8_t old_apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO;
                             old_apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO;
                             old_apt++) {
                            if (old_apt != apt_no &&
                                g_apt_slots[old_apt].nvs.registered &&
                                memcmp(g_apt_slots[old_apt].nvs.unit_mac, pc.unit_mac, 6) == 0) {
                                doorbell_storage_panel_erase_apt(old_apt);
                                memset(&g_apt_slots[old_apt], 0, sizeof(g_apt_slots[old_apt]));
                                ESP_LOGI(TAG, "Removed previous registration of same unit_mac from apartment %u",
                                         (unsigned)old_apt);
                            }
                        }

                        uint32_t panel_ip = 0;
                        doorbell_net_get_sta_ipv4(&panel_ip);
                        uint32_t ack_cnt = 0;
                        if (doorbell_storage_panel_next_tx_counter(&slot->nvs,
                                                                   &slot->runtime_tx_counter,
                                                                   CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                                   &ack_cnt) == ESP_OK) {
                            doorbell_pl_pair_complete_ack_t ack_pl = {
                                .req_tx_counter = auth_hdr.tx_counter,
                                .req_seq_id = auth_hdr.seq_id,
                                .status = DOORBELL_STATUS_OK,
                                .sender_ipv4 = panel_ip,
                                .panel_boot_id = g_panel_boot_id,
                            };
                            uint8_t ack_plain[15];
                            size_t ack_plain_len = doorbell_pack_heartbeat_ack(&ack_pl, ack_plain);
                            doorbell_hdr_t ack_hdr = {
                                .magic = DOORBELL_PROTO_MAGIC,
                                .version = DOORBELL_PROTO_VERSION,
                                .msg_type = DOORBELL_MSG_PAIR_COMPLETE_ACK,
                                .flags = 0,
                                .src_id = DOORBELL_ID_PANEL,
                                .dst_id = apt_no,
                                .seq_id = auth_hdr.seq_id,
                                .tx_counter = ack_cnt,
                            };
                            if (doorbell_frame_encrypt(&ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len,
                                                       slot->last_ack.frame, sizeof(slot->last_ack.frame),
                                                       &slot->last_ack.frame_len) == ESP_OK) {
                                slot->last_ack.valid = true;
                                slot->last_ack.src_id = apt_no;
                                slot->last_ack.tx_counter = auth_hdr.tx_counter;
                                slot->last_ack.seq_id = auth_hdr.seq_id;
                                memcpy(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);
                                doorbell_udp_send_direct(lan_sock,
                                                         slot->last_ack.frame,
                                                         slot->last_ack.frame_len,
                                                         from_ip_host,
                                                         CONFIG_DOORBELL_UDP_PORT);
                            }
                        }

                        s_last_any_module_rx_us = esp_timer_get_time();
                        rtc_silence_set_level(0);
                        abort_pairing_session_locked(false);
                        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
                        ESP_LOGI(TAG, "Completed two-phase pairing for apartment %u (fw v%u.%u)",
                                 (unsigned)apt_no, (unsigned)pc.fw_version_major, (unsigned)pc.fw_version_minor);
                    }
                }
            }
            xSemaphoreGive(g_panel_mutex);
            return;
        }
    }

    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    if (!slot->nvs.registered) {
        s_unregistered_packets++;
        if ((s_unregistered_packets & 0x1FU) == 1U) {
            ESP_LOGW(TAG, "Dropped packet from unregistered apartment %u (total=%lu)",
                     (unsigned)apt_no, (unsigned long)s_unregistered_packets);
        }
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    /* Step 3: Deduplication check (I-29) — re-send cached ACK on duplicate request */
    if (slot->last_ack.valid &&
        slot->last_ack.tx_counter == clear_hdr.tx_counter &&
        slot->last_ack.seq_id == clear_hdr.seq_id &&
        mbedtls_ct_memcmp(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE) == 0) {
        doorbell_udp_send_direct(lan_sock,
                                 slot->last_ack.frame,
                                 slot->last_ack.frame_len,
                                 from_ip_host,
                                 CONFIG_DOORBELL_UDP_PORT);
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    /* Step 4: Replay check */
    if (clear_hdr.tx_counter <= slot->nvs.rx_floor) {
        s_rejected_messages++;
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    /* Step 5: AEAD decryption and authentication */
    doorbell_hdr_t auth_hdr;
    uint8_t plain[128];
    size_t plain_len = 0;
    if (doorbell_frame_decrypt(buf, len, slot->nvs.k_apt,
                               &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK) {
        s_rejected_messages++;
        xSemaphoreGive(g_panel_mutex);
        return;
    }

    int64_t now_us = esp_timer_get_time();
    s_last_any_module_rx_us = now_us;
    rtc_silence_set_level(0);

    uint32_t panel_ip = 0;
    doorbell_net_get_sta_ipv4(&panel_ip);

    if (auth_hdr.msg_type == DOORBELL_MSG_OPEN_DOOR_REQ) {
        doorbell_pl_open_door_req_t req;
        if (doorbell_unpack_open_door_req(plain, plain_len, &req) != ESP_OK) {
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        slot->nvs.rx_floor = auth_hdr.tx_counter;
        bool ip_changed = false;
        if (req.sender_ipv4 == from_ip_host && from_ip_host != 0 && slot->nvs.last_ipv4 != from_ip_host) {
            slot->nvs.last_ipv4 = from_ip_host;
            ip_changed = true;
        }
        if (ip_changed || (slot->nvs.rx_floor - slot->persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
            if (doorbell_storage_panel_save_apt(&slot->nvs) == ESP_OK) {
                slot->persisted_rx_floor = slot->nvs.rx_floor;
            }
        }
        slot->last_heartbeat_us = now_us;
        slot->no_report_warned = false;

        /* Reboot replay & withheld-packet freshness verification (I-7, I-18) */
        bool challenge_verified = (req.panel_boot_id == g_panel_boot_id) &&
                                  (req.echo_nonce != 0) &&
                                  (req.echo_nonce == slot->active_challenge_nonce) &&
                                  (now_us <= slot->challenge_deadline_us);

        uint32_t ack_cnt = 0;
        if (doorbell_storage_panel_next_tx_counter(&slot->nvs,
                                                   &slot->runtime_tx_counter,
                                                   CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                   &ack_cnt) != ESP_OK) {
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        if (!challenge_verified) {
            /* Issue 64-bit challenge_nonce valid for 2000 ms without unlocking door yet (I-7, I-18) */
            uint64_t chal = 0;
            doorbell_crypto_random((uint8_t *)&chal, sizeof(chal));
            if (chal == 0) {
                chal = 1;
            }
            slot->active_challenge_nonce = chal;
            slot->challenge_deadline_us = now_us + 2000000LL;

            doorbell_pl_open_door_ack_t ack_pl = {
                .req_tx_counter = auth_hdr.tx_counter,
                .req_seq_id = auth_hdr.seq_id,
                .status = DOORBELL_STATUS_CHALLENGE,
                .sender_ipv4 = panel_ip,
                .panel_boot_id = g_panel_boot_id,
                .challenge_nonce = chal,
            };
            uint8_t ack_plain[23];
            size_t ack_plain_len = doorbell_pack_open_door_ack(&ack_pl, ack_plain);
            doorbell_hdr_t ack_hdr = {
                .magic = DOORBELL_PROTO_MAGIC,
                .version = DOORBELL_PROTO_VERSION,
                .msg_type = DOORBELL_MSG_OPEN_DOOR_ACK,
                .flags = 0,
                .src_id = DOORBELL_ID_PANEL,
                .dst_id = apt_no,
                .seq_id = auth_hdr.seq_id,
                .tx_counter = ack_cnt,
            };
            if (doorbell_frame_encrypt(&ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len,
                                       slot->last_ack.frame, sizeof(slot->last_ack.frame),
                                       &slot->last_ack.frame_len) == ESP_OK) {
                slot->last_ack.valid = true;
                slot->last_ack.src_id = apt_no;
                slot->last_ack.tx_counter = auth_hdr.tx_counter;
                slot->last_ack.seq_id = auth_hdr.seq_id;
                memcpy(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);
                doorbell_udp_send_direct(lan_sock,
                                         slot->last_ack.frame,
                                         slot->last_ack.frame_len,
                                         from_ip_host,
                                         CONFIG_DOORBELL_UDP_PORT);
            }
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        /* Challenge verified: consume challenge, send ACK first, then energize door relay */
        slot->active_challenge_nonce = 0;
        slot->challenge_deadline_us = 0;
        bool already_active = g_door_relay.is_active;

        doorbell_pl_open_door_ack_t ack_pl = {
            .req_tx_counter = auth_hdr.tx_counter,
            .req_seq_id = auth_hdr.seq_id,
            .status = already_active ? DOORBELL_STATUS_ALREADY_ACTIVE : DOORBELL_STATUS_OK,
            .sender_ipv4 = panel_ip,
            .panel_boot_id = g_panel_boot_id,
            .challenge_nonce = 0,
        };
        uint8_t ack_plain[23];
        size_t ack_plain_len = doorbell_pack_open_door_ack(&ack_pl, ack_plain);
        doorbell_hdr_t ack_hdr = {
            .magic = DOORBELL_PROTO_MAGIC,
            .version = DOORBELL_PROTO_VERSION,
            .msg_type = DOORBELL_MSG_OPEN_DOOR_ACK,
            .flags = 0,
            .src_id = DOORBELL_ID_PANEL,
            .dst_id = apt_no,
            .seq_id = auth_hdr.seq_id,
            .tx_counter = ack_cnt,
        };
        if (doorbell_frame_encrypt(&ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len,
                                   slot->last_ack.frame, sizeof(slot->last_ack.frame),
                                   &slot->last_ack.frame_len) == ESP_OK) {
            slot->last_ack.valid = true;
            slot->last_ack.src_id = apt_no;
            slot->last_ack.tx_counter = auth_hdr.tx_counter;
            slot->last_ack.seq_id = auth_hdr.seq_id;
            memcpy(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);
            doorbell_udp_send_direct(lan_sock,
                                     slot->last_ack.frame,
                                     slot->last_ack.frame_len,
                                     from_ip_host,
                                     CONFIG_DOORBELL_UDP_PORT);
        }
        xSemaphoreGive(g_panel_mutex);

        if (doorbell_relay_trigger(&g_door_relay, NULL) == ESP_OK) {
            doorbell_led_panel_start(LED_PANEL_DOOR_OPEN);
            ESP_LOGI(TAG, "Door unlocked by apartment %u", (unsigned)apt_no);
        }
        return;
    }

    if (auth_hdr.msg_type == DOORBELL_MSG_HEARTBEAT_REQ) {
        doorbell_pl_heartbeat_req_t hb;
        if (doorbell_unpack_heartbeat_req(plain, plain_len, &hb) != ESP_OK) {
            xSemaphoreGive(g_panel_mutex);
            return;
        }

        slot->nvs.rx_floor = auth_hdr.tx_counter;
        bool ip_changed = false;
        if (hb.sender_ipv4 == from_ip_host && from_ip_host != 0 && slot->nvs.last_ipv4 != from_ip_host) {
            slot->nvs.last_ipv4 = from_ip_host;
            ip_changed = true;
        }
        if (ip_changed || (slot->nvs.rx_floor - slot->persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
            if (doorbell_storage_panel_save_apt(&slot->nvs) == ESP_OK) {
                slot->persisted_rx_floor = slot->nvs.rx_floor;
            }
        }
        slot->last_heartbeat_us = now_us;
        slot->no_report_warned = false;

        uint32_t ack_cnt = 0;
        if (doorbell_storage_panel_next_tx_counter(&slot->nvs,
                                                   &slot->runtime_tx_counter,
                                                   CONFIG_DOORBELL_NVS_COUNTER_BLOCK,
                                                   &ack_cnt) == ESP_OK) {
            doorbell_pl_heartbeat_ack_t ack_pl = {
                .req_tx_counter = auth_hdr.tx_counter,
                .req_seq_id = auth_hdr.seq_id,
                .status = DOORBELL_STATUS_OK,
                .sender_ipv4 = panel_ip,
                .panel_boot_id = g_panel_boot_id,
            };
            uint8_t ack_plain[15];
            size_t ack_plain_len = doorbell_pack_heartbeat_ack(&ack_pl, ack_plain);
            doorbell_hdr_t ack_hdr = {
                .magic = DOORBELL_PROTO_MAGIC,
                .version = DOORBELL_PROTO_VERSION,
                .msg_type = DOORBELL_MSG_HEARTBEAT_ACK,
                .flags = 0,
                .src_id = DOORBELL_ID_PANEL,
                .dst_id = apt_no,
                .seq_id = auth_hdr.seq_id,
                .tx_counter = ack_cnt,
            };
            if (doorbell_frame_encrypt(&ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len,
                                       slot->last_ack.frame, sizeof(slot->last_ack.frame),
                                       &slot->last_ack.frame_len) == ESP_OK) {
                slot->last_ack.valid = true;
                slot->last_ack.src_id = apt_no;
                slot->last_ack.tx_counter = auth_hdr.tx_counter;
                slot->last_ack.seq_id = auth_hdr.seq_id;
                memcpy(slot->last_ack.req_tag, buf + len - DOORBELL_TAG_SIZE, DOORBELL_TAG_SIZE);
                doorbell_udp_send_direct(lan_sock,
                                         slot->last_ack.frame,
                                         slot->last_ack.frame_len,
                                         from_ip_host,
                                         CONFIG_DOORBELL_UDP_PORT);
            }
        }
        xSemaphoreGive(g_panel_mutex);

        ESP_LOGI(TAG,
                 "Heartbeat from apt %u: up=%lus rssi=%ddBm ch=%u rings=%u unacked_open=%u rej=%u disconn=%u last restart: %s (%u) heap=%lu/%luB fw=%u.%u",
                 (unsigned)apt_no,
                 (unsigned long)hb.uptime_s,
                 (int)hb.ap_rssi,
                 (unsigned)hb.ap_channel,
                 (unsigned)hb.ring_count,
                 (unsigned)hb.unacked_open_count,
                 (unsigned)hb.rejected_count,
                 (unsigned)hb.disconnect_count,
                 doorbell_reset_reason_str(hb.reset_reason),
                 (unsigned)hb.reset_reason,
                 (unsigned long)hb.free_heap,
                 (unsigned long)hb.min_free_heap,
                 (unsigned)hb.fw_version_major,
                 (unsigned)hb.fw_version_minor);
        return;
    }

    xSemaphoreGive(g_panel_mutex);
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
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        return;
    }

    bool acked = false;
    int max_attempts = 1 + CONFIG_DOORBELL_PANEL_RING_RETRIES;

    for (int attempt = 0; attempt < max_attempts && !acked; attempt++) {
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
                    slot->nvs.rx_floor = rx_hdr.tx_counter;
                    bool ip_changed = false;
                    if (ack_pl.sender_ipv4 == from_ip && from_ip != 0 && slot->nvs.last_ipv4 != from_ip) {
                        slot->nvs.last_ipv4 = from_ip;
                        ip_changed = true;
                    }
                    if (ip_changed || (slot->nvs.rx_floor - slot->persisted_rx_floor) >= CONFIG_DOORBELL_NVS_COUNTER_BLOCK) {
                        if (doorbell_storage_panel_save_apt(&slot->nvs) == ESP_OK) {
                            slot->persisted_rx_floor = slot->nvs.rx_floor;
                        }
                    }
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

    if (acked) {
        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
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
        }

        if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PAIR_COMPLETE) {
            step_pairing_discovery();
        }

        uint8_t ring_apt = s_pending_ring_apt;
        if (ring_apt != 0) {
            s_pending_ring_apt = 0;
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
         * the shared defaults; recovery is a full re-provision with Flash NVS (I-14). */
        ESP_LOGE(TAG, "CRITICAL: panel_cfg unreadable (%s) — running with in-RAM defaults, stored blob left intact",
                 esp_err_to_name(cfg_err));
        memset(&g_panel_cfg, 0, sizeof(g_panel_cfg));
        strncpy(g_panel_cfg.admin_code, CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE, sizeof(g_panel_cfg.admin_code) - 1);
        strncpy(g_panel_cfg.user_code, CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE, sizeof(g_panel_cfg.user_code) - 1);
        strncpy(g_panel_cfg.factory_admin_code, CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE,
                sizeof(g_panel_cfg.factory_admin_code) - 1);
        strncpy(g_panel_cfg.factory_user_code, CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE,
                sizeof(g_panel_cfg.factory_user_code) - 1);
        strncpy(g_panel_cfg.wifi_ssid, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_SSID, sizeof(g_panel_cfg.wifi_ssid) - 1);
        strncpy(g_panel_cfg.wifi_pass, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_PASS, sizeof(g_panel_cfg.wifi_pass) - 1);
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
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

