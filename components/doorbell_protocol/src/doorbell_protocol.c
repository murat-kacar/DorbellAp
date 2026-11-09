#include "doorbell_protocol.h"
#include <string.h>
#include "esp_log.h"
#include "mbedtls/constant_time.h"

static const char *TAG = "doorbell_proto";

/* Big-endian helper functions */
static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFU);
    p[2] = (uint8_t)((v >> 8) & 0xFFU);
    p[3] = (uint8_t)(v & 0xFFU);
}

static inline uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
}

static inline void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFULL));
}

static inline uint64_t get_be64(const uint8_t *p)
{
    return ((uint64_t)get_be32(p) << 32) | (uint64_t)get_be32(p + 4);
}

esp_err_t doorbell_crypto_init(void)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t doorbell_crypto_random(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    psa_status_t status = psa_generate_random(buf, len);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

esp_err_t doorbell_x25519_generate(doorbell_x25519_ctx_t *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->initialized) {
        psa_destroy_key(ctx->key_id);
        ctx->initialized = false;
    }
    memset(ctx, 0, sizeof(*ctx));

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    psa_status_t status = psa_generate_key(&attr, &ctx->key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key(X25519) failed: %d", (int)status);
        return ESP_FAIL;
    }

    size_t pub_len = 0;
    status = psa_export_public_key(ctx->key_id, ctx->pub_key, sizeof(ctx->pub_key), &pub_len);
    if (status != PSA_SUCCESS || pub_len != DOORBELL_X25519_PUB_SIZE) {
        psa_destroy_key(ctx->key_id);
        ESP_LOGE(TAG, "psa_export_public_key(X25519) failed: %d", (int)status);
        return ESP_FAIL;
    }

    ctx->initialized = true;
    return ESP_OK;
}

void doorbell_x25519_free(doorbell_x25519_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->initialized) {
        psa_destroy_key(ctx->key_id);
    }
    memset(ctx, 0, sizeof(*ctx));
}

