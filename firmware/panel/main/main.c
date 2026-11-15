#include "panel_state.h"
#include <stdatomic.h>
#include <stddef.h>
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
#include "esp_task_wdt.h"
#include "doorbell_twdt.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "doorbell_panel";

#define LAN_SOCKET_RCVTIMEO_MS 50

/* Shared global state */
SemaphoreHandle_t g_panel_mutex = NULL;
doorbell_panel_cfg_blob_t g_panel_cfg = {0};
panel_apt_slot_t g_apt_slots[DOORBELL_APT_SLOT_COUNT] = {0};
_Atomic panel_mode_t g_panel_mode = MODE_IDLE;
uint32_t g_panel_boot_id = 0;
doorbell_relay_t g_door_relay = {0};

/* Persistent silence-restart backoff in RTC_NOINIT memory protected by magic + CRC32 */
#define RTC_SILENCE_MAGIC     0x44425254UL /* 'DBRT' */
#define SILENCE_MAX_TIMEOUT_S (48ULL * 3600ULL)
/* Backoff doublings needed to reach the 48 h cap from the shortest base (3 x 30 s heartbeat): 2^11 x 90 s > 48 h */
#define SILENCE_MAX_LEVEL     11U
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
/* panel_cfg could not be loaded at boot: keypad codes are disabled until re-provisioned (fail closed) */
static bool s_panel_cfg_fault = false;
static uint32_t s_cfg_fault_led_divider = 0;
static uint32_t s_unregistered_packets = 0;
static uint32_t s_rejected_messages = 0;

/* ---- RTC Silence Backoff Helpers ---- */

static uint32_t rtc_silence_crc(void)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&s_rtc_silence, offsetof(rtc_silence_state_t, crc32));
}

static void rtc_silence_set_level(uint32_t level)
{
    if (level > SILENCE_MAX_LEVEL) {
        level = SILENCE_MAX_LEVEL;
    }
    if (s_rtc_silence.backoff_level != level || s_rtc_silence.magic != RTC_SILENCE_MAGIC) {
        s_rtc_silence.magic = RTC_SILENCE_MAGIC;
        s_rtc_silence.backoff_level = level;
        s_rtc_silence.crc32 = rtc_silence_crc();
    }
}

static void rtc_silence_init_or_validate(void)
{
    if (s_rtc_silence.magic != RTC_SILENCE_MAGIC || s_rtc_silence.crc32 != rtc_silence_crc() ||
            s_rtc_silence.backoff_level > SILENCE_MAX_LEVEL) {
        s_rtc_silence.magic = 0; /* force the rewrite below */
        rtc_silence_set_level(0);
    }
    s_last_any_module_rx_us = esp_timer_get_time();
}

/* ---- Network & Relay Callbacks ---- */

static void on_door_relay_off(void *ctx)
{
    (void)ctx;
    doorbell_led_stop(LED_PANEL_DOOR_OPEN);
}

static void on_panel_net_state(bool connected, uint32_t ipv4_host, void *ctx)
{
    (void)ipv4_host;
    (void)ctx;
    if (connected) {
        doorbell_led_stop(LED_PANEL_NET_LOST);
    } else {
        doorbell_led_start(LED_PANEL_NET_LOST);
    }
}

/* Periodic work while the worker task waits for an ACK: task watchdog and door relay safety clamp */
static void panel_poll_cb(void *ctx)
{
    (void)ctx;
    esp_task_wdt_reset();
    doorbell_relay_safety_check(&g_door_relay);
}

/* ---- Keypad actions (caller holds g_panel_mutex) ---- */

