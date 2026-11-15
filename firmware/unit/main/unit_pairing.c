/* Two-phase pairing client of the apartment module (I-26, I-27, I-30, I-45): join the panel's open pairing SoftAP,
 * PAIR_HELLO / PAIR_CHALLENGE / PAIR_CONFIRM there, then join the building AP and confirm with PAIR_COMPLETE. */
#include "unit_state.h"
#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "unit_config.h"
#include "doorbell_net.h"
#include "doorbell_transport.h"

static const char *TAG = "unit_pairing";

#define PAIR_JOIN_TIMEOUT_MS  10000     /* scan + association + DHCP on the pairing SoftAP */
#define PAIR_SOFTAP_PHASE_US  6000000LL /* PAIR_HELLO / PAIR_CHALLENGE exchange on the SoftAP */
#define PAIR_HELLO_REPEAT_US  500000LL
#define PAIR_COMPLETE_RETRIES 3U        /* 4 PAIR_COMPLETE attempts (doorbell_config_checks.h budget) */

/* unit_worker_task only */
static doorbell_x25519_ctx_t s_keypair = {0};

void unit_pairing_pregenerate_key(void)
{
    doorbell_x25519_generate(&s_keypair);
}

/* Phase 1 on the open SoftAP: PAIR_HELLO until a PAIR_CHALLENGE decrypts, then PAIR_CONFIRM */
static bool pair_on_softap(int udp_sock, const uint8_t unit_mac[DOORBELL_MAC_SIZE],
                           const uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE], doorbell_pair_challenge_t *challenge,
                           doorbell_provision_blob_t *prov)
{
    uint32_t my_softap_ip = 0;
    if (!doorbell_net_get_sta_ipv4(&my_softap_ip)) {
        return false;
    }
    uint32_t panel_softap_ip = (my_softap_ip & 0xFFFFFF00UL) | 1UL;

    doorbell_pair_hello_t hello = {
        .fw_version_major = DOORBELL_FW_VERSION_MAJOR,
        .fw_version_minor = DOORBELL_FW_VERSION_MINOR,
    };
    memcpy(hello.unit_mac, unit_mac, DOORBELL_MAC_SIZE);
    memcpy(hello.unit_nonce, unit_nonce, DOORBELL_PAIR_NONCE_SIZE);
    memcpy(hello.unit_x25519_pub, s_keypair.pub_key, DOORBELL_X25519_PUB_SIZE);
    uint8_t hello_pkt[DOORBELL_PAIR_HELLO_FRAME_SIZE];
    size_t hello_len = doorbell_pack_pair_hello(&hello, hello_pkt, sizeof(hello_pkt));

    uint8_t k_pair[DOORBELL_KEY_SIZE] = {0};
    bool got_challenge = false;
    int64_t deadline_us = esp_timer_get_time() + PAIR_SOFTAP_PHASE_US;
    int64_t next_send_us = 0;
    while (!got_challenge && esp_timer_get_time() < deadline_us) {
        unit_poll(NULL);

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_send_us) {
            doorbell_udp_send_direct(udp_sock, hello_pkt, hello_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
            next_send_us = now_us + PAIR_HELLO_REPEAT_US;
        }

        uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
        ssize_t n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0, NULL, NULL);
        got_challenge = n > 0 &&
                        doorbell_unpack_pair_challenge_header(rx_buf, (size_t)n, challenge) == ESP_OK &&
                        doorbell_x25519_derive_kpair(&s_keypair, challenge->panel_x25519_pub,
                                                     challenge->panel_nonce, unit_nonce, challenge->panel_mac,
                                                     unit_mac, challenge->apt_no, k_pair) == ESP_OK &&
                        doorbell_decrypt_provision_blob(rx_buf, (size_t)n, k_pair, prov) == ESP_OK;
    }
    if (!got_challenge) {
        ESP_LOGW(TAG, "Timed out waiting for valid PAIR_CHALLENGE");
        mbedtls_platform_zeroize(k_pair, sizeof(k_pair));
        return false;
    }

    doorbell_pair_confirm_t confirm = {
        .apt_no = challenge->apt_no,
        .confirm_status = DOORBELL_STATUS_OK,
    };
    memcpy(confirm.unit_mac, unit_mac, DOORBELL_MAC_SIZE);
    uint8_t confirm_pkt[DOORBELL_MAX_FRAME_SIZE];
    size_t confirm_len = 0;
    if (doorbell_pack_pair_confirm(&confirm, k_pair, confirm_pkt, sizeof(confirm_pkt), &confirm_len) == ESP_OK) {
        doorbell_udp_send_direct(udp_sock, confirm_pkt, confirm_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
        vTaskDelay(pdMS_TO_TICKS(60));
        doorbell_udp_send_direct(udp_sock, confirm_pkt, confirm_len, panel_softap_ip, CONFIG_DOORBELL_UDP_PORT);
    }
    mbedtls_platform_zeroize(k_pair, sizeof(k_pair));
    return true;
}