esp_err_t doorbell_x25519_derive_kpair(const doorbell_x25519_ctx_t *ctx,
                                       const uint8_t peer_pub[DOORBELL_X25519_PUB_SIZE],
                                       const uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t panel_mac[6],
                                       const uint8_t unit_mac[6],
                                       uint8_t apt_no,
                                       uint8_t out_k_pair[DOORBELL_KEY_SIZE])
{
    if (!ctx || !ctx->initialized || !peer_pub || !panel_nonce || !unit_nonce ||
        !panel_mac || !unit_mac || !out_k_pair) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t shared_secret[DOORBELL_X25519_PUB_SIZE] = {0};
    size_t shared_len = 0;

    psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH,
                                                ctx->key_id,
                                                peer_pub,
                                                DOORBELL_X25519_PUB_SIZE,
                                                shared_secret,
                                                sizeof(shared_secret),
                                                &shared_len);
    if (status != PSA_SUCCESS || shared_len != DOORBELL_X25519_PUB_SIZE) {
        memset(shared_secret, 0, sizeof(shared_secret));
        ESP_LOGE(TAG, "ECDH key agreement failed: %d", (int)status);
        return ESP_FAIL;
    }

    /* Check for all-zero low-order X25519 point (RFC 7748 §6.1) */
    uint8_t zero_check = 0;
    for (size_t i = 0; i < sizeof(shared_secret); i++) {
        zero_check |= shared_secret[i];
    }
    if (zero_check == 0) {
        memset(shared_secret, 0, sizeof(shared_secret));
        ESP_LOGE(TAG, "Rejected low-order X25519 public key");
        return ESP_ERR_INVALID_STATE;
    }

    /* Construct HKDF salt: panel_nonce (16 B) || unit_nonce (16 B) */
    uint8_t salt[32];
    memcpy(salt, panel_nonce, 16);
    memcpy(salt + 16, unit_nonce, 16);

    /* Construct HKDF info: "DorbellAp-Pair-v1" (17 B) || panel_mac (6 B) || unit_mac (6 B) || apt_no (1 B) */
    static const char info_prefix[] = "DorbellAp-Pair-v1";
    uint8_t info[17 + 6 + 6 + 1];
    memcpy(info, info_prefix, 17);
    memcpy(info + 17, panel_mac, 6);
    memcpy(info + 23, unit_mac, 6);
    info[29] = apt_no;

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, sizeof(salt));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET,
                                                shared_secret, sizeof(shared_secret));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, sizeof(info));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, out_k_pair, DOORBELL_KEY_SIZE);
    }

    psa_key_derivation_abort(&op);
    memset(shared_secret, 0, sizeof(shared_secret));

    if (status != PSA_SUCCESS) {
        memset(out_k_pair, 0, DOORBELL_KEY_SIZE);
        ESP_LOGE(TAG, "HKDF-SHA256 derivation failed: %d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void doorbell_hdr_serialize(const doorbell_hdr_t *hdr, uint8_t out[DOORBELL_HDR_SIZE])
{
    out[0] = hdr->magic;
    out[1] = hdr->version;
    out[2] = hdr->msg_type;
    out[3] = hdr->flags;
    out[4] = hdr->src_id;
    out[5] = hdr->dst_id;
    put_be16(&out[6], hdr->seq_id);
    put_be32(&out[8], hdr->tx_counter);
}

esp_err_t doorbell_hdr_parse(const uint8_t *buf, size_t len, doorbell_hdr_t *out)
{
    if (!buf || !out || len < DOORBELL_HDR_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buf[0] != DOORBELL_PROTO_MAGIC || buf[1] != DOORBELL_PROTO_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    out->magic = buf[0];
    out->version = buf[1];
    out->msg_type = buf[2];
    out->flags = buf[3];
    out->src_id = buf[4];
    out->dst_id = buf[5];
    out->seq_id = get_be16(&buf[6]);
    out->tx_counter = get_be32(&buf[8]);
    return ESP_OK;
}

static void build_ccm_nonce_and_aad(const doorbell_hdr_t *hdr,
                                    uint8_t nonce[DOORBELL_NONCE_SIZE],
                                    uint8_t aad[DOORBELL_HDR_SIZE])
{
    doorbell_hdr_t aad_hdr = *hdr;
    /* Mask out transient transport flags (is_retry, is_bcast) in AAD per docs/protocol.md §2.3 step 5 */
    aad_hdr.flags &= DOORBELL_FLAG_AAD_MASK;
    doorbell_hdr_serialize(&aad_hdr, aad);

    /* Nonce layout (13 bytes, docs/protocol.md §2.2):
     * [0] direction (0 or 1), [1] src_id, [2] dst_id, [3..4] seq_id, [5..8] tx_counter, [9..12] 0x00000000 */
    nonce[0] = (hdr->flags & DOORBELL_FLAG_DIR_UNIT_TO_PANEL) ? 0x01U : 0x00U;
    nonce[1] = hdr->src_id;
    nonce[2] = hdr->dst_id;
    put_be16(&nonce[3], hdr->seq_id);
    put_be32(&nonce[5], hdr->tx_counter);
    memset(&nonce[9], 0, 4);
}

static esp_err_t aead_ccm_crypt(bool encrypt,
                                const uint8_t key[DOORBELL_KEY_SIZE],
                                const uint8_t nonce[DOORBELL_NONCE_SIZE],
                                const uint8_t *aad,
                                size_t aad_len,
                                const uint8_t *input,
                                size_t input_len,
                                uint8_t *output,
                                size_t output_cap,
                                size_t *output_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CCM);

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attr, key, DOORBELL_KEY_SIZE, &key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    if (encrypt) {
        status = psa_aead_encrypt(key_id,
                                  PSA_ALG_CCM,
                                  nonce,
                                  DOORBELL_NONCE_SIZE,
                                  aad,
                                  aad_len,
                                  input,
                                  input_len,
                                  output,
                                  output_cap,
                                  output_len);
    } else {
        status = psa_aead_decrypt(key_id,
                                  PSA_ALG_CCM,
                                  nonce,
                                  DOORBELL_NONCE_SIZE,
                                  aad,
                                  aad_len,
                                  input,
                                  input_len,
                                  output,
                                  output_cap,
                                  output_len);
    }

    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_ERR_INVALID_MAC;
}

esp_err_t doorbell_frame_encrypt(const doorbell_hdr_t *hdr,
                                 const uint8_t key[DOORBELL_KEY_SIZE],
                                 const uint8_t *plaintext,
                                 size_t plaintext_len,
                                 uint8_t *out_frame,
                                 size_t out_capacity,
                                 size_t *out_frame_len)
{
    if (!hdr || !key || (!plaintext && plaintext_len > 0) || !out_frame || !out_frame_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr->tx_counter == 0 || hdr->tx_counter > DOORBELL_COUNTER_MAX_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hdr->tx_counter >= DOORBELL_COUNTER_WARN_THRESHOLD) {
        ESP_LOGW(TAG, "Counter exhaustion warning: tx_counter=0x%08lx — re-pairing recommended",
                 (unsigned long)hdr->tx_counter);
    }

    size_t needed = DOORBELL_HDR_SIZE + plaintext_len + DOORBELL_TAG_SIZE;
    if (out_capacity < needed) {
        return ESP_ERR_INVALID_SIZE;
    }

    doorbell_hdr_serialize(hdr, out_frame);

    uint8_t nonce[DOORBELL_NONCE_SIZE];
    uint8_t aad[DOORBELL_HDR_SIZE];
    build_ccm_nonce_and_aad(hdr, nonce, aad);

    size_t ciph_len = 0;
    esp_err_t err = aead_ccm_crypt(true,
                                   key,
                                   nonce,
                                   aad,
                                   sizeof(aad),
                                   plaintext,
                                   plaintext_len,
                                   out_frame + DOORBELL_HDR_SIZE,
                                   out_capacity - DOORBELL_HDR_SIZE,
                                   &ciph_len);
    if (err != ESP_OK) {
        return err;
    }

    *out_frame_len = DOORBELL_HDR_SIZE + ciph_len;
    return ESP_OK;
}

esp_err_t doorbell_frame_decrypt(const uint8_t *frame,
                                 size_t frame_len,
                                 const uint8_t key[DOORBELL_KEY_SIZE],
                                 doorbell_hdr_t *out_hdr,
                                 uint8_t *out_plaintext,
                                 size_t out_capacity,
                                 size_t *out_plaintext_len)
{
    if (!frame || frame_len < DOORBELL_MIN_FRAME_SIZE || !key || !out_hdr || !out_plaintext || !out_plaintext_len) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = doorbell_hdr_parse(frame, frame_len, out_hdr);
    if (err != ESP_OK) {
        return err;
    }
    if (out_hdr->tx_counter == 0 || out_hdr->tx_counter > DOORBELL_COUNTER_MAX_VALID) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t nonce[DOORBELL_NONCE_SIZE];
    uint8_t aad[DOORBELL_HDR_SIZE];
    build_ccm_nonce_and_aad(out_hdr, nonce, aad);

    return aead_ccm_crypt(false,
                          key,
                          nonce,
                          aad,
                          sizeof(aad),
                          frame + DOORBELL_HDR_SIZE,
                          frame_len - DOORBELL_HDR_SIZE,
                          out_plaintext,
                          out_capacity,
                          out_plaintext_len);
}

void doorbell_frame_set_transport_flags(uint8_t *frame, bool is_retry, bool is_bcast)
{
    if (!frame) {
        return;
    }
    uint8_t flags = frame[3] & DOORBELL_FLAG_AAD_MASK;
    if (is_retry) {
        flags |= DOORBELL_FLAG_IS_RETRY;
    }
    if (is_bcast) {
        flags |= DOORBELL_FLAG_IS_BCAST;
    }
    frame[3] = flags;
}

/* Operational Payload Packing & Unpacking */

size_t doorbell_pack_ring_req(const doorbell_pl_ring_req_t *pl, uint8_t out[8])
{
    put_be32(&out[0], pl->sender_ipv4);
    put_be32(&out[4], pl->panel_boot_id);
    return 8;
}

esp_err_t doorbell_unpack_ring_req(const uint8_t *buf, size_t len, doorbell_pl_ring_req_t *pl)
{
    if (!buf || !pl || len < 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->sender_ipv4 = get_be32(&buf[0]);
    pl->panel_boot_id = get_be32(&buf[4]);
    return ESP_OK;
}

size_t doorbell_pack_ring_ack(const doorbell_pl_ring_ack_t *pl, uint8_t out[11])
{
    put_be32(&out[0], pl->req_tx_counter);
    put_be16(&out[4], pl->req_seq_id);
    out[6] = pl->status;
    put_be32(&out[7], pl->sender_ipv4);
    return 11;
}

esp_err_t doorbell_unpack_ring_ack(const uint8_t *buf, size_t len, doorbell_pl_ring_ack_t *pl)
{
    if (!buf || !pl || len < 11) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->req_tx_counter = get_be32(&buf[0]);
    pl->req_seq_id = get_be16(&buf[4]);
    pl->status = buf[6];
    pl->sender_ipv4 = get_be32(&buf[7]);
    return ESP_OK;
}

size_t doorbell_pack_open_door_req(const doorbell_pl_open_door_req_t *pl, uint8_t out[16])
{
    put_be32(&out[0], pl->sender_ipv4);
    put_be32(&out[4], pl->panel_boot_id);
    put_be64(&out[8], pl->echo_nonce);
    return 16;
}

esp_err_t doorbell_unpack_open_door_req(const uint8_t *buf, size_t len, doorbell_pl_open_door_req_t *pl)
{
    if (!buf || !pl || len < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->sender_ipv4 = get_be32(&buf[0]);
    pl->panel_boot_id = get_be32(&buf[4]);
    pl->echo_nonce = get_be64(&buf[8]);
    return ESP_OK;
}

size_t doorbell_pack_open_door_ack(const doorbell_pl_open_door_ack_t *pl, uint8_t out[23])
{
    put_be32(&out[0], pl->req_tx_counter);
    put_be16(&out[4], pl->req_seq_id);
    out[6] = pl->status;
    put_be32(&out[7], pl->sender_ipv4);
    put_be32(&out[11], pl->panel_boot_id);
    put_be64(&out[15], pl->challenge_nonce);
    return 23;
}

esp_err_t doorbell_unpack_open_door_ack(const uint8_t *buf, size_t len, doorbell_pl_open_door_ack_t *pl)
{
    if (!buf || !pl || len < 23) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->req_tx_counter = get_be32(&buf[0]);
    pl->req_seq_id = get_be16(&buf[4]);
    pl->status = buf[6];
    pl->sender_ipv4 = get_be32(&buf[7]);
    pl->panel_boot_id = get_be32(&buf[11]);
    pl->challenge_nonce = get_be64(&buf[15]);
    return ESP_OK;
}

size_t doorbell_pack_heartbeat_req(const doorbell_pl_heartbeat_req_t *pl, uint8_t out[37])
{
    put_be32(&out[0], pl->sender_ipv4);
    put_be32(&out[4], pl->uptime_s);
    put_be32(&out[8], pl->free_heap);
    put_be32(&out[12], pl->min_free_heap);
    put_be16(&out[16], pl->ring_count);
    put_be16(&out[18], pl->unacked_open_count);
    put_be16(&out[20], pl->rejected_count);
    put_be16(&out[22], pl->disconnect_count);
    out[24] = pl->reset_reason;
    out[25] = (uint8_t)pl->ap_rssi;
    out[26] = pl->ap_channel;
    memcpy(&out[27], pl->ap_bssid, 6);
    out[33] = pl->fw_version_major;
    out[34] = pl->fw_version_minor;
    put_be16(&out[35], pl->heartbeat_interval_s);
    return 37;
}
esp_err_t doorbell_unpack_heartbeat_req(const uint8_t *buf, size_t len, doorbell_pl_heartbeat_req_t *pl)
{
    if (!buf || !pl || len < 37) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->sender_ipv4 = get_be32(&buf[0]);
    pl->uptime_s = get_be32(&buf[4]);
    pl->free_heap = get_be32(&buf[8]);
    pl->min_free_heap = get_be32(&buf[12]);
    pl->ring_count = get_be16(&buf[16]);
    pl->unacked_open_count = get_be16(&buf[18]);
    pl->rejected_count = get_be16(&buf[20]);
    pl->disconnect_count = get_be16(&buf[22]);
    pl->reset_reason = buf[24];
    pl->ap_rssi = (int8_t)buf[25];
    pl->ap_channel = buf[26];
    memcpy(pl->ap_bssid, &buf[27], 6);
    pl->fw_version_major = buf[33];
    pl->fw_version_minor = buf[34];
    pl->heartbeat_interval_s = get_be16(&buf[35]);
    return ESP_OK;
}

size_t doorbell_pack_heartbeat_ack(const doorbell_pl_heartbeat_ack_t *pl, uint8_t out[15])
{
    put_be32(&out[0], pl->req_tx_counter);
    put_be16(&out[4], pl->req_seq_id);
    out[6] = pl->status;
    put_be32(&out[7], pl->sender_ipv4);
    put_be32(&out[11], pl->panel_boot_id);
    return 15;
}

esp_err_t doorbell_unpack_heartbeat_ack(const uint8_t *buf, size_t len, doorbell_pl_heartbeat_ack_t *pl)
{
    if (!buf || !pl || len < 15) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->req_tx_counter = get_be32(&buf[0]);
    pl->req_seq_id = get_be16(&buf[4]);
    pl->status = buf[6];
    pl->sender_ipv4 = get_be32(&buf[7]);
    pl->panel_boot_id = get_be32(&buf[11]);
    return ESP_OK;
}

size_t doorbell_pack_pair_complete(const doorbell_pl_pair_complete_t *pl, uint8_t out[12])
{
    put_be32(&out[0], pl->sender_ipv4);
    memcpy(&out[4], pl->unit_mac, 6);
    out[10] = pl->fw_version_major;
    out[11] = pl->fw_version_minor;
    return 12;
}

esp_err_t doorbell_unpack_pair_complete(const uint8_t *buf, size_t len, doorbell_pl_pair_complete_t *pl)
{
    if (!buf || !pl || len < 12) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl->sender_ipv4 = get_be32(&buf[0]);
    memcpy(pl->unit_mac, &buf[4], 6);
    pl->fw_version_major = buf[10];
    pl->fw_version_minor = buf[11];
    return ESP_OK;
}

/* Pairing SoftAP Messages (§3.2) */

/* PAIR_HELLO frame: 12-byte header + 56-byte cleartext payload (total 68 bytes) */
size_t doorbell_pack_pair_hello(const doorbell_pair_hello_t *msg, uint8_t *out, size_t cap)
{
    if (!msg || !out || cap < (DOORBELL_HDR_SIZE + 56)) {
        return 0;
    }
    doorbell_hdr_t hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_PAIR_HELLO,
        .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
        .src_id = 0,
        .dst_id = DOORBELL_ID_PANEL,
        .seq_id = 1,
        .tx_counter = 1,
    };
    doorbell_hdr_serialize(&hdr, out);
    uint8_t *p = out + DOORBELL_HDR_SIZE;
    memcpy(p, msg->unit_mac, 6);
    memcpy(p + 6, msg->unit_nonce, DOORBELL_PAIR_NONCE_SIZE);
    memcpy(p + 22, msg->unit_x25519_pub, DOORBELL_X25519_PUB_SIZE);
    p[54] = msg->fw_version_major;
    p[55] = msg->fw_version_minor;
    return DOORBELL_HDR_SIZE + 56;
}

esp_err_t doorbell_unpack_pair_hello(const uint8_t *buf, size_t len, doorbell_pair_hello_t *msg)
{
    if (!buf || !msg || len < (DOORBELL_HDR_SIZE + 56)) {
        return ESP_ERR_INVALID_SIZE;
    }
    doorbell_hdr_t hdr;
    esp_err_t err = doorbell_hdr_parse(buf, len, &hdr);
    if (err != ESP_OK || hdr.msg_type != DOORBELL_MSG_PAIR_HELLO) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *p = buf + DOORBELL_HDR_SIZE;
    memcpy(msg->unit_mac, p, 6);
    memcpy(msg->unit_nonce, p + 6, DOORBELL_PAIR_NONCE_SIZE);
    memcpy(msg->unit_x25519_pub, p + 22, DOORBELL_X25519_PUB_SIZE);
    msg->fw_version_major = p[54];
    msg->fw_version_minor = p[55];
    return ESP_OK;
}

/* PAIR_CHALLENGE:
 * 12-byte header + 55-byte cleartext prefix (panel_mac[6], panel_nonce[16], panel_x25519_pub[32], apt_no[1])
 * + 137-byte AEAD-encrypted provision blob (121 B plaintext + 16 B tag).
 * Nonce for PAIR_CHALLENGE AEAD: [0]=0x00, [1..12]=panel_nonce[0..11]; AAD = first 67 bytes (header + cleartext prefix). */
esp_err_t doorbell_pack_pair_challenge(const uint8_t panel_mac[6],
                                       const uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE],
                                       const uint8_t panel_pub[DOORBELL_X25519_PUB_SIZE],
                                       uint8_t apt_no,
                                       const doorbell_provision_blob_t *blob,
                                       const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                       uint8_t *out,
                                       size_t cap,
                                       size_t *out_len)
{
    const size_t prefix_len = DOORBELL_HDR_SIZE + 55U;
    const size_t total_len = prefix_len + DOORBELL_PROVISION_BLOB_ENC_SIZE;
    if (!panel_mac || !panel_nonce || !panel_pub || !blob || !k_pair || !out || !out_len || cap < total_len) {
        return ESP_ERR_INVALID_ARG;
    }

    doorbell_hdr_t hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_PAIR_CHALLENGE,
        .flags = 0,
        .src_id = DOORBELL_ID_PANEL,
        .dst_id = apt_no,
        .seq_id = 1,
        .tx_counter = 1,
    };
    doorbell_hdr_serialize(&hdr, out);

    uint8_t *p = out + DOORBELL_HDR_SIZE;
    memcpy(p, panel_mac, 6);
    memcpy(p + 6, panel_nonce, DOORBELL_PAIR_NONCE_SIZE);
    memcpy(p + 22, panel_pub, DOORBELL_X25519_PUB_SIZE);
    p[54] = apt_no;

    uint8_t plain[DOORBELL_PROVISION_BLOB_PLAIN_SIZE] = {0};
    memcpy(&plain[0], blob->wifi_ssid, 33);
    plain[32] = '\0';
    memcpy(&plain[33], blob->wifi_pass, 65);
    plain[33 + 64] = '\0';
    plain[98] = blob->ap_channel;
    put_be32(&plain[99], blob->panel_lan_ipv4);
    memcpy(&plain[103], blob->k_apt, DOORBELL_KEY_SIZE);
    put_be16(&plain[119], blob->heartbeat_interval_s);

    uint8_t nonce[DOORBELL_NONCE_SIZE];
    nonce[0] = 0x00U;
    memcpy(&nonce[1], panel_nonce, 12);

    size_t enc_len = 0;
    esp_err_t err = aead_ccm_crypt(true,
                                   k_pair,
                                   nonce,
                                   out,
                                   prefix_len,
                                   plain,
                                   sizeof(plain),
                                   out + prefix_len,
                                   cap - prefix_len,
                                   &enc_len);
    memset(plain, 0, sizeof(plain));
    if (err != ESP_OK) {
        return err;
    }

    *out_len = prefix_len + enc_len;
    return ESP_OK;
}

esp_err_t doorbell_unpack_pair_challenge_header(const uint8_t *buf,
                                                size_t len,
                                                doorbell_pair_challenge_t *out_ch)
{
    const size_t prefix_len = DOORBELL_HDR_SIZE + 55U;
    const size_t total_len = prefix_len + DOORBELL_PROVISION_BLOB_ENC_SIZE;
    if (!buf || !out_ch || len < total_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    doorbell_hdr_t hdr;
    esp_err_t err = doorbell_hdr_parse(buf, len, &hdr);
    if (err != ESP_OK || hdr.msg_type != DOORBELL_MSG_PAIR_CHALLENGE) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *p = buf + DOORBELL_HDR_SIZE;
    memcpy(out_ch->panel_mac, p, 6);
    memcpy(out_ch->panel_nonce, p + 6, DOORBELL_PAIR_NONCE_SIZE);
    memcpy(out_ch->panel_x25519_pub, p + 22, DOORBELL_X25519_PUB_SIZE);
    out_ch->apt_no = p[54];
    memcpy(out_ch->encrypted_blob, buf + prefix_len, DOORBELL_PROVISION_BLOB_ENC_SIZE);
    return ESP_OK;
}

esp_err_t doorbell_decrypt_provision_blob(const uint8_t *frame,
                                          size_t frame_len,
                                          const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                          doorbell_provision_blob_t *out_blob)
{
    const size_t prefix_len = DOORBELL_HDR_SIZE + 55U;
    const size_t total_len = prefix_len + DOORBELL_PROVISION_BLOB_ENC_SIZE;
    if (!frame || !k_pair || !out_blob || frame_len < total_len) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *panel_nonce = frame + DOORBELL_HDR_SIZE + 6;
    uint8_t nonce[DOORBELL_NONCE_SIZE];
    nonce[0] = 0x00U;
    memcpy(&nonce[1], panel_nonce, 12);

    uint8_t plain[DOORBELL_PROVISION_BLOB_PLAIN_SIZE] = {0};
    size_t plain_len = 0;
    esp_err_t err = aead_ccm_crypt(false,
                                   k_pair,
                                   nonce,
                                   frame,
                                   prefix_len,
                                   frame + prefix_len,
                                   DOORBELL_PROVISION_BLOB_ENC_SIZE,
                                   plain,
                                   sizeof(plain),
                                   &plain_len);
    if (err != ESP_OK || plain_len != DOORBELL_PROVISION_BLOB_PLAIN_SIZE) {
        memset(plain, 0, sizeof(plain));
        return ESP_ERR_INVALID_MAC;
    }

    memset(out_blob, 0, sizeof(*out_blob));
    memcpy(out_blob->wifi_ssid, &plain[0], 32);
    out_blob->wifi_ssid[32] = '\0';
    memcpy(out_blob->wifi_pass, &plain[33], 64);
    out_blob->wifi_pass[64] = '\0';
    out_blob->ap_channel = plain[98];
    out_blob->panel_lan_ipv4 = get_be32(&plain[99]);
    memcpy(out_blob->k_apt, &plain[103], DOORBELL_KEY_SIZE);
    out_blob->heartbeat_interval_s = get_be16(&plain[119]);

    memset(plain, 0, sizeof(plain));
    return ESP_OK;
}

esp_err_t doorbell_pack_pair_confirm(const doorbell_pair_confirm_t *confirm,
                                     const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                     uint8_t *out,
                                     size_t cap,
                                     size_t *out_len)
{
    if (!confirm || !k_pair || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    doorbell_hdr_t hdr = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = DOORBELL_MSG_PAIR_CONFIRM,
        .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
        .src_id = confirm->apt_no,
        .dst_id = DOORBELL_ID_PANEL,
        .seq_id = 2,
        .tx_counter = 1,
    };
    uint8_t plain[8];
    memcpy(&plain[0], confirm->unit_mac, 6);
    plain[6] = confirm->apt_no;
    plain[7] = confirm->confirm_status;

    return doorbell_frame_encrypt(&hdr, k_pair, plain, sizeof(plain), out, cap, out_len);
}

esp_err_t doorbell_unpack_pair_confirm(const uint8_t *buf,
                                       size_t len,
                                       const uint8_t k_pair[DOORBELL_KEY_SIZE],
                                       doorbell_pair_confirm_t *out_confirm)
{
    if (!buf || !k_pair || !out_confirm) {
        return ESP_ERR_INVALID_ARG;
    }
    doorbell_hdr_t hdr;
    uint8_t plain[16];
    size_t plain_len = 0;
    esp_err_t err = doorbell_frame_decrypt(buf, len, k_pair, &hdr, plain, sizeof(plain), &plain_len);
    if (err != ESP_OK || hdr.msg_type != DOORBELL_MSG_PAIR_CONFIRM || plain_len < 8) {
        return ESP_ERR_INVALID_MAC;
    }
    memcpy(out_confirm->unit_mac, &plain[0], 6);
    out_confirm->apt_no = plain[6];
    out_confirm->confirm_status = plain[7];
    return ESP_OK;
}
