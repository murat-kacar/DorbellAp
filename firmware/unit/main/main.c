#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "doorbell_twdt.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

#include "unit_config.h"
#include "doorbell_relay.h"
#include "doorbell_reset_reason.h"
#include "doorbell_protocol.h"
#include "doorbell_storage.h"
#include "doorbell_led.h"
#include "doorbell_net.h"
#include "doorbell_transport.h"
#include "unit_state.h"

static const char *TAG = "doorbell_unit";

#define UDP_SOCKET_RCVTIMEO_MS   100
#define UNPAIRED_HOSTNAME        "doorbell-apt-unpaired"
#define HEARTBEAT_INITIAL_MIN_MS 1000U
#define HEARTBEAT_INITIAL_MAX_MS 5000U
#define HEARTBEAT_UNREACHABLE_S  15U

/* Runtime state protected by s_state_mutex */
static SemaphoreHandle_t s_state_mutex = NULL;
static doorbell_unit_state_blob_t s_unit_state = {0};

static inline void unit_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static inline void unit_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}
static uint32_t s_runtime_tx_counter = 0;
static uint32_t s_persisted_rx_floor = 0;
static uint16_t s_next_seq_id = 1;
static uint32_t s_known_panel_boot_id = 0;
static doorbell_dedup_cache_t s_last_ring_ack = {0};

/* Telemetry counters, unit_worker_task only */
static uint16_t s_ring_count = 0;
static uint16_t s_unacked_open_count = 0;
static uint16_t s_rejected_count = 0;
static uint8_t  s_reset_reason = 0;

/* Coverage and heartbeat tracking */
static _Atomic bool s_sta_has_ip = false;
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
static _Atomic bool s_panel_reachable = false;
#else
static _Atomic bool s_panel_reachable = true;
#endif
static _Atomic uint32_t s_missed_heartbeats = 0;
static _Atomic int64_t s_next_heartbeat_us = 0;

/* Shared with unit_buttons.c and unit_pairing.c (unit_state.h) */
_Atomic bool g_unit_paired = false;
_Atomic bool g_unit_pairing_active = false;
_Atomic bool g_unit_open_door_in_flight = false;
_Atomic bool g_unit_open_door_requested = false;
_Atomic bool g_unit_pregen_x25519_requested = false;
_Atomic bool g_unit_pairing_requested = false;
_Atomic bool g_unit_reset_requested = false;

static doorbell_relay_t s_bell_relay = {0};

static void on_bell_relay_off(void *ctx)
{
    (void)ctx;
#if CONFIG_DOORBELL_UNIT_VISUAL_BELL
    doorbell_led_stop(LED_UNIT_RINGING);
#endif
}

void unit_poll(void *ctx)
{
    (void)ctx;
    esp_task_wdt_reset();
    doorbell_relay_safety_check(&s_bell_relay);
}

uint16_t unit_next_seq_id(void)
{
    return s_next_seq_id++;
}

void unit_set_apartment_hostname(uint8_t apt_no)
{
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "doorbell-apt-%03u", (unsigned)apt_no);
    doorbell_net_set_hostname(hostname);
}

static void update_steady_led(void)
{
    if (!g_unit_paired) {
        doorbell_led_stop(LED_UNIT_IN_COVERAGE);
        doorbell_led_stop(LED_UNIT_OUT_OF_COV);
        doorbell_led_start(LED_UNIT_UNPAIRED);
        return;
    }

    doorbell_led_stop(LED_UNIT_UNPAIRED);
    if (s_sta_has_ip && s_panel_reachable) {
        doorbell_led_stop(LED_UNIT_OUT_OF_COV);
        doorbell_led_start(LED_UNIT_IN_COVERAGE);
    } else {
        doorbell_led_stop(LED_UNIT_IN_COVERAGE);
        doorbell_led_start(LED_UNIT_OUT_OF_COV);
    }
}