void panel_action_open_door_local_locked(void)
{
    PANEL_ASSERT_LOCKED();
    if (doorbell_relay_trigger(&g_door_relay, NULL) == ESP_OK) {
        doorbell_led_start(LED_PANEL_DOOR_OPEN);
        ESP_LOGI(TAG, "Door unlocked via keypad user code");
    } else {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
}

static bool apt_registered_locked(uint8_t apt_no)
{
    return apt_no >= CONFIG_DOORBELL_PANEL_MIN_APT_NO && apt_no <= CONFIG_DOORBELL_PANEL_MAX_APT_NO &&
           g_apt_slots[apt_no].nvs.registered;
}

void panel_action_ring_apartment_locked(uint8_t apt_no)
{
    PANEL_ASSERT_LOCKED();
    if (!apt_registered_locked(apt_no)) {
        /* No registered apartment under this number: 1 s fast blink (kullanim-kilavuzu.md §2) */
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }
    if (xQueueSend(s_ring_queue, &apt_no, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Ring request for apartment %u dropped: %d rings already queued", (unsigned)apt_no,
                 RING_QUEUE_LEN);
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
}

void panel_action_delete_apartment_locked(uint8_t apt_no)
{
    PANEL_ASSERT_LOCKED();
    if (!apt_registered_locked(apt_no)) {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }
    if (doorbell_storage_panel_erase_apt(apt_no) == ESP_OK) {
        memset(&g_apt_slots[apt_no], 0, sizeof(g_apt_slots[apt_no]));
        ESP_LOGI(TAG, "Deleted registration for apartment %u", (unsigned)apt_no);
        doorbell_led_start(LED_PANEL_RESULT_OK);
    } else {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
}

/* ---- Requests executed later by panel_worker_task ---- */

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
    doorbell_led_start(ok ? LED_PANEL_RESULT_OK : LED_PANEL_RESULT_ERR);
    for (int i = 0; i < 20; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_restart();
}

/* Close whichever temporary mode (pairing discovery, portal) owns the radio. Caller holds g_panel_mutex. */
static void cancel_active_mode_locked(void)
{
    if (g_panel_mode == MODE_PAIR_DISCOVER) {
        ESP_LOGI(TAG, "Pairing discovery cancelled");
        panel_pairing_abort_locked(false);
    } else if (g_panel_mode == MODE_PORTAL) {
        panel_portal_stop();
    }
}

static void execute_pending_panel_actions(void)
{
    if (atomic_exchange(&s_pending_building_reset, false)) {
        ESP_LOGW(TAG, "Executing full building reset (*<admin>*6*<admin>#)");
        panel_lock();
        panel_pairing_abort_locked(false);
        panel_portal_stop();
        /* Building reset erases apartment registrations and keys only; user/admin codes and building Wi-Fi
         * credentials are preserved (kullanim-kilavuzu.md §6.3) */
        esp_err_t err = doorbell_storage_panel_erase_all_apts(CONFIG_DOORBELL_PANEL_MIN_APT_NO,
                                                              CONFIG_DOORBELL_PANEL_MAX_APT_NO);
        memset(g_apt_slots, 0, sizeof(g_apt_slots));
        panel_unlock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Building reset incomplete (%s); restarting to reload the stored registrations",
                     esp_err_to_name(err));
        }
        show_result_and_restart(err == ESP_OK);
    }

    if (atomic_exchange(&s_pending_factory_reset, false)) {
        panel_lock();
        cancel_active_mode_locked();
        /* Back to the provisioned state (Burn / Flash): factory codes, factory building Wi-Fi, no lockout, no
         * apartment registrations. Storage commits panel_cfg first and erases registrations only after that */
        bool nvs_changed = false;
        esp_err_t err = doorbell_storage_panel_factory_reset(&g_panel_cfg, CONFIG_DOORBELL_PANEL_MIN_APT_NO,
                                                             CONFIG_DOORBELL_PANEL_MAX_APT_NO, &nvs_changed);
        if (nvs_changed) {
            panel_lockout_clear_locked();
            memset(g_apt_slots, 0, sizeof(g_apt_slots)); /* never serve a half-erased set until the restart */
        }
        panel_unlock();

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
            doorbell_led_start(LED_PANEL_RESULT_ERR);
        }
    }

    if (atomic_exchange(&s_pending_cancel_mode, false)) {
        panel_lock();
        cancel_active_mode_locked();
        panel_unlock();
    }

    if (atomic_exchange(&s_pending_start_portal, false)) {
        panel_lock();
        esp_err_t err = panel_portal_start();
        panel_unlock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Setup portal could not start: %s", esp_err_to_name(err));
            doorbell_led_start(LED_PANEL_RESULT_ERR);
        }
    }

    uint8_t pair_apt = atomic_exchange(&s_pending_pair_apt, 0);
    if (pair_apt != 0) {
        panel_lock();
        panel_pairing_start_locked(pair_apt);
        panel_unlock();
    }
}