/* Phase 2 on the building LAN: join, then authenticated PAIR_COMPLETE until PAIR_COMPLETE_ACK (I-30) */
static bool pair_complete_on_lan(int udp_sock, const uint8_t unit_mac[DOORBELL_MAC_SIZE], unit_pairing_result_t *res)
{
    doorbell_provision_blob_t *prov = &res->prov;
    ESP_LOGI(TAG, "Joining building AP '%s' to complete pairing for apartment %u", prov->wifi_ssid,
             (unsigned)res->apt_no);
    doorbell_net_connect_sta(prov->wifi_ssid, prov->wifi_pass);

    /* The panel starts its +PAIRING_COMPLETE_TIMEOUT window when it receives PAIR_CONFIRM. Reserve time for the
     * PAIR_COMPLETE attempts below (each ACK timeout, retries with jitter) plus a 500 ms safety margin, so a slow DHCP
     * lease cannot push PAIR_COMPLETE past the panel's deadline. */
    uint32_t reserve_ms = (PAIR_COMPLETE_RETRIES + 1U) * (uint32_t)CONFIG_DOORBELL_ACK_TIMEOUT_MS +
                          PAIR_COMPLETE_RETRIES * DOORBELL_TXN_RETRY_JITTER_MS + 500U;
    uint32_t window_ms = (uint32_t)CONFIG_DOORBELL_PAIRING_COMPLETE_TIMEOUT_S * 1000U;
    uint32_t lan_wait_ms = (window_ms > reserve_ms + 1000U) ? (window_ms - reserve_ms) : 1000U;
    int64_t lan_deadline_us = esp_timer_get_time() + (int64_t)lan_wait_ms * 1000LL;
    uint32_t my_lan_ip = 0;
    while (esp_timer_get_time() < lan_deadline_us) {
        unit_poll(NULL);
        if (doorbell_net_wait_for_sta_ip(250) == ESP_OK && doorbell_net_get_sta_ipv4(&my_lan_ip)) {
            break;
        }
    }
    if (my_lan_ip == 0) {
        ESP_LOGW(TAG, "Failed to obtain DHCP lease on building AP within pairing completion window");
        return false;
    }

    /* PAIR_COMPLETE encrypted under K_apt with tx_counter 1: K_apt is new, so counter 1 was never used with it */
    doorbell_pl_pair_complete_t pc_pl = {
        .sender_ipv4 = my_lan_ip,
        .fw_version_major = DOORBELL_FW_VERSION_MAJOR,
        .fw_version_minor = DOORBELL_FW_VERSION_MINOR,
    };
    memcpy(pc_pl.unit_mac, unit_mac, DOORBELL_MAC_SIZE);
    uint8_t pc_plain[DOORBELL_PL_PAIR_COMPLETE_SIZE];
    size_t pc_plain_len = doorbell_pack_pair_complete(&pc_pl, pc_plain);

    uint16_t seq_id = unit_next_seq_id();
    doorbell_hdr_t pc_hdr = doorbell_hdr_make(DOORBELL_MSG_PAIR_COMPLETE, DOORBELL_FLAG_DIR_UNIT_TO_PANEL, res->apt_no,
                                              DOORBELL_ID_PANEL, seq_id, 1);
    uint8_t pc_frame[DOORBELL_MAX_FRAME_SIZE];
    size_t pc_frame_len = 0;
    if (doorbell_frame_encrypt(&pc_hdr, prov->k_apt, pc_plain, pc_plain_len, pc_frame, sizeof(pc_frame),
                               &pc_frame_len) != ESP_OK) {
        return false;
    }

    unit_ack_wait_t w = {
        .sock = udp_sock,
        .apt_no = res->apt_no,
        .ack_type = DOORBELL_MSG_PAIR_COMPLETE_ACK,
        .k_apt = prov->k_apt,
        .req_tx_counter = 1,
        .req_seq_id = seq_id,
    };
    doorbell_pl_pair_complete_ack_t ack_pl;
    if (unit_run_txn(pc_frame, pc_frame_len, prov->panel_lan_ipv4, PAIR_COMPLETE_RETRIES, &w, NULL) != ESP_OK ||
            w.status != DOORBELL_STATUS_OK ||
            doorbell_unpack_pair_complete_ack(w.plain, w.plain_len, &ack_pl) != ESP_OK) {
        ESP_LOGW(TAG, "Did not receive PAIR_COMPLETE_ACK from panel — rolling back (I-30)");
        return false;
    }
    res->panel_boot_id = ack_pl.panel_boot_id;
    if (ack_pl.sender_ipv4 == w.from_ip) {
        prov->panel_lan_ipv4 = w.from_ip;
    }
    res->ack_rx_counter = w.ack_hdr.tx_counter;
    return true;
}

bool unit_pairing_run(int udp_sock, unit_pairing_result_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t unit_mac[DOORBELL_MAC_SIZE] = {0};
    uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE] = {0};
    doorbell_pair_challenge_t challenge = {0};
    char pair_ssid[33] = {0};

    bool ok = (s_keypair.initialized || doorbell_x25519_generate(&s_keypair) == ESP_OK) &&
              doorbell_net_get_sta_mac(unit_mac) == ESP_OK &&
              doorbell_crypto_random(unit_nonce, sizeof(unit_nonce)) == ESP_OK;
    if (ok) {
        esp_err_t err = doorbell_net_scan_and_join_open_ap(DOORBELL_PAIR_SSID_PREFIX, pair_ssid, PAIR_JOIN_TIMEOUT_MS,
                                                           unit_poll, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not join " DOORBELL_PAIR_SSID_PREFIX "* SoftAP: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    ok = ok && pair_on_softap(udp_sock, unit_mac, unit_nonce, &challenge, &out->prov);
    if (ok) {
        out->apt_no = challenge.apt_no;
        memcpy(out->panel_mac, challenge.panel_mac, DOORBELL_MAC_SIZE);
        unit_set_apartment_hostname(out->apt_no);
        ok = pair_complete_on_lan(udp_sock, unit_mac, out);
    }
    doorbell_x25519_free(&s_keypair);
    if (!ok) {
        mbedtls_platform_zeroize(out, sizeof(*out));
    }
    return ok;
}