static void schedule_next_heartbeat(bool initial_after_boot)
{
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
    int64_t now_us = esp_timer_get_time();
    if (initial_after_boot) {
        /* Boot / link-up quick probe: uniform random delay in [1 s, 5 s] */
        uint32_t span_ms = HEARTBEAT_INITIAL_MAX_MS - HEARTBEAT_INITIAL_MIN_MS;
        uint32_t delay_ms = HEARTBEAT_INITIAL_MIN_MS + ((span_ms > 0) ? (esp_random() % (span_ms + 1U)) : 0U);
        s_next_heartbeat_us = now_us + (int64_t)delay_ms * 1000LL;
        return;
    }

    unit_lock();
    uint32_t interval_s = s_unit_state.heartbeat_interval_s;
    unit_unlock();
    if (interval_s < 30) {
        interval_s = CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S;
    }
    if (!s_panel_reachable && interval_s > HEARTBEAT_UNREACHABLE_S) {
        interval_s = HEARTBEAT_UNREACHABLE_S;
    }
    /* Periodic interval +/- 10% random jitter */
    int32_t base_ms = (int32_t)(interval_s * 1000U);
    int32_t jitter_span = base_ms / 10;
    int32_t jitter_ms = (jitter_span > 0)
                        ? ((int32_t)(esp_random() % (uint32_t)(2 * jitter_span)) - jitter_span)
                        : 0;
    s_next_heartbeat_us = now_us + (int64_t)(base_ms + jitter_ms) * 1000LL;
#else
    (void)initial_after_boot;
#endif
}

static void on_net_state_changed(bool connected, uint32_t ipv4_host, void *ctx)
{
    (void)ipv4_host;
    (void)ctx;
    s_sta_has_ip = connected;
    if (!connected) {
        s_next_heartbeat_us = 0;
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
        s_panel_reachable = false;
#endif
    }
    if (g_unit_pairing_active) {
        return;
    }
    if (connected && g_unit_paired && (!s_panel_reachable || s_next_heartbeat_us == 0)) {
        schedule_next_heartbeat(true);
    }
    update_steady_led();
}

