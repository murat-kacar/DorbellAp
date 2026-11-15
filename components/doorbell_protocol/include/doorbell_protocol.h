#pragma once

#include <stdbool.h> /* also compiled by the host compiler for the linux test target */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOORBELL_PROTO_MAGIC        0xDBU
#define DOORBELL_PROTO_VERSION      0x01U

#define DOORBELL_HDR_SIZE           12U
#define DOORBELL_TAG_SIZE           16U
#define DOORBELL_NONCE_SIZE         13U
#define DOORBELL_KEY_SIZE           16U
#define DOORBELL_MAC_SIZE           6U
#define DOORBELL_X25519_PUB_SIZE    32U
#define DOORBELL_PAIR_NONCE_SIZE    16U
#define DOORBELL_MIN_FRAME_SIZE     (DOORBELL_HDR_SIZE + DOORBELL_TAG_SIZE)
#define DOORBELL_MAX_FRAME_SIZE     256U

#define DOORBELL_ID_PANEL           0U

/* SSID prefix of the panel's open pairing SoftAP, followed by the last two SoftAP MAC bytes in hex (docs/protocol.md
 * §3.2) */
#define DOORBELL_PAIR_SSID_PREFIX   "Doorbell-Pair-"

/* Serialized payload sizes (docs/protocol.md §4) */
#define DOORBELL_PL_RING_REQ_SIZE           8U
#define DOORBELL_PL_RING_ACK_SIZE           11U
#define DOORBELL_PL_OPEN_DOOR_REQ_SIZE      16U
#define DOORBELL_PL_OPEN_DOOR_ACK_SIZE      23U
#define DOORBELL_PL_HEARTBEAT_REQ_SIZE      37U
#define DOORBELL_PL_HEARTBEAT_ACK_SIZE      15U
#define DOORBELL_PL_PAIR_COMPLETE_SIZE      12U
#define DOORBELL_PL_PAIR_COMPLETE_ACK_SIZE  DOORBELL_PL_HEARTBEAT_ACK_SIZE
/* Largest operational payload (sizes plaintext buffers) */
#define DOORBELL_PL_MAX_SIZE                DOORBELL_PL_HEARTBEAT_REQ_SIZE

/* Pairing SoftAP frames (docs/protocol.md §3.2) */
#define DOORBELL_PAIR_HELLO_PAYLOAD_SIZE     56U
#define DOORBELL_PAIR_HELLO_FRAME_SIZE       (DOORBELL_HDR_SIZE + DOORBELL_PAIR_HELLO_PAYLOAD_SIZE)
/* Header + cleartext prefix (panel_mac, panel_nonce, panel_x25519_pub, apt_no): the AAD of the provisioning blob */
#define DOORBELL_PAIR_CHALLENGE_PREFIX_SIZE  (DOORBELL_HDR_SIZE + 55U)
#define DOORBELL_PAIR_CONFIRM_PAYLOAD_SIZE   8U

#define DOORBELL_FLAG_DIR_UNIT_TO_PANEL (1U << 0)
#define DOORBELL_FLAG_IS_RETRY          (1U << 1)
#define DOORBELL_FLAG_IS_BCAST          (1U << 2)
#define DOORBELL_FLAG_AAD_MASK          0x01U /* Only bit 0 (direction) is included in AAD */

#define DOORBELL_COUNTER_WARN_THRESHOLD 0xFFFFFF00UL
#define DOORBELL_COUNTER_MAX_VALID      0xFFFFFFFEUL

typedef enum {
    DOORBELL_MSG_RING_REQ          = 0x01,
    DOORBELL_MSG_RING_ACK          = 0x02,
    DOORBELL_MSG_OPEN_DOOR_REQ     = 0x03,
    DOORBELL_MSG_OPEN_DOOR_ACK     = 0x04,
    DOORBELL_MSG_HEARTBEAT_REQ     = 0x05,
    DOORBELL_MSG_HEARTBEAT_ACK     = 0x06,
    DOORBELL_MSG_PAIR_COMPLETE     = 0x07,
    DOORBELL_MSG_PAIR_COMPLETE_ACK = 0x08,
    DOORBELL_MSG_PAIR_HELLO        = 0x10,
    DOORBELL_MSG_PAIR_CHALLENGE    = 0x11,
    DOORBELL_MSG_PAIR_CONFIRM      = 0x12,
} doorbell_msg_type_t;

