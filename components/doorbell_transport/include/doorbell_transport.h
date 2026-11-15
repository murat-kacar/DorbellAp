#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "doorbell_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Random jitter added to the ACK timeout of every retry attempt (docs/protocol.md §4.1) */
#define DOORBELL_TXN_RETRY_JITTER_MS 25

/**
 * @brief Send an encrypted protocol frame: unicast on attempt 1 (limited broadcast if the peer IP is unknown) and
 *        dual-path unicast + 255.255.255.255 on retries (I-29, I-33). Rewrites only the unauthenticated transport
 *        flags of @p frame.
 */
esp_err_t doorbell_udp_send_frame(int sock,
                                  uint8_t *frame,
                                  size_t frame_len,
                                  uint32_t peer_ipv4_host,
                                  uint16_t port,
                                  bool is_retry);

/** What a transaction's receive callback decided about one received frame */
typedef enum {
    DOORBELL_TXN_CONTINUE = 0, /**< not the awaited ACK (or handled elsewhere): keep waiting */
    DOORBELL_TXN_DONE,         /**< the awaited ACK was authenticated and matched: transaction complete */
    DOORBELL_TXN_ABORT,        /**< stop now without success */
} doorbell_txn_verdict_t;

/**
 * @brief Called for every received frame whose header parses while a transaction waits for its ACK.
 * @param hdr cleartext (not yet authenticated) header of @p frame
 */
typedef doorbell_txn_verdict_t (*doorbell_txn_rx_cb_t)(const uint8_t *frame, size_t len, const doorbell_hdr_t *hdr,
                                                       uint32_t from_ipv4_host, void *ctx);

/** One confirmable request: the encrypted frame and how to wait for its ACK */
typedef struct {
    int sock;
    uint8_t *frame;                 /**< encrypted request; its transport flags are rewritten per attempt */
    size_t frame_len;
    uint32_t peer_ipv4_host;        /**< last known peer address, 0 = unknown (broadcast) */
    uint16_t port;
    uint32_t retries;               /**< attempts after the first one */
    uint32_t ack_timeout_ms;        /**< per attempt; retries add +/- DOORBELL_TXN_RETRY_JITTER_MS */
    doorbell_txn_rx_cb_t on_rx;     /**< required */
    /** Called before every send and on every receive slice (at most the socket's SO_RCVTIMEO apart): feed the task
     *  watchdog and run periodic safety checks here (I-46, I-22). Optional. */
    void (*on_poll)(void *ctx);
    void *ctx;
} doorbell_txn_t;

/**
 * @brief Send @p txn's frame and wait for its ACK, retransmitting the byte-identical frame on timeout.
 *
 * Blocks for up to (retries + 1) ACK timeouts; the caller feeds its watchdog in on_poll, so the socket's SO_RCVTIMEO
 * must be well below the TWDT timeout (I-46).
 *
 * @param[out] out_rtt_ms optional: milliseconds from the last send to the accepted ACK
 * @return ESP_OK when on_rx returned DONE, ESP_ERR_INVALID_STATE when it returned ABORT, ESP_ERR_TIMEOUT when every
 *         attempt timed out, ESP_ERR_INVALID_ARG for a malformed @p txn.
 */
esp_err_t doorbell_txn_run(const doorbell_txn_t *txn, uint32_t *out_rtt_ms);

#ifdef __cplusplus
}
#endif