/* ---- Module frames: dispatch under g_panel_mutex, side effects (relay, logging) after releasing it ---- */

/* Work of a handled frame that must run after g_panel_mutex is released */
typedef struct {
    bool open_door;
    bool heartbeat_valid;
    doorbell_pl_heartbeat_req_t heartbeat;
} lan_followup_t;

void panel_note_module_rx_locked(panel_apt_slot_t *slot, int64_t now_us)
{
    PANEL_ASSERT_LOCKED();
    slot->last_heartbeat_us = now_us;
    slot->no_report_warned = false;
    s_last_any_module_rx_us = now_us;
    rtc_silence_set_level(0);
}

/* Accepted authenticated message: advance the replay floor, learn a changed module IP and persist both in
 * write-ahead blocks. Caller holds g_panel_mutex. */
static void slot_accept_rx_locked(panel_apt_slot_t *slot, uint32_t rx_counter, uint32_t sender_ipv4,
                                  uint32_t from_ip_host)
{
    PANEL_ASSERT_LOCKED();
    if (doorbell_rx_accept(&slot->nvs.rx_floor, &slot->nvs.last_ipv4, slot->persisted_rx_floor, rx_counter,
                           sender_ipv4, from_ip_host, CONFIG_DOORBELL_NVS_COUNTER_BLOCK)) {
        if (doorbell_storage_panel_save_apt(&slot->nvs) == ESP_OK) {
            slot->persisted_rx_floor = slot->nvs.rx_floor;
        }
    }
}

/* The cache keeps the request's CCM tag so only a byte-identical retry is answered from it */
void panel_send_ack_locked(int lan_sock, panel_apt_slot_t *slot, uint8_t ack_msg_type, const doorbell_hdr_t *req_hdr,
                           const uint8_t *req_frame, size_t req_len, uint32_t ack_counter, const uint8_t *ack_plain,
                           size_t ack_plain_len, uint32_t to_ip_host)
{
    PANEL_ASSERT_LOCKED();
    doorbell_hdr_t ack_hdr = doorbell_hdr_make(ack_msg_type, 0, DOORBELL_ID_PANEL, slot->nvs.apt_no, req_hdr->seq_id,
                                               ack_counter);
    if (doorbell_dedup_store(&slot->last_ack, &ack_hdr, slot->nvs.k_apt, ack_plain, ack_plain_len, req_hdr, req_frame,
                             req_len) != ESP_OK) {
        return;
    }
    doorbell_udp_send_direct(lan_sock, slot->last_ack.frame, slot->last_ack.frame_len, to_ip_host,
                             CONFIG_DOORBELL_UDP_PORT);
}

/* OPEN_DOOR_REQ with reboot-replay / withheld-packet freshness check: an unverified request gets a
 * STATUS_CHALLENGE ACK, a request echoing the live challenge opens the door. Caller holds g_panel_mutex.
 * @return true if the door relay must be energized (by the caller, after releasing the mutex) */