typedef enum {
    DOORBELL_STATUS_OK             = 0x00,
    DOORBELL_STATUS_ALREADY_ACTIVE = 0x01,
    DOORBELL_STATUS_ERROR          = 0x02,
    DOORBELL_STATUS_CHALLENGE      = 0x03,
} doorbell_ack_status_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t msg_type;
    uint8_t flags;
    uint8_t src_id;
    uint8_t dst_id;
    uint16_t seq_id;
    uint32_t tx_counter;
} doorbell_hdr_t;

/* Operational Payload Structures (host-order fields, serialized as big-endian on wire) */
typedef struct {
    uint32_t sender_ipv4;
    uint32_t panel_boot_id;
} doorbell_pl_ring_req_t;

typedef struct {
    uint32_t req_tx_counter;
    uint16_t req_seq_id;
    uint8_t status;
    uint32_t sender_ipv4;
} doorbell_pl_ring_ack_t;

typedef struct {
    uint32_t sender_ipv4;
    uint32_t panel_boot_id;
    uint64_t echo_nonce;
} doorbell_pl_open_door_req_t;

typedef struct {
    uint32_t req_tx_counter;
    uint16_t req_seq_id;
    uint8_t status;
    uint32_t sender_ipv4;
    uint32_t panel_boot_id;
    uint64_t challenge_nonce;
} doorbell_pl_open_door_ack_t;

typedef struct {
    uint32_t sender_ipv4;
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint16_t ring_count;
    uint16_t unacked_open_count;
    uint16_t rejected_count;
    uint16_t disconnect_count;
    uint8_t reset_reason;
    int8_t ap_rssi;
    uint8_t ap_channel;
    uint8_t ap_bssid[6];
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
    uint16_t heartbeat_interval_s;
} doorbell_pl_heartbeat_req_t;

typedef struct {
    uint32_t req_tx_counter;
    uint16_t req_seq_id;
    uint8_t status;
    uint32_t sender_ipv4;
    uint32_t panel_boot_id;
} doorbell_pl_heartbeat_ack_t;

typedef struct {
    uint32_t sender_ipv4;
    uint8_t unit_mac[DOORBELL_MAC_SIZE];
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
} doorbell_pl_pair_complete_t;

typedef doorbell_pl_heartbeat_ack_t doorbell_pl_pair_complete_ack_t;

/* Pairing SoftAP Messages (§3.2) */
typedef struct {
    uint8_t unit_mac[DOORBELL_MAC_SIZE];
    uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE];
    uint8_t unit_x25519_pub[DOORBELL_X25519_PUB_SIZE];
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
} doorbell_pair_hello_t;

#define DOORBELL_PROVISION_BLOB_PLAIN_SIZE (33U + 65U + 1U + 4U + 16U + 2U) /* 121 bytes */
#define DOORBELL_PROVISION_BLOB_ENC_SIZE   (DOORBELL_PROVISION_BLOB_PLAIN_SIZE + DOORBELL_TAG_SIZE)
#define DOORBELL_PAIR_CHALLENGE_FRAME_SIZE (DOORBELL_PAIR_CHALLENGE_PREFIX_SIZE + DOORBELL_PROVISION_BLOB_ENC_SIZE)

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t ap_channel;
    uint32_t panel_lan_ipv4;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    uint16_t heartbeat_interval_s;
} doorbell_provision_blob_t;

typedef struct {
    uint8_t panel_mac[DOORBELL_MAC_SIZE];
    uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE];
    uint8_t panel_x25519_pub[DOORBELL_X25519_PUB_SIZE];
    uint8_t apt_no;
} doorbell_pair_challenge_t;

typedef struct {
    uint8_t unit_mac[DOORBELL_MAC_SIZE];
    uint8_t apt_no;
    uint8_t confirm_status;
} doorbell_pair_confirm_t;

/* Deduplication cache entry per peer */
typedef struct {
    bool valid;
    uint32_t tx_counter;
    uint16_t seq_id;
    /* CCM tag of the request that produced this ACK. A duplicate must carry the same tag, so a forged cleartext
     * header with a guessed (tx_counter, seq_id) cannot make the receiver reflect the cached ACK. */
    uint8_t req_tag[DOORBELL_TAG_SIZE];
    size_t frame_len;
    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
} doorbell_dedup_cache_t;

/* X25519 ephemeral key context */
typedef struct {
    psa_key_id_t key_id;
    bool initialized;
    uint8_t pub_key[DOORBELL_X25519_PUB_SIZE];
} doorbell_x25519_ctx_t;

/* ESP-IDF v6.1 calls psa_crypto_init() automatically at startup on ESP32 targets;
 * doorbell_crypto_init() is used by the `linux` host unit tests. */
esp_err_t doorbell_crypto_init(void);
esp_err_t doorbell_crypto_random(uint8_t *buf, size_t len);

