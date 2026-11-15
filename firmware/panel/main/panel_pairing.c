/* Two-phase pairing server of the panel: open SoftAP discovery window, PAIR_HELLO
 * collision window, PAIR_CHALLENGE / PAIR_CONFIRM on the SoftAP, then PAIR_COMPLETE over the building LAN. */
#include "panel_state.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "panel_pairing";

#define PAIR_COLLISION_WINDOW_US 1500000LL /* 1.5 s multi-module collision listen */
#define PAIR_SOCKET_RCVTIMEO_MS  50

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
    uint8_t panel_mac[DOORBELL_MAC_SIZE];
    uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE];
    doorbell_x25519_ctx_t x25519;
    uint8_t k_pair[DOORBELL_KEY_SIZE];
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    /* Byte-identical PAIR_CHALLENGE kept for retransmission when the module repeats PAIR_HELLO */
    uint8_t challenge_pkt[DOORBELL_PAIR_CHALLENGE_FRAME_SIZE];
    size_t challenge_len;
} panel_pair_session_t;

static panel_pair_session_t s_pair_sess = {.softap_sock = -1};

static void close_pair_softap_locked(void)
{
    if (s_pair_sess.softap_sock >= 0) {
        close(s_pair_sess.softap_sock);
        s_pair_sess.softap_sock = -1;
    }
    doorbell_net_stop_softap();
    doorbell_net_set_sta_reconnect_paused(false);
}

void panel_pairing_abort_locked(bool show_error_led)
{
    PANEL_ASSERT_LOCKED();
    close_pair_softap_locked();
    doorbell_x25519_free(&s_pair_sess.x25519);
    /* Static storage: the wipe of the keys cannot be optimized away */
    memset(&s_pair_sess, 0, sizeof(s_pair_sess));
    s_pair_sess.softap_sock = -1;

    doorbell_led_stop(LED_PANEL_PAIRING);
    if (g_panel_mode == MODE_PAIR_DISCOVER || g_panel_mode == MODE_PAIR_COMPLETE) {
        g_panel_mode = MODE_IDLE;
    }
    if (show_error_led) {
        doorbell_led_start(LED_PANEL_RESULT_ERR);
    }
}