static bool handle_open_door_locked(int lan_sock, panel_apt_slot_t *slot, const doorbell_hdr_t *auth_hdr,
                                    const uint8_t *plain, size_t plain_len, const uint8_t *buf, size_t len,
                                    uint32_t from_ip_host, uint32_t panel_ip, int64_t now_us)
{
    doorbell_pl_open_door_req_t req;
    if (doorbell_unpack_open_door_req(plain, plain_len, &req) != ESP_OK) {
        return false;
    }
    slot_accept_rx_locked(slot, auth_hdr->tx_counter, req.sender_ipv4, from_ip_host);

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
        ack_pl.status = doorbell_relay_is_active(&g_door_relay) ? DOORBELL_STATUS_ALREADY_ACTIVE : DOORBELL_STATUS_OK;
        open_door = true;
    }
    uint8_t ack_plain[DOORBELL_PL_OPEN_DOOR_ACK_SIZE];
    size_t ack_plain_len = doorbell_pack_open_door_ack(&ack_pl, ack_plain);
    panel_send_ack_locked(lan_sock, slot, DOORBELL_MSG_OPEN_DOOR_ACK, auth_hdr, buf, len, ack_cnt,
                          ack_plain, ack_plain_len, from_ip_host);
    return open_door;
}

/* HEARTBEAT_REQ: liveness, replay floor, telemetry. Caller holds g_panel_mutex.
 * @return true if @p out_hb holds a valid report to log */
static bool handle_heartbeat_locked(int lan_sock, panel_apt_slot_t *slot, const doorbell_hdr_t *auth_hdr,
                                    const uint8_t *plain, size_t plain_len, const uint8_t *buf, size_t len,
                                    uint32_t from_ip_host, uint32_t panel_ip, doorbell_pl_heartbeat_req_t *out_hb)
{
    if (doorbell_unpack_heartbeat_req(plain, plain_len, out_hb) != ESP_OK) {
        return false;
    }
    slot_accept_rx_locked(slot, auth_hdr->tx_counter, out_hb->sender_ipv4, from_ip_host);

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
        uint8_t ack_plain[DOORBELL_PL_HEARTBEAT_ACK_SIZE];
        size_t ack_plain_len = doorbell_pack_heartbeat_ack(&ack_pl, ack_plain);
        panel_send_ack_locked(lan_sock, slot, DOORBELL_MSG_HEARTBEAT_ACK, auth_hdr, buf, len, ack_cnt,
                              ack_plain, ack_plain_len, from_ip_host);
    }
    return true;
}

/* One module frame after the pre-crypto filter: pairing commit, deduplication, replay check, AEAD, then the
 * message handler. Caller holds g_panel_mutex; post-unlock work is returned in @p follow. */