static void execute_unit_reset(void)
{
    ESP_LOGI(TAG, "Resetting apartment module pairing state and restarting (kullanim-kilavuzu.md §3.5)");
    doorbell_net_disconnect_sta();
    unit_lock();
    doorbell_storage_unit_erase();
    mbedtls_platform_zeroize(&s_unit_state, sizeof(s_unit_state));
    unit_unlock();
    g_unit_paired = false;

    doorbell_led_start(LED_UNIT_RESULT_OK);
    for (int i = 0; i < 20; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_restart();
}

/* ---- Confirmable requests to the panel ---- */

static void handle_incoming_frame(int udp_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host);

static doorbell_txn_verdict_t unit_ack_rx_cb(const uint8_t *frame, size_t len, const doorbell_hdr_t *hdr,
                                             uint32_t from_ip, void *arg)
{
    unit_ack_wait_t *w = (unit_ack_wait_t *)arg;
    if (hdr->msg_type == DOORBELL_MSG_RING_REQ) {
        handle_incoming_frame(w->sock, frame, len, from_ip);
        return DOORBELL_TXN_CONTINUE;
    }
    if (hdr->msg_type != w->ack_type || hdr->src_id != DOORBELL_ID_PANEL || hdr->dst_id != w->apt_no ||
            hdr->tx_counter <= w->rx_floor) {
        return DOORBELL_TXN_CONTINUE;
    }
    uint32_t req_tx_counter = 0;
    uint16_t req_seq_id = 0;
    if (doorbell_frame_decrypt(frame, len, w->k_apt, &w->ack_hdr, w->plain, sizeof(w->plain), &w->plain_len) !=
            ESP_OK ||
            doorbell_unpack_ack_ref(w->plain, w->plain_len, &req_tx_counter, &req_seq_id, &w->status) != ESP_OK ||
            req_tx_counter != w->req_tx_counter || req_seq_id != w->req_seq_id) {
        return DOORBELL_TXN_CONTINUE;
    }
    w->from_ip = from_ip;
    return DOORBELL_TXN_DONE;
}

esp_err_t unit_run_txn(uint8_t *frame, size_t frame_len, uint32_t peer_ip, uint32_t retries, unit_ack_wait_t *w,
                       uint32_t *out_rtt_ms)
{
    const doorbell_txn_t txn = {
        .sock = w->sock,
        .frame = frame,
        .frame_len = frame_len,
        .peer_ipv4_host = peer_ip,
        .port = CONFIG_DOORBELL_UDP_PORT,
        .retries = retries,
        .ack_timeout_ms = CONFIG_DOORBELL_ACK_TIMEOUT_MS,
        .on_rx = unit_ack_rx_cb,
        .on_poll = unit_poll,
        .ctx = w,
    };
    return doorbell_txn_run(&txn, out_rtt_ms);
}

/* One request of a paired module: counter (write-ahead), seq_id, key and panel address taken under the lock */
typedef struct {
    uint32_t tx_counter;
    uint32_t rx_floor;
    uint16_t seq_id;
    uint16_t heartbeat_interval_s;
    uint32_t panel_ip;
    uint8_t apt_no;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
} unit_request_t;

static esp_err_t unit_request_begin(unit_request_t *req)
{
    unit_lock();
    esp_err_t err = doorbell_storage_unit_next_tx_counter(&s_unit_state, &s_runtime_tx_counter,
                                                          CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &req->tx_counter);
    req->rx_floor = s_unit_state.rx_floor;
    req->seq_id = unit_next_seq_id();
    req->heartbeat_interval_s = s_unit_state.heartbeat_interval_s;
    req->panel_ip = s_unit_state.panel_ipv4;
    req->apt_no = s_unit_state.apt_no;
    memcpy(req->k_apt, s_unit_state.k_apt, DOORBELL_KEY_SIZE);
    unit_unlock();
    return err;
}

static esp_err_t unit_request_encrypt(const unit_request_t *req, uint8_t msg_type, const uint8_t *plain,
                                      size_t plain_len, uint8_t frame[DOORBELL_MAX_FRAME_SIZE], size_t *frame_len)
{
    doorbell_hdr_t hdr = doorbell_hdr_make(msg_type, DOORBELL_FLAG_DIR_UNIT_TO_PANEL, req->apt_no, DOORBELL_ID_PANEL,
                                           req->seq_id, req->tx_counter);
    return doorbell_frame_encrypt(&hdr, req->k_apt, plain, plain_len, frame, DOORBELL_MAX_FRAME_SIZE, frame_len);
}

/* Accepted ACK from the panel: replay floor, authenticated panel IP (persisted in write-ahead blocks) and
 * the panel's boot id */
static void unit_accept_panel_ack(const unit_ack_wait_t *w, uint32_t sender_ipv4, uint32_t panel_boot_id)
{
    unit_lock();
    if (doorbell_rx_accept(&s_unit_state.rx_floor, &s_unit_state.panel_ipv4, s_persisted_rx_floor,
                           w->ack_hdr.tx_counter, sender_ipv4, w->from_ip, CONFIG_DOORBELL_NVS_COUNTER_BLOCK) &&
            doorbell_storage_unit_save(&s_unit_state) == ESP_OK) {
        s_persisted_rx_floor = s_unit_state.rx_floor;
    }
    s_known_panel_boot_id = panel_boot_id;
    unit_unlock();
    s_missed_heartbeats = 0;
    s_panel_reachable = true;
}

/* ---- Two-phase pairing: run the client (unit_pairing.c), then commit ---- */

/* Persist the pairing state, adopting it in RAM only after NVS confirms */
static esp_err_t commit_pairing(const unit_pairing_result_t *res)
{
    doorbell_unit_state_blob_t next = {0};
    next.paired = 1;
    next.apt_no = res->apt_no;
    memcpy(next.panel_mac, res->panel_mac, DOORBELL_MAC_SIZE);
    next.ap_channel = res->prov.ap_channel;
    strncpy(next.wifi_ssid, res->prov.wifi_ssid, sizeof(next.wifi_ssid) - 1);
    strncpy(next.wifi_pass, res->prov.wifi_pass, sizeof(next.wifi_pass) - 1);
    next.panel_ipv4 = res->prov.panel_lan_ipv4;
    memcpy(next.k_apt, res->prov.k_apt, DOORBELL_KEY_SIZE);
    next.heartbeat_interval_s = res->prov.heartbeat_interval_s;
    next.tx_counter_ceiling = CONFIG_DOORBELL_NVS_COUNTER_BLOCK;
    next.rx_floor = res->ack_rx_counter;

    unit_lock();
    esp_err_t err = doorbell_storage_unit_save(&next);
    if (err == ESP_OK) {
        s_unit_state = next;
        s_persisted_rx_floor = res->ack_rx_counter;
        s_runtime_tx_counter = 1; /* PAIR_COMPLETE used counter 1 */
        s_known_panel_boot_id = res->panel_boot_id;
        memset(&s_last_ring_ack, 0, sizeof(s_last_ring_ack));
    }
    unit_unlock();
    mbedtls_platform_zeroize(&next, sizeof(next));
    if (err == ESP_OK) {
        g_unit_paired = true;
    }
    return err;
}

static void execute_unit_pairing(int udp_sock)
{
    g_unit_pairing_active = true;
    doorbell_led_start(LED_UNIT_PAIRING_SEARCH);
    ESP_LOGI(TAG, "Starting apartment module pairing flow");

    unit_pairing_result_t res;
    bool ok = unit_pairing_run(udp_sock, &res);
    if (ok) {
        esp_err_t err = commit_pairing(&res);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save pairing state to NVS: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    uint8_t apt_no = res.apt_no;
    mbedtls_platform_zeroize(&res, sizeof(res));

    g_unit_pairing_active = false;
    doorbell_led_stop(LED_UNIT_PAIRING_SEARCH);
    if (ok) {
        s_panel_reachable = true;
        s_missed_heartbeats = 0;
        schedule_next_heartbeat(false);
        doorbell_led_start(LED_UNIT_RESULT_OK);
        doorbell_relay_trigger(&s_bell_relay, NULL); /* 1 bell ring on pairing success */
        ESP_LOGI(TAG, "Successfully paired as apartment %u", (unsigned)apt_no);
    } else {
        doorbell_net_disconnect_sta();
        doorbell_net_set_hostname(UNPAIRED_HOSTNAME);
        doorbell_led_start(LED_UNIT_NO_RESPONSE);
    }
    update_steady_led();
}

/* ---- Operational Packet Handling (RING_REQ) ---- */

static void handle_incoming_frame(int udp_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host)
{
    if (!g_unit_paired || len < DOORBELL_MIN_FRAME_SIZE) {
        return;
    }

    doorbell_hdr_t clear_hdr;
    if (doorbell_hdr_parse(buf, len, &clear_hdr) != ESP_OK) {
        return;
    }

    unit_lock();
    /* Step 2: Pre-crypto destination filter — ignore frames for other apartments without counting */
    if (!g_unit_paired || clear_hdr.dst_id != s_unit_state.apt_no || clear_hdr.src_id != DOORBELL_ID_PANEL ||
            (clear_hdr.flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) != 0) {
        unit_unlock();
        return;
    }

    /* Step 3: Deduplication check — re-send cached ACK on duplicate RING_REQ */
    if (clear_hdr.msg_type == DOORBELL_MSG_RING_REQ && doorbell_dedup_match(&s_last_ring_ack, &clear_hdr, buf, len)) {
        doorbell_udp_send_direct(udp_sock, s_last_ring_ack.frame, s_last_ring_ack.frame_len, from_ip_host,
                                 CONFIG_DOORBELL_UDP_PORT);
        unit_unlock();
        return;
    }

    /* Step 4: Replay check, step 5: AEAD decryption and authentication */
    doorbell_hdr_t auth_hdr;
    uint8_t plain[DOORBELL_PL_MAX_SIZE];
    size_t plain_len = 0;
    if (clear_hdr.tx_counter <= s_unit_state.rx_floor ||
            doorbell_frame_decrypt(buf, len, s_unit_state.k_apt, &auth_hdr, plain, sizeof(plain),
                                   &plain_len) != ESP_OK) {
        unit_unlock();
        if (s_rejected_count < 0xFFFFU) {
            s_rejected_count++;
        }
        return;
    }

    doorbell_pl_ring_req_t req;
    if (auth_hdr.msg_type != DOORBELL_MSG_RING_REQ || doorbell_unpack_ring_req(plain, plain_len, &req) != ESP_OK) {
        unit_unlock();
        return;
    }

    /* Step 6: Commit state & authenticated IP update. The replay floor is persisted on EVERY accepted RING
     * (rings are rare, so flash wear is negligible); otherwise after a module reboot the floor could lag by up to one
     * counter block and a recorded RING_REQ could be replayed to ring the bell again. */
    doorbell_rx_accept(&s_unit_state.rx_floor, &s_unit_state.panel_ipv4, s_persisted_rx_floor, auth_hdr.tx_counter,
                       req.sender_ipv4, from_ip_host, CONFIG_DOORBELL_NVS_COUNTER_BLOCK);
    if (doorbell_storage_unit_save(&s_unit_state) == ESP_OK) {
        s_persisted_rx_floor = s_unit_state.rx_floor;
    }
    s_known_panel_boot_id = req.panel_boot_id;
    s_missed_heartbeats = 0;
    s_panel_reachable = true;

    uint32_t my_ip = 0;
    doorbell_net_get_sta_ipv4(&my_ip);
    uint32_t ack_counter = 0;
    if (doorbell_storage_unit_next_tx_counter(&s_unit_state, &s_runtime_tx_counter,
                                              CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &ack_counter) == ESP_OK) {
        doorbell_pl_ring_ack_t ack_pl = {
            .req_tx_counter = auth_hdr.tx_counter,
            .req_seq_id = auth_hdr.seq_id,
            .status = doorbell_relay_is_active(&s_bell_relay) ? DOORBELL_STATUS_ALREADY_ACTIVE : DOORBELL_STATUS_OK,
            .sender_ipv4 = my_ip,
        };
        uint8_t ack_plain[DOORBELL_PL_RING_ACK_SIZE];
        size_t ack_plain_len = doorbell_pack_ring_ack(&ack_pl, ack_plain);
        doorbell_hdr_t ack_hdr = doorbell_hdr_make(DOORBELL_MSG_RING_ACK, DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
                                                   s_unit_state.apt_no, DOORBELL_ID_PANEL, auth_hdr.seq_id,
                                                   ack_counter);
        if (doorbell_dedup_store(&s_last_ring_ack, &ack_hdr, s_unit_state.k_apt, ack_plain, ack_plain_len, &auth_hdr,
                                 buf, len) == ESP_OK) {
            /* Send ACK BEFORE energizing bell relay (docs/protocol.md §4.1) */
            doorbell_udp_send_direct(udp_sock, s_last_ring_ack.frame, s_last_ring_ack.frame_len, from_ip_host,
                                     CONFIG_DOORBELL_UDP_PORT);
        }
    }
    unit_unlock();

    /* Trigger bell relay and visual bell LED */
    bool was_active = false;
    if (doorbell_relay_trigger(&s_bell_relay, &was_active) == ESP_OK && !was_active) {
        if (s_ring_count < 0xFFFFU) {
            s_ring_count++;
        }
#if CONFIG_DOORBELL_UNIT_VISUAL_BELL
        doorbell_led_start(LED_UNIT_RINGING);
#endif
    }
    update_steady_led();
}

/* ---- OPEN_DOOR with the reboot-replay challenge ---- */

static void execute_open_door_transaction(int udp_sock)
{
    uint32_t my_ip = 0;
    if (!g_unit_paired || !doorbell_net_get_sta_ipv4(&my_ip)) {
        if (s_unacked_open_count < 0xFFFFU) {
            s_unacked_open_count++;
        }
        if (g_unit_paired) {
            s_panel_reachable = false;
            update_steady_led();
        }
        doorbell_led_start(LED_UNIT_NO_RESPONSE);
        return;
    }

    g_unit_open_door_in_flight = true;
    uint64_t echo_nonce = 0;
    bool success = false;

    /* Round 0 (initial) and round 1 (transparent STATUS_CHALLENGE response within the same press) */
    for (int round = 0; round < 2 && !success; round++) {
        unit_request_t req;
        if (unit_request_begin(&req) != ESP_OK) {
            mbedtls_platform_zeroize(&req, sizeof(req));
            break;
        }
        doorbell_pl_open_door_req_t req_pl = {
            .sender_ipv4 = my_ip,
            .panel_boot_id = s_known_panel_boot_id,
            .echo_nonce = echo_nonce,
        };
        uint8_t req_plain[DOORBELL_PL_OPEN_DOOR_REQ_SIZE];
        size_t req_plain_len = doorbell_pack_open_door_req(&req_pl, req_plain);
        uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        unit_ack_wait_t w = {
            .sock = udp_sock,
            .apt_no = req.apt_no,
            .ack_type = DOORBELL_MSG_OPEN_DOOR_ACK,
            .k_apt = req.k_apt,
            .rx_floor = req.rx_floor,
            .req_tx_counter = req.tx_counter,
            .req_seq_id = req.seq_id,
        };
        uint32_t rtt_ms = 0;
        esp_err_t err = unit_request_encrypt(&req, DOORBELL_MSG_OPEN_DOOR_REQ, req_plain, req_plain_len, frame,
                                             &frame_len);
        if (err == ESP_OK) {
            err = unit_run_txn(frame, frame_len, req.panel_ip, CONFIG_DOORBELL_UNIT_OPEN_DOOR_RETRIES, &w, &rtt_ms);
        }
        mbedtls_platform_zeroize(&req, sizeof(req));

        doorbell_pl_open_door_ack_t ack_pl;
        if (err != ESP_OK || doorbell_unpack_open_door_ack(w.plain, w.plain_len, &ack_pl) != ESP_OK) {
            break;
        }
        unit_accept_panel_ack(&w, ack_pl.sender_ipv4, ack_pl.panel_boot_id);

        if (ack_pl.status == DOORBELL_STATUS_CHALLENGE && round == 0) {
            echo_nonce = ack_pl.challenge_nonce;
        } else if (ack_pl.status == DOORBELL_STATUS_OK || ack_pl.status == DOORBELL_STATUS_ALREADY_ACTIVE) {
            ESP_LOGI(TAG, "OPEN_DOOR acknowledged after %lu ms", (unsigned long)rtt_ms);
            success = true;
        } else {
            break;
        }
    }

    g_unit_open_door_in_flight = false;
    if (success) {
        doorbell_led_start(LED_UNIT_RESULT_OK);
        update_steady_led();
    } else {
        if (s_unacked_open_count < 0xFFFFU) {
            s_unacked_open_count++;
        }
        s_panel_reachable = false;
        schedule_next_heartbeat(false);
        doorbell_led_start(LED_UNIT_NO_RESPONSE);
        update_steady_led();
    }
}

/* ---- Confirmable heartbeat ---- */

#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
static void execute_heartbeat_transaction(int udp_sock)
{
    uint32_t my_ip = 0;
    unit_request_t req;
    if (!g_unit_paired || !doorbell_net_get_sta_ipv4(&my_ip) || unit_request_begin(&req) != ESP_OK) {
        mbedtls_platform_zeroize(&req, sizeof(req));
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
        .heartbeat_interval_s = req.heartbeat_interval_s,
    };
    memcpy(hb_pl.ap_bssid, bssid, sizeof(bssid));
    uint8_t hb_plain[DOORBELL_PL_HEARTBEAT_REQ_SIZE];
    size_t hb_plain_len = doorbell_pack_heartbeat_req(&hb_pl, hb_plain);

    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    unit_ack_wait_t w = {
        .sock = udp_sock,
        .apt_no = req.apt_no,
        .ack_type = DOORBELL_MSG_HEARTBEAT_ACK,
        .k_apt = req.k_apt,
        .rx_floor = req.rx_floor,
        .req_tx_counter = req.tx_counter,
        .req_seq_id = req.seq_id,
    };
    esp_err_t err = unit_request_encrypt(&req, DOORBELL_MSG_HEARTBEAT_REQ, hb_plain, hb_plain_len, frame, &frame_len);
    if (err == ESP_OK) {
        err = unit_run_txn(frame, frame_len, req.panel_ip, CONFIG_DOORBELL_HEARTBEAT_RETRIES, &w, NULL);
    }
    mbedtls_platform_zeroize(&req, sizeof(req));

    doorbell_pl_heartbeat_ack_t ack_pl;
    if (err == ESP_OK && doorbell_unpack_heartbeat_ack(w.plain, w.plain_len, &ack_pl) == ESP_OK) {
        unit_accept_panel_ack(&w, ack_pl.sender_ipv4, ack_pl.panel_boot_id);
    } else {
        s_missed_heartbeats++;
        s_panel_reachable = false;
        ESP_LOGW(TAG, "Missed HEARTBEAT_ACK (consecutive=%lu)", (unsigned long)s_missed_heartbeats);
    }
    update_steady_led();
    schedule_next_heartbeat(false);
}
#endif /* CONFIG_DOORBELL_HEARTBEAT_ENABLED */

static void unit_worker_task(void *arg)
{
    (void)arg;
    /* Subscribe worker task to Task Watchdog */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int udp_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, 0, UDP_SOCKET_RCVTIMEO_MS);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket on port %d", CONFIG_DOORBELL_UDP_PORT);
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        unit_poll(NULL);

        if (atomic_exchange(&g_unit_reset_requested, false)) {
            execute_unit_reset();
        }
        if (atomic_exchange(&g_unit_pregen_x25519_requested, false)) {
            unit_pairing_pregenerate_key();
        }
        if (atomic_exchange(&g_unit_pairing_requested, false)) {
            execute_unit_pairing(udp_sock);
        }
        if (atomic_exchange(&g_unit_open_door_requested, false)) {
            execute_open_door_transaction(udp_sock);
        }
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
        if (g_unit_paired && s_sta_has_ip && s_next_heartbeat_us > 0 && esp_timer_get_time() >= s_next_heartbeat_us) {
            execute_heartbeat_transaction(udp_sock);
        }
#endif

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n >= (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
            handle_incoming_frame(udp_sock, rx_buf, (size_t)n, ntohl(from_addr.sin_addr.s_addr));
        }
    }
}