void panel_pairing_start_locked(uint8_t apt_no)
{
    PANEL_ASSERT_LOCKED();
    /* Pre-check: STA must be connected with valid IPv4 lease and apartment slot must be free */
    uint32_t panel_lan_ip = 0;
    if (g_panel_mode != MODE_IDLE ||
            !doorbell_net_get_sta_ipv4(&panel_lan_ip) ||
            apt_no < CONFIG_DOORBELL_PANEL_MIN_APT_NO ||
            apt_no > CONFIG_DOORBELL_PANEL_MAX_APT_NO ||
            g_apt_slots[apt_no].nvs.registered) {
        ESP_LOGW(TAG, "Rejected pairing start for apt %u (mode=%d, lan_ip=0x%08lx)",
                 (unsigned)apt_no, (int)g_panel_mode, (unsigned long)panel_lan_ip);
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    panel_pairing_abort_locked(false);

    uint8_t ap_mac[DOORBELL_MAC_SIZE] = {0};
    doorbell_net_get_ap_mac(ap_mac);

    char pair_ssid[33];
    snprintf(pair_ssid, sizeof(pair_ssid), DOORBELL_PAIR_SSID_PREFIX "%02X%02X",
             (unsigned)ap_mac[4], (unsigned)ap_mac[5]);

    doorbell_net_set_sta_reconnect_paused(true);

    uint32_t softap_ip = 0;
    if (doorbell_net_start_softap(pair_ssid, NULL, 1, &softap_ip) != ESP_OK) {
        doorbell_net_set_sta_reconnect_paused(false);
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    /* Bind pairing listener strictly to the SoftAP: its IPv4 address AND its netif (SO_BINDTODEVICE), since lwIP's
     * weak host model would otherwise accept datagrams for the SoftAP address arriving from the building LAN */
    int pair_sock = doorbell_udp_socket_create(CONFIG_DOORBELL_UDP_PORT, softap_ip, PAIR_SOCKET_RCVTIMEO_MS);
    if (pair_sock >= 0 && doorbell_net_bind_socket_to_softap(pair_sock) != ESP_OK) {
        close(pair_sock);
        pair_sock = -1;
    }
    if (pair_sock < 0) {
        doorbell_net_stop_softap();
        doorbell_net_set_sta_reconnect_paused(false);
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return;
    }

    s_pair_sess.apt_no = apt_no;
    s_pair_sess.softap_sock = pair_sock;
    s_pair_sess.softap_ip_host = softap_ip;
    s_pair_sess.window_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PAIRING_WINDOW_S * 1000000LL;
    memcpy(s_pair_sess.panel_mac, ap_mac, DOORBELL_MAC_SIZE);

    g_panel_mode = MODE_PAIR_DISCOVER;
    doorbell_led_start(LED_PANEL_PAIRING);
    ESP_LOGI(TAG, "Opened pairing discovery window for apartment %u on '%s' (%d s)",
             (unsigned)apt_no, pair_ssid, CONFIG_DOORBELL_PAIRING_WINDOW_S);
}

/* PAIR_HELLO from @p from_ip: first candidate, repeat of the same module, or collision. @return false if aborted */
static bool on_pair_hello_locked(const doorbell_pair_hello_t *hello, uint32_t from_ip, int64_t now_us)
{
    if (!s_pair_sess.has_candidate) {
        s_pair_sess.has_candidate = true;
        s_pair_sess.candidate_ip_host = from_ip;
        s_pair_sess.hello = *hello;
        s_pair_sess.collision_deadline_us = now_us + PAIR_COLLISION_WINDOW_US;
        ESP_LOGI(TAG, "Received PAIR_HELLO candidate; waiting 1.5 s collision window");
        return true;
    }
    if (memcmp(s_pair_sess.hello.unit_mac, hello->unit_mac, DOORBELL_MAC_SIZE) != 0) {
        ESP_LOGW(TAG, "Collision detected: second module sent PAIR_HELLO — aborting");
        panel_pairing_abort_locked(true);
        return false;
    }
    s_pair_sess.candidate_ip_host = from_ip;
    /* Same module repeated PAIR_HELLO after PAIR_CHALLENGE was sent: the challenge was lost, so retransmit the
     * identical frame (the module keeps its keypair/nonce until timeout) */
    if (s_pair_sess.challenge_sent && s_pair_sess.challenge_len > 0 &&
            memcmp(s_pair_sess.hello.unit_nonce, hello->unit_nonce, DOORBELL_PAIR_NONCE_SIZE) == 0) {
        doorbell_udp_send_direct(s_pair_sess.softap_sock, s_pair_sess.challenge_pkt, s_pair_sess.challenge_len,
                                 from_ip, CONFIG_DOORBELL_UDP_PORT);
    }
    return true;
}

/* Derive k_pair, provision the building Wi-Fi + K_apt and send PAIR_CHALLENGE. @return false if aborted */
static bool send_pair_challenge_locked(void)
{
    uint32_t panel_lan_ip = 0;
    if (!doorbell_net_get_sta_ipv4(&panel_lan_ip) ||
            doorbell_x25519_generate(&s_pair_sess.x25519) != ESP_OK ||
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
        panel_pairing_abort_locked(true);
        return false;
    }

    uint8_t ch = 1;
    doorbell_net_get_sta_telemetry(NULL, &ch, NULL, NULL);

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

    esp_err_t pack_err = doorbell_pack_pair_challenge(s_pair_sess.panel_mac,
                                                      s_pair_sess.panel_nonce,
                                                      s_pair_sess.x25519.pub_key,
                                                      s_pair_sess.apt_no,
                                                      &blob,
                                                      s_pair_sess.k_pair,
                                                      s_pair_sess.challenge_pkt,
                                                      sizeof(s_pair_sess.challenge_pkt),
                                                      &s_pair_sess.challenge_len);
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    if (pack_err != ESP_OK) {
        panel_pairing_abort_locked(true);
        return false;
    }

    doorbell_udp_send_direct(s_pair_sess.softap_sock, s_pair_sess.challenge_pkt, s_pair_sess.challenge_len,
                             s_pair_sess.candidate_ip_host, CONFIG_DOORBELL_UDP_PORT);
    s_pair_sess.challenge_sent = true;
    ESP_LOGI(TAG, "Sent PAIR_CHALLENGE to candidate module for apartment %u", (unsigned)s_pair_sess.apt_no);
    return true;
}

/* Receive one SoftAP datagram (bounded by the socket timeout) and run the discovery state machine on it */
static void receive_on_softap_locked(int64_t now_us)
{
    uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    ssize_t n = recvfrom(s_pair_sess.softap_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr,
                         &from_len);
    if (n <= 0) {
        return;
    }
    uint32_t from_ip = ntohl(from_addr.sin_addr.s_addr);
    /* Verify source IP belongs to SoftAP /24 subnet */
    if ((from_ip & 0xFFFFFF00UL) != (s_pair_sess.softap_ip_host & 0xFFFFFF00UL)) {
        return;
    }

    doorbell_pair_hello_t hello;
    if (doorbell_unpack_pair_hello(rx_buf, (size_t)n, &hello) == ESP_OK) {
        on_pair_hello_locked(&hello, from_ip, now_us);
        return;
    }

    doorbell_pair_confirm_t confirm;
    if (s_pair_sess.challenge_sent &&
            doorbell_unpack_pair_confirm(rx_buf, (size_t)n, s_pair_sess.k_pair, &confirm) == ESP_OK &&
            confirm.apt_no == s_pair_sess.apt_no &&
            memcmp(confirm.unit_mac, s_pair_sess.hello.unit_mac, DOORBELL_MAC_SIZE) == 0 &&
            confirm.confirm_status == DOORBELL_STATUS_OK) {
        /* Phase 1 complete: close SoftAP immediately and wait for PAIR_COMPLETE on the LAN */
        close_pair_softap_locked();
        s_pair_sess.window_deadline_us = esp_timer_get_time() +
                                         (int64_t)CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000000LL;
        g_panel_mode = MODE_PAIR_COMPLETE;
        ESP_LOGI(TAG, "Received PAIR_CONFIRM for apt %u; waiting up to %d s for LAN PAIR_COMPLETE",
                 (unsigned)s_pair_sess.apt_no, CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S);
    }
}

void panel_pairing_step(void)
{
    panel_lock();
    int64_t now_us = esp_timer_get_time();

    if (g_panel_mode == MODE_PAIR_COMPLETE) {
        if (now_us >= s_pair_sess.window_deadline_us) {
            ESP_LOGW(TAG, "Timed out waiting for PAIR_COMPLETE on LAN for apartment %u — rolling back",
                     (unsigned)s_pair_sess.apt_no);
            panel_pairing_abort_locked(true);
        }
    } else if (g_panel_mode == MODE_PAIR_DISCOVER) {
        if (now_us >= s_pair_sess.window_deadline_us) {
            ESP_LOGW(TAG, "Pairing discovery window expired for apartment %u", (unsigned)s_pair_sess.apt_no);
            panel_pairing_abort_locked(true);
        } else if (doorbell_net_get_softap_sta_count() > 1) {
            /* Multi-station collision check on SoftAP */
            ESP_LOGW(TAG, "Multiple stations associated to pairing SoftAP — aborting pairing");
            panel_pairing_abort_locked(true);
        } else {
            receive_on_softap_locked(now_us);
            /* Send PAIR_CHALLENGE after the 1.5 s collision window passes with a single module */
            if (g_panel_mode == MODE_PAIR_DISCOVER && s_pair_sess.has_candidate && !s_pair_sess.challenge_sent &&
                    now_us >= s_pair_sess.collision_deadline_us) {
                send_pair_challenge_locked();
            }
        }
    }

    panel_unlock();
}

bool panel_pairing_handle_complete_locked(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host,
                                          const doorbell_hdr_t *clear_hdr)
{
    PANEL_ASSERT_LOCKED();
    if (clear_hdr->msg_type != DOORBELL_MSG_PAIR_COMPLETE || g_panel_mode != MODE_PAIR_COMPLETE ||
            s_pair_sess.apt_no != clear_hdr->src_id) {
        return false;
    }
    uint8_t apt_no = s_pair_sess.apt_no;

    doorbell_hdr_t auth_hdr;
    uint8_t plain[DOORBELL_PL_MAX_SIZE];
    size_t plain_len = 0;
    doorbell_pl_pair_complete_t pc;
    if (doorbell_frame_decrypt(buf, len, s_pair_sess.k_apt, &auth_hdr, plain, sizeof(plain), &plain_len) != ESP_OK ||
            doorbell_unpack_pair_complete(plain, plain_len, &pc) != ESP_OK ||
            memcmp(pc.unit_mac, s_pair_sess.hello.unit_mac, DOORBELL_MAC_SIZE) != 0) {
        return true;
    }

    /* Build the record in a scratch copy and publish it to g_apt_slots only after nvs_commit() succeeds, so a failed
     * write never leaves a RAM-only registration behind */
    doorbell_panel_apt_blob_t new_rec = {0};
    new_rec.apt_no = apt_no;
    new_rec.registered = 1;
    memcpy(new_rec.unit_mac, pc.unit_mac, DOORBELL_MAC_SIZE);
    memcpy(new_rec.k_apt, s_pair_sess.k_apt, DOORBELL_KEY_SIZE);
    new_rec.last_ipv4 = (pc.sender_ipv4 == from_ip_host) ? from_ip_host : 0;
    new_rec.tx_counter_ceiling = CONFIG_DOORBELL_NVS_COUNTER_BLOCK;
    new_rec.rx_floor = auth_hdr.tx_counter;

    esp_err_t commit_err = doorbell_storage_panel_save_apt(&new_rec);
    if (commit_err != ESP_OK) {
        mbedtls_platform_zeroize(&new_rec, sizeof(new_rec));
        ESP_LOGE(TAG, "NVS commit of apartment %u failed (%s) — pairing rolled back",
                 (unsigned)apt_no, esp_err_to_name(commit_err));
        panel_pairing_abort_locked(true);
        return true;
    }

    panel_apt_slot_t *slot = &g_apt_slots[apt_no];
    memset(slot, 0, sizeof(*slot));
    slot->nvs = new_rec;
    mbedtls_platform_zeroize(&new_rec, sizeof(new_rec));
    slot->persisted_rx_floor = auth_hdr.tx_counter;

    /* Erase any older apartment slot sharing the same unit_mac ONLY after the new commit succeeded */
    for (unsigned old_apt = CONFIG_DOORBELL_PANEL_MIN_APT_NO; old_apt <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; old_apt++) {
        if (old_apt != apt_no && g_apt_slots[old_apt].nvs.registered &&
                memcmp(g_apt_slots[old_apt].nvs.unit_mac, pc.unit_mac, DOORBELL_MAC_SIZE) == 0) {
            doorbell_storage_panel_erase_apt((uint8_t)old_apt);
            memset(&g_apt_slots[old_apt], 0, sizeof(g_apt_slots[old_apt]));
            ESP_LOGI(TAG, "Removed previous registration of same unit_mac from apartment %u", old_apt);
        }
    }

    panel_note_module_rx_locked(slot, esp_timer_get_time());

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
        uint8_t ack_plain[DOORBELL_PL_PAIR_COMPLETE_ACK_SIZE];
        size_t ack_plain_len = doorbell_pack_pair_complete_ack(&ack_pl, ack_plain);
        panel_send_ack_locked(lan_sock, slot, DOORBELL_MSG_PAIR_COMPLETE_ACK, &auth_hdr, buf, len, ack_cnt,
                              ack_plain, ack_plain_len, from_ip_host);
    }

    panel_pairing_abort_locked(false);
    doorbell_led_start(LED_PANEL_RESULT_OK);
    ESP_LOGI(TAG, "Completed two-phase pairing for apartment %u (fw v%u.%u)",
             (unsigned)apt_no, (unsigned)pc.fw_version_major, (unsigned)pc.fw_version_minor);
    return true;
}