static void handle_frame_locked(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host,
                                const doorbell_hdr_t *clear_hdr, lan_followup_t *follow)
{
    if (panel_pairing_handle_complete_locked(lan_sock, buf, len, from_ip_host, clear_hdr)) {
        return;
    }

    uint8_t apt_no = clear_hdr->src_id;
    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    if (!slot->nvs.registered) {
        s_unregistered_packets++;
        if ((s_unregistered_packets & 0x1FU) == 1U) {
            ESP_LOGW(TAG, "Dropped packet from unregistered apartment %u (total=%lu)",
                     (unsigned)apt_no, (unsigned long)s_unregistered_packets);
        }
        return;
    }

    /* Duplicate request: re-send the cached ACK */
    if (doorbell_dedup_match(&slot->last_ack, clear_hdr, buf, len)) {
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
    uint8_t plain[DOORBELL_PL_MAX_SIZE];
    size_t plain_len = 0;
    if (doorbell_frame_decrypt(buf, len, slot->nvs.k_apt, &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK) {
        s_rejected_messages++;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    panel_note_module_rx_locked(slot, now_us);
    uint32_t panel_ip = 0;
    doorbell_net_get_sta_ipv4(&panel_ip);

    if (auth_hdr.msg_type == DOORBELL_MSG_OPEN_DOOR_REQ) {
        follow->open_door = handle_open_door_locked(lan_sock, slot, &auth_hdr, plain, plain_len, buf, len,
                                                    from_ip_host, panel_ip, now_us);
    } else if (auth_hdr.msg_type == DOORBELL_MSG_HEARTBEAT_REQ) {
        follow->heartbeat_valid = handle_heartbeat_locked(lan_sock, slot, &auth_hdr, plain, plain_len, buf, len,
                                                          from_ip_host, panel_ip, &follow->heartbeat);
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
    /* Pre-crypto destination & direction filter */
    if (clear_hdr.dst_id != DOORBELL_ID_PANEL ||
            (clear_hdr.flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) == 0 ||
            clear_hdr.src_id < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
            clear_hdr.src_id > CONFIG_DOORBELL_PANEL_MAX_APT_NO) {
        return;
    }

    lan_followup_t follow = {0};
    panel_lock();
    handle_frame_locked(lan_sock, buf, len, from_ip_host, &clear_hdr, &follow);
    panel_unlock();

    uint8_t apt_no = clear_hdr.src_id;
    if (follow.open_door && doorbell_relay_trigger(&g_door_relay, NULL) == ESP_OK) {
        doorbell_led_start(LED_PANEL_DOOR_OPEN);
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

/* ---- RING transaction ---- */

typedef struct {
    int lan_sock;
    uint8_t apt_no;
    const uint8_t *k_apt;
    uint32_t tx_counter;
    uint16_t seq_id;
} ring_txn_ctx_t;

/* RING_ACK of this transaction, or another module frame handled as usual while the panel waits */
static doorbell_txn_verdict_t ring_ack_rx_cb(const uint8_t *frame, size_t len, const doorbell_hdr_t *hdr,
                                             uint32_t from_ip, void *arg)
{
    ring_txn_ctx_t *ctx = (ring_txn_ctx_t *)arg;
    if (hdr->msg_type != DOORBELL_MSG_RING_ACK || hdr->src_id != ctx->apt_no) {
        handle_lan_packet(ctx->lan_sock, frame, len, from_ip);
        return DOORBELL_TXN_CONTINUE;
    }

    doorbell_txn_verdict_t verdict = DOORBELL_TXN_CONTINUE;
    panel_lock();
    panel_apt_slot_t *slot = &g_apt_slots[ctx->apt_no];
    if (!slot->nvs.registered) {
        /* handle_lan_packet() above may have removed this registration (re-pairing under another number) */
        verdict = DOORBELL_TXN_ABORT;
    } else if (hdr->tx_counter > slot->nvs.rx_floor) {
        doorbell_hdr_t auth_hdr;
        uint8_t plain[DOORBELL_PL_MAX_SIZE];
        size_t plain_len = 0;
        doorbell_pl_ring_ack_t ack;
        if (doorbell_frame_decrypt(frame, len, ctx->k_apt, &auth_hdr, plain, sizeof(plain), &plain_len) == ESP_OK &&
                doorbell_unpack_ring_ack(plain, plain_len, &ack) == ESP_OK &&
                ack.req_tx_counter == ctx->tx_counter && ack.req_seq_id == ctx->seq_id &&
                (ack.status == DOORBELL_STATUS_OK || ack.status == DOORBELL_STATUS_ALREADY_ACTIVE)) {
            slot_accept_rx_locked(slot, auth_hdr.tx_counter, ack.sender_ipv4, from_ip);
            panel_note_module_rx_locked(slot, esp_timer_get_time());
            verdict = DOORBELL_TXN_DONE;
        }
    }
    panel_unlock();
    return verdict;
}

static void execute_ring_transaction(int lan_sock, uint8_t apt_no)
{
    uint32_t panel_ip = 0;
    if (!doorbell_net_get_sta_ipv4(&panel_ip)) {
        doorbell_led_start(LED_PANEL_NO_RESPONSE);
        return;
    }

    panel_lock();
    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    if (!slot->nvs.registered) {
        panel_unlock();
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }
    uint32_t tx_counter = 0;
    esp_err_t cnt_err = doorbell_storage_panel_next_tx_counter(&slot->nvs, &slot->runtime_tx_counter,
                                                               CONFIG_DOORBELL_NVS_COUNTER_BLOCK, &tx_counter);
    uint16_t seq_id = s_panel_next_seq_id++;
    uint32_t peer_ip = slot->nvs.last_ipv4;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    memcpy(k_apt, slot->nvs.k_apt, DOORBELL_KEY_SIZE);
    panel_unlock();

    esp_err_t err = cnt_err;
    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    if (err == ESP_OK) {
        doorbell_pl_ring_req_t req_pl = {
            .sender_ipv4 = panel_ip,
            .panel_boot_id = g_panel_boot_id,
        };
        uint8_t req_plain[DOORBELL_PL_RING_REQ_SIZE];
        size_t req_plain_len = doorbell_pack_ring_req(&req_pl, req_plain);
        doorbell_hdr_t req_hdr = doorbell_hdr_make(DOORBELL_MSG_RING_REQ, 0, DOORBELL_ID_PANEL, apt_no, seq_id,
                                                   tx_counter);
        err = doorbell_frame_encrypt(&req_hdr, k_apt, req_plain, req_plain_len, frame, sizeof(frame), &frame_len);
    }

    uint32_t rtt_ms = 0;
    if (err == ESP_OK) {
        ring_txn_ctx_t ctx = {
            .lan_sock = lan_sock,
            .apt_no = apt_no,
            .k_apt = k_apt,
            .tx_counter = tx_counter,
            .seq_id = seq_id,
        };
        const doorbell_txn_t txn = {
            .sock = lan_sock,
            .frame = frame,
            .frame_len = frame_len,
            .peer_ipv4_host = peer_ip,
            .port = CONFIG_DOORBELL_UDP_PORT,
            .retries = CONFIG_DOORBELL_PANEL_RING_RETRIES,
            .ack_timeout_ms = CONFIG_DOORBELL_ACK_TIMEOUT_MS,
            .on_rx = ring_ack_rx_cb,
            .on_poll = panel_poll_cb,
            .ctx = &ctx,
        };
        err = doorbell_txn_run(&txn, &rtt_ms);
    }
    mbedtls_platform_zeroize(k_apt, sizeof(k_apt));

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RING to apt %u acknowledged after %lu ms", (unsigned)apt_no, (unsigned long)rtt_ms);
        doorbell_led_start(LED_PANEL_RESULT_OK);
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "RING to apartment %u timed out without ACK", (unsigned)apt_no);
        doorbell_led_start(LED_PANEL_NO_RESPONSE);
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "RING to apartment %u stopped: registration removed meanwhile", (unsigned)apt_no);
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    } else {
        ESP_LOGE(TAG, "RING to apartment %u not sent: %s", (unsigned)apt_no, esp_err_to_name(err));
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
}

/* ---- Heartbeat Liveness & Silence Restart Monitor ---- */

static void check_module_health_1s(void)
{
#if CONFIG_DOORBELL_HEARTBEAT_ENABLED
    int64_t now_us = esp_timer_get_time();
    int64_t miss_threshold_us = (int64_t)CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S * 3LL * 1000000LL;
    uint32_t reg_count = 0;

    panel_lock();
    for (unsigned apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; apt++) {
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
            ESP_LOGW(TAG, "No report from apartment %u for 3 heartbeat intervals", apt);
        }
    }
    panel_unlock();

#if CONFIG_DOORBELL_PANEL_SILENCE_RESTART
    if (reg_count > 0 && g_panel_mode == MODE_IDLE) {
        uint64_t timeout_s = ((uint64_t)CONFIG_DOORBELL_HEARTBEAT_INTERVAL_S * 3ULL) << s_rtc_silence.backoff_level;
        if (timeout_s > SILENCE_MAX_TIMEOUT_S) {
            timeout_s = SILENCE_MAX_TIMEOUT_S;
        }
        if ((now_us - s_last_any_module_rx_us) >= (int64_t)(timeout_s * 1000000ULL)) {
            uint32_t next_level = s_rtc_silence.backoff_level + 1U;
            rtc_silence_set_level(next_level);
            ESP_LOGW(TAG, "No module reported for %llu s — restarting panel (next backoff level=%lu)",
                     (unsigned long long)timeout_s, (unsigned long)s_rtc_silence.backoff_level);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
#endif
#endif
}

/* ---- Main TWDT-Monitored Worker Task ---- */

static void panel_worker_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int lan_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, 0, LAN_SOCKET_RCVTIMEO_MS);
    if (lan_sock < 0) {
        ESP_LOGE(TAG, "Failed to create panel LAN UDP socket");
        esp_task_wdt_delete(NULL);
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
            /* Persistent storage-fault indication: repeat the error pattern every 10 s */
            if (s_panel_cfg_fault && (++s_cfg_fault_led_divider >= 10)) {
                s_cfg_fault_led_divider = 0;
                doorbell_led_start(LED_PANEL_RESULT_ERR);
            }
        }

        if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PAIR_COMPLETE) {
            panel_pairing_step();
        }

        uint8_t ring_apt = 0;
        if (xQueueReceive(s_ring_queue, &ring_apt, 0) == pdTRUE) {
            execute_ring_transaction(lan_sock, ring_apt);
        }

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(lan_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n >= (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
            handle_lan_packet(lan_sock, rx_buf, (size_t)n, ntohl(from_addr.sin_addr.s_addr));
        }
    }
}

static void load_panel_cfg(void)
{
    esp_err_t cfg_err = doorbell_storage_panel_load_cfg(&g_panel_cfg);
    if (cfg_err != ESP_OK) {
        /* A missing or unreadable panel_cfg blob must not abort() into an endless boot loop that disables visitor rings
         * and apartment-initiated door open, and must never fall back to shared hard-coded codes. Keep codes and
         * building Wi-Fi empty (fail closed; doorbell_storage leaves any existing blob intact and write-protected)
         * until provisioned/re-flashed with the flasher. */
        ESP_LOGE(TAG, "CRITICAL: panel_cfg unavailable (%s) — keypad codes and building Wi-Fi disabled; "
                 "provision with the flasher", esp_err_to_name(cfg_err));
        mbedtls_platform_zeroize(&g_panel_cfg, sizeof(g_panel_cfg));
        s_panel_cfg_fault = true;
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }
    /* First boot after Burn / Flash: remember the provisioned building Wi-Fi for the hidden-button factory reset
     * (no-op once recorded). Without it the factory reset refuses rather than guessing. */
    esp_err_t fw_err = doorbell_storage_panel_snapshot_factory_wifi(&g_panel_cfg);
    if (fw_err != ESP_OK) {
        ESP_LOGW(TAG, "Factory Wi-Fi record unavailable (%s): hidden-button factory reset will be refused",
                 esp_err_to_name(fw_err));
    }
}

static void load_registrations(void)
{
    uint32_t loaded_apts = 0;
    int64_t boot_us = esp_timer_get_time();
    for (unsigned apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; apt++) {
        panel_apt_slot_t *slot = &g_apt_slots[apt];
        if (doorbell_storage_panel_load_apt((uint8_t)apt, &slot->nvs) == ESP_OK && slot->nvs.registered) {
            /* Resume monotonic counter from reserved write-ahead ceiling */
            slot->runtime_tx_counter = slot->nvs.tx_counter_ceiling;
            slot->persisted_rx_floor = slot->nvs.rx_floor;
            slot->last_heartbeat_us = boot_us;
            loaded_apts++;
        }
    }
    ESP_LOGI(TAG, "Loaded %lu registered apartment(s) from NVS (last restart: %s)",
             (unsigned long)loaded_apts, doorbell_reset_reason_str((uint8_t)esp_reset_reason()));
}

void app_main(void)
{
    g_panel_mutex = xSemaphoreCreateMutex();
    configASSERT(g_panel_mutex != NULL);
    s_ring_queue = xQueueCreate(RING_QUEUE_LEN, sizeof(uint8_t));
    configASSERT(s_ring_queue != NULL);

    rtc_silence_init_or_validate();

    /* 1. Boot-safe Door Relay initialization BEFORE any other peripheral */
    ESP_ERROR_CHECK(doorbell_relay_init(&g_door_relay,
                                        CONFIG_DOORBELL_PANEL_GPIO_DOOR_RELAY,
                                        !DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW,
                                        CONFIG_DOORBELL_PANEL_DOOR_PULSE_MS,
                                        &on_door_relay_off,
                                        NULL));

    /* 2. Status LED initialization (patterns: docs/protocol.md §7.1) */
    static const doorbell_led_indication_t led_table[LED_PANEL_MAX] = {
        [LED_PANEL_KEY_PRESS]   = {DOORBELL_LED_SHORT_FLASH, 0},
        [LED_PANEL_DOOR_OPEN]   = {DOORBELL_LED_SOLID, CONFIG_DOORBELL_PANEL_DOOR_PULSE_MS},
        [LED_PANEL_RESULT_OK]   = {DOORBELL_LED_SOLID, CONFIG_DOORBELL_LED_OK_DURATION_MS},
        [LED_PANEL_RESULT_ERR]  = {DOORBELL_LED_FAST_BLINK, CONFIG_DOORBELL_LED_ERR_DURATION_MS},
        [LED_PANEL_NO_RESPONSE] = {DOORBELL_LED_MEDIUM_BLINK, CONFIG_DOORBELL_LED_NO_RESP_DURATION_MS},
        [LED_PANEL_RESET_HOLD]  = {DOORBELL_LED_LONG_ON_SHORT_OFF, 0},
        [LED_PANEL_TYPING]      = {DOORBELL_LED_SLOW_BLINK, 0},
        [LED_PANEL_PAIRING]     = {DOORBELL_LED_DOUBLE_FLASH, 0},
        [LED_PANEL_PORTAL]      = {DOORBELL_LED_SLOW_BLINK, 0},
        [LED_PANEL_LOCKOUT]     = {DOORBELL_LED_SPARSE_FLASH, 0},
        [LED_PANEL_NET_LOST]    = {DOORBELL_LED_SPARSE_FLASH, 0},
        [LED_PANEL_IDLE]        = {DOORBELL_LED_OFF, 0},
    };
    ESP_ERROR_CHECK(doorbell_led_init(CONFIG_DOORBELL_PANEL_GPIO_STATUS_LED, led_table, LED_PANEL_MAX, LED_PANEL_IDLE));
    doorbell_led_start(LED_PANEL_NET_LOST);

    /* 3. NVS storage initialization — auto-erase strictly forbidden on panel */
    if (doorbell_storage_init() != ESP_OK) {
        /* Storage fault: never erase. Keep a persistent error indication instead of a one-shot blink. */
        for (;;) {
            doorbell_led_start(LED_PANEL_RESULT_ERR);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    load_panel_cfg();
    load_registrations();

    /* 4. Network initialization; starting the radio also enables RF RNG entropy */
    ESP_ERROR_CHECK(doorbell_net_init("doorbell-panel", &on_panel_net_state, NULL));

    /* Generate non-zero random 32-bit panel_boot_id after RF entropy is available */
    do {
        ESP_ERROR_CHECK(doorbell_crypto_random((uint8_t *)&g_panel_boot_id, sizeof(g_panel_boot_id)));
    } while (g_panel_boot_id == 0);

    if (g_panel_cfg.wifi_ssid[0] != '\0') {
        doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
    } else {
        ESP_LOGW(TAG, "No building Wi-Fi SSID configured yet — use *<admin>*5# portal or UART console");
    }

    /* 5. Matrix keypad & hidden reset button initialization */
    ESP_ERROR_CHECK(panel_keypad_init());

    /* 6. Authenticated UART console initialization (best effort: the panel works without it) */
    panel_console_init();

    /* 7. Register esp_timer and FreeRTOS timer pipeline health handles with TWDT */
    ESP_ERROR_CHECK(doorbell_twdt_register_timer_pipelines("panel_esp_timer", "panel_rtos_timer"));

    /* 8. Start TWDT-monitored panel worker task */
    BaseType_t created = xTaskCreate(panel_worker_task, "panel_worker", 8192, NULL, 5, NULL);
    configASSERT(created == pdPASS);
}