/* X25519 & HKDF-SHA256 Pairing Key Agreement */
esp_err_t doorbell_x25519_generate(doorbell_x25519_ctx_t *ctx);
void doorbell_x25519_free(doorbell_x25519_ctx_t *ctx);
esp_err_t doorbell_x25519_derive_kpair(const doorbell_x25519_ctx_t *ctx,
                                       const uint8_t peer_pub[DOORBELL_X25519_PUB_SIZE],
                                       const uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t panel_mac[DOORBELL_MAC_SIZE],
                                       const uint8_t unit_mac[DOORBELL_MAC_SIZE],
                                       uint8_t apt_no,
                                       uint8_t out_k_pair[DOORBELL_KEY_SIZE]);

/* Header Serialization & Parsing */

/** Header of a protocol frame with the current magic and version */
static inline doorbell_hdr_t doorbell_hdr_make(uint8_t msg_type, uint8_t flags, uint8_t src_id, uint8_t dst_id,
                                               uint16_t seq_id, uint32_t tx_counter)
{
    doorbell_hdr_t hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = msg_type,
        .flags = flags,
        .src_id = src_id,
        .dst_id = dst_id,
        .seq_id = seq_id,
        .tx_counter = tx_counter,
    };
    return hdr;
}

void doorbell_hdr_serialize(const doorbell_hdr_t *hdr, uint8_t out[DOORBELL_HDR_SIZE]);
esp_err_t doorbell_hdr_parse(const uint8_t *buf, size_t len, doorbell_hdr_t *out);

/* Operational Frame AEAD Encrypt & Decrypt */
esp_err_t doorbell_frame_encrypt(const doorbell_hdr_t *hdr,
                                 const uint8_t key[DOORBELL_KEY_SIZE],
                                 const uint8_t *plaintext,
                                 size_t plaintext_len,
                                 uint8_t *out_frame,
                                 size_t out_capacity,
                                 size_t *out_frame_len);

esp_err_t doorbell_frame_decrypt(const uint8_t *frame,
                                 size_t frame_len,
                                 const uint8_t key[DOORBELL_KEY_SIZE],
                                 doorbell_hdr_t *out_hdr,
                                 uint8_t *out_plaintext,
                                 size_t out_capacity,
                                 size_t *out_plaintext_len);

/**
 * Accepted authenticated frame: advance @p rx_floor to @p rx_counter and adopt @p from_ipv4 as the peer
 * address if the authenticated @p sender_ipv4 matches it.
 * @pre The caller has already rejected replays (@p rx_counter > *@p rx_floor); this function does not check it and
 *      would move the floor backwards otherwise.
 * @return true when the record must be persisted now: the peer address changed, or the floor has advanced a whole
 *         write-ahead block since @p persisted_rx_floor
 */
bool doorbell_rx_accept(uint32_t *rx_floor, uint32_t *peer_ipv4, uint32_t persisted_rx_floor, uint32_t rx_counter,
                        uint32_t sender_ipv4, uint32_t from_ipv4, uint32_t block_size);

/**
 * A retransmission of the request whose ACK is cached: same tx_counter and seq_id and the same CCM tag, so a
 * forged cleartext header with a guessed (tx_counter, seq_id) never makes the receiver reflect the cached ACK.
 */
bool doorbell_dedup_match(const doorbell_dedup_cache_t *cache, const doorbell_hdr_t *req_hdr, const uint8_t *req_frame,
                          size_t req_len);

/**
 * Encrypt the ACK (@p ack_hdr, @p ack_plain) for the request @p req_hdr / @p req_frame into @p cache; the caller then
 * sends cache->frame. On failure the cache is invalidated.
 */
esp_err_t doorbell_dedup_store(doorbell_dedup_cache_t *cache, const doorbell_hdr_t *ack_hdr,
                               const uint8_t key[DOORBELL_KEY_SIZE], const uint8_t *ack_plain, size_t ack_plain_len,
                               const doorbell_hdr_t *req_hdr, const uint8_t *req_frame, size_t req_len);

/* Update transport flags (is_retry, is_bcast) in an already-encrypted frame without re-encrypting */
void doorbell_frame_set_transport_flags(uint8_t *frame, bool is_retry, bool is_bcast);

/* Payload Serialization & Deserialization */
size_t doorbell_pack_ring_req(const doorbell_pl_ring_req_t *pl, uint8_t out[DOORBELL_PL_RING_REQ_SIZE]);
esp_err_t doorbell_unpack_ring_req(const uint8_t *buf, size_t len, doorbell_pl_ring_req_t *pl);

