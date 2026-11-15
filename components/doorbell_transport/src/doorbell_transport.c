#include "doorbell_transport.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "doorbell_net.h"

#define LIMITED_BROADCAST_IPV4 0xFFFFFFFFUL

static bool peer_ip_known(uint32_t ipv4_host)
{
    return ipv4_host != 0 && ipv4_host != LIMITED_BROADCAST_IPV4;
}

esp_err_t doorbell_udp_send_frame(int sock,
                                  uint8_t *frame,
                                  size_t frame_len,
                                  uint32_t peer_ipv4_host,
                                  uint16_t port,
                                  bool is_retry)
{
    if (sock < 0 || !frame || frame_len < DOORBELL_MIN_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Attempt 1 (Initial): unicast if peer IP known, else limited broadcast 255.255.255.255 */
    if (!is_retry) {
        bool unicast = peer_ip_known(peer_ipv4_host);
        doorbell_frame_set_transport_flags(frame, false, !unicast);
        return doorbell_udp_send_direct(sock, frame, frame_len, unicast ? peer_ipv4_host : LIMITED_BROADCAST_IPV4,
                                        port);
    }

    /* Attempt 2+ (Retry): Dual-path send to BOTH last known unicast IP AND limited broadcast 255.255.255.255 */
    esp_err_t err1 = ESP_FAIL;
    if (peer_ip_known(peer_ipv4_host)) {
        doorbell_frame_set_transport_flags(frame, true, false);
        err1 = doorbell_udp_send_direct(sock, frame, frame_len, peer_ipv4_host, port);
    }
    doorbell_frame_set_transport_flags(frame, true, true);
    esp_err_t err2 = doorbell_udp_send_direct(sock, frame, frame_len, LIMITED_BROADCAST_IPV4, port);
    return (err1 == ESP_OK || err2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t doorbell_txn_run(const doorbell_txn_t *txn, uint32_t *out_rtt_ms)
{
    if (!txn || txn->sock < 0 || !txn->frame || txn->frame_len < DOORBELL_MIN_FRAME_SIZE || !txn->on_rx) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t attempt = 0; attempt <= txn->retries; attempt++) {
        if (txn->on_poll) {
            txn->on_poll(txn->ctx);
        }
        int64_t t_send_us = esp_timer_get_time();
        doorbell_udp_send_frame(txn->sock, txn->frame, txn->frame_len, txn->peer_ipv4_host, txn->port, attempt > 0);

        int32_t timeout_ms = (int32_t)txn->ack_timeout_ms;
        if (attempt > 0) {
            timeout_ms += (int32_t)(esp_random() % (2U * DOORBELL_TXN_RETRY_JITTER_MS + 1U)) -
                          DOORBELL_TXN_RETRY_JITTER_MS;
        }
        int64_t deadline_us = t_send_us + (int64_t)timeout_ms * 1000LL;

        while (esp_timer_get_time() < deadline_us) {
            if (txn->on_poll) {
                txn->on_poll(txn->ctx);
            }

            uint8_t rx_buf[DOORBELL_MAX_FRAME_SIZE];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(txn->sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
            if (n < (ssize_t)DOORBELL_MIN_FRAME_SIZE) {
                continue;
            }
            doorbell_hdr_t hdr;
            if (doorbell_hdr_parse(rx_buf, (size_t)n, &hdr) != ESP_OK) {
                continue;
            }

            doorbell_txn_verdict_t verdict = txn->on_rx(rx_buf, (size_t)n, &hdr, ntohl(from_addr.sin_addr.s_addr),
                                                        txn->ctx);
            if (verdict == DOORBELL_TXN_DONE) {
                if (out_rtt_ms) {
                    *out_rtt_ms = (uint32_t)((esp_timer_get_time() - t_send_us) / 1000LL);
                }
                return ESP_OK;
            }
            if (verdict == DOORBELL_TXN_ABORT) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}