void app_main(void)
{
    s_reset_reason = (uint8_t)esp_reset_reason();
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex != NULL);

    /* 1. Boot-safe Bell Relay initialization BEFORE any other peripheral */
    ESP_ERROR_CHECK(doorbell_relay_init(&s_bell_relay,
                                        CONFIG_DOORBELL_UNIT_GPIO_BELL_RELAY,
                                        DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH,
                                        CONFIG_DOORBELL_UNIT_BELL_PULSE_MS,
                                        &on_bell_relay_off,
                                        NULL));

    /* 2. Status LED initialization (patterns: docs/protocol.md §7.2) */
    static const doorbell_led_indication_t led_table[LED_UNIT_MAX] = {
        [LED_UNIT_RINGING]        = {DOORBELL_LED_FAST_BLINK, 0},
        [LED_UNIT_BTN_CANCEL]     = {DOORBELL_LED_TRIPLE_FLASH, 2 * CONFIG_DOORBELL_LED_MULTI_PERIOD_MS},
        [LED_UNIT_PAIR_HOLD]      = {DOORBELL_LED_MEDIUM_BLINK, 0},
        [LED_UNIT_RESET_HOLD]     = {DOORBELL_LED_LONG_ON_SHORT_OFF, 0},
        [LED_UNIT_RESULT_OK]      = {DOORBELL_LED_SOLID, CONFIG_DOORBELL_LED_OK_DURATION_MS},
        [LED_UNIT_NO_RESPONSE]    = {DOORBELL_LED_MEDIUM_BLINK, CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS},
        [LED_UNIT_PAIRING_SEARCH] = {DOORBELL_LED_SLOW_BLINK, 0},
        [LED_UNIT_UNPAIRED]       = {DOORBELL_LED_DOUBLE_FLASH, 0},
        [LED_UNIT_OUT_OF_COV]     = {DOORBELL_LED_SLOW_BLINK, 0},
        [LED_UNIT_IN_COVERAGE]    = {DOORBELL_LED_DIM, 0},
    };
    ESP_ERROR_CHECK(doorbell_led_init(CONFIG_DOORBELL_UNIT_GPIO_STATUS_LED, led_table, LED_UNIT_MAX, -1));

    /* 3. NVS storage initialization (strictly no auto-erase on error) */
    if (doorbell_storage_init() != ESP_OK) {
        /* Storage fault: never erase. Keep a persistent error indication instead of a one-shot pattern. */
        for (;;) {
            doorbell_led_start(LED_UNIT_BTN_CANCEL);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    if (doorbell_storage_unit_load(&s_unit_state) == ESP_OK && s_unit_state.paired) {
        /* Resume monotonic counter from reserved ceiling */
        s_runtime_tx_counter = s_unit_state.tx_counter_ceiling;
        s_persisted_rx_floor = s_unit_state.rx_floor;
        g_unit_paired = true;
        ESP_LOGI(TAG, "Loaded paired state for apartment %u (last restart: %s)",
                 (unsigned)s_unit_state.apt_no, doorbell_reset_reason_str(s_reset_reason));
    } else {
        memset(&s_unit_state, 0, sizeof(s_unit_state));
        ESP_LOGI(TAG, "Apartment module is currently unpaired (last restart: %s)",
                 doorbell_reset_reason_str(s_reset_reason));
    }

    /* 4. Network initialization; starting the radio also enables RF RNG entropy */
    ESP_ERROR_CHECK(doorbell_net_init(UNPAIRED_HOSTNAME, &on_net_state_changed, NULL));
    if (g_unit_paired) {
        unit_set_apartment_hostname(s_unit_state.apt_no);
    }

    update_steady_led();

    if (g_unit_paired && s_unit_state.wifi_ssid[0] != '\0') {
        doorbell_net_connect_sta(s_unit_state.wifi_ssid, s_unit_state.wifi_pass);
    }

    /* 5. Buttons initialization */
    ESP_ERROR_CHECK(unit_buttons_init());

    /* 6. Register esp_timer and FreeRTOS timer pipeline health handles with TWDT */
    ESP_ERROR_CHECK(doorbell_twdt_register_timer_pipelines("unit_esp_timer", "unit_rtos_timer"));

    /* 7. Start TWDT-monitored UDP worker task */
    BaseType_t created = xTaskCreate(unit_worker_task, "unit_worker", 6144, NULL, 5, NULL);
    configASSERT(created == pdPASS);
}