size_t doorbell_pack_ring_ack(const doorbell_pl_ring_ack_t *pl, uint8_t out[DOORBELL_PL_RING_ACK_SIZE]);
esp_err_t doorbell_unpack_ring_ack(const uint8_t *buf, size_t len, doorbell_pl_ring_ack_t *pl);

size_t doorbell_pack_open_door_req(const doorbell_pl_open_door_req_t *pl, uint8_t out[DOORBELL_PL_OPEN_DOOR_REQ_SIZE]);
esp_err_t doorbell_unpack_open_door_req(const uint8_t *buf, size_t len, doorbell_pl_open_door_req_t *pl);

size_t doorbell_pack_open_door_ack(const doorbell_pl_open_door_ack_t *pl, uint8_t out[DOORBELL_PL_OPEN_DOOR_ACK_SIZE]);
esp_err_t doorbell_unpack_open_door_ack(const uint8_t *buf, size_t len, doorbell_pl_open_door_ack_t *pl);

size_t doorbell_pack_heartbeat_req(const doorbell_pl_heartbeat_req_t *pl, uint8_t out[DOORBELL_PL_HEARTBEAT_REQ_SIZE]);
esp_err_t doorbell_unpack_heartbeat_req(const uint8_t *buf, size_t len, doorbell_pl_heartbeat_req_t *pl);

size_t doorbell_pack_heartbeat_ack(const doorbell_pl_heartbeat_ack_t *pl, uint8_t out[DOORBELL_PL_HEARTBEAT_ACK_SIZE]);
esp_err_t doorbell_unpack_heartbeat_ack(const uint8_t *buf, size_t len, doorbell_pl_heartbeat_ack_t *pl);

size_t doorbell_pack_pair_complete(const doorbell_pl_pair_complete_t *pl, uint8_t out[DOORBELL_PL_PAIR_COMPLETE_SIZE]);
esp_err_t doorbell_unpack_pair_complete(const uint8_t *buf, size_t len, doorbell_pl_pair_complete_t *pl);

/* PAIR_COMPLETE_ACK has the HEARTBEAT_ACK payload layout (docs/protocol.md §4) */
static inline size_t doorbell_pack_pair_complete_ack(const doorbell_pl_pair_complete_ack_t *pl,
                                                     uint8_t out[DOORBELL_PL_PAIR_COMPLETE_ACK_SIZE])
{
    return doorbell_pack_heartbeat_ack(pl, out);
}

static inline esp_err_t doorbell_unpack_pair_complete_ack(const uint8_t *buf, size_t len,
                                                          doorbell_pl_pair_complete_ack_t *pl)
{
    return doorbell_unpack_heartbeat_ack(buf, len, pl);
}

/**
 * Every *_ACK payload starts with the request it answers: req_tx_counter (4 B), req_seq_id (2 B), status (1 B).
 * Reads that common prefix so a transaction can match an ACK before decoding the type-specific rest.
 */
esp_err_t doorbell_unpack_ack_ref(const uint8_t *buf, size_t len, uint32_t *out_req_tx_counter,
                                  uint16_t *out_req_seq_id, uint8_t *out_status);

/* Pairing SoftAP Message Serialization & Encryption */
size_t doorbell_pack_pair_hello(const doorbell_pair_hello_t *msg, uint8_t *out, size_t cap);
esp_err_t doorbell_unpack_pair_hello(const uint8_t *buf, size_t len, doorbell_pair_hello_t *msg);

esp_err_t doorbell_pack_pair_challenge(const uint8_t panel_mac[DOORBELL_MAC_SIZE],
                                       const uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t panel_pub[DOORBELL_X25519_PUB_SIZE],
                                       uint8_t apt_no,
                                       const doorbell_provision_blob_t *blob,
                                       const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                       uint8_t *out,
                                       size_t cap,
                                       size_t *out_len);

esp_err_t doorbell_unpack_pair_challenge_header(const uint8_t *buf,
                                                size_t len,
                                                doorbell_pair_challenge_t *out_ch);

esp_err_t doorbell_decrypt_provision_blob(const uint8_t *frame,
                                          size_t frame_len,
                                          const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                          doorbell_provision_blob_t *out_blob);

esp_err_t doorbell_pack_pair_confirm(const doorbell_pair_confirm_t *confirm,
                                     const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                     uint8_t *out,
                                     size_t cap,
                                     size_t *out_len);

esp_err_t doorbell_unpack_pair_confirm(const uint8_t *buf,
                                       size_t len,
                                       const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                       doorbell_pair_confirm_t *out_confirm);

#ifdef __cplusplus
}
#endif
