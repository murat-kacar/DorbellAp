/* Host unit tests (ESP-IDF linux target) for doorbell_protocol and doorbell_captive_dns: they run the product C code,
 * complementing tools/verify_protocol_vectors.py, which checks the specification with Python reference models. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "doorbell_protocol.h"
#include "doorbell_captive_dns.h"

static const uint8_t KEY[DOORBELL_KEY_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                               0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

static doorbell_hdr_t make_hdr(uint8_t msg_type, uint32_t tx_counter)
{
    doorbell_hdr_t h = {
        .magic = DOORBELL_PROTO_MAGIC,
        .version = DOORBELL_PROTO_VERSION,
        .msg_type = msg_type,
        .flags = DOORBELL_FLAG_DIR_UNIT_TO_PANEL,
        .src_id = 7,
        .dst_id = DOORBELL_ID_PANEL,
        .seq_id = 0x1234,
        .tx_counter = tx_counter,
    };
    return h;
}

/* ---- Wire header ---- */

TEST_CASE("header serialize / parse round trip, big-endian fields", "[protocol]")
{
    doorbell_hdr_t h = make_hdr(DOORBELL_MSG_OPEN_DOOR_REQ, 0x01020304);
    uint8_t wire[DOORBELL_HDR_SIZE];
    doorbell_hdr_serialize(&h, wire);
    TEST_ASSERT_EQUAL_HEX8(DOORBELL_PROTO_MAGIC, wire[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, wire[6]);
    TEST_ASSERT_EQUAL_HEX8(0x34, wire[7]);
    TEST_ASSERT_EQUAL_HEX8(0x01, wire[8]);
    TEST_ASSERT_EQUAL_HEX8(0x04, wire[11]);

    doorbell_hdr_t back;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_hdr_parse(wire, sizeof(wire), &back));
    TEST_ASSERT_EQUAL(h.msg_type, back.msg_type);
    TEST_ASSERT_EQUAL(h.flags, back.flags);
    TEST_ASSERT_EQUAL(h.src_id, back.src_id);
    TEST_ASSERT_EQUAL(h.dst_id, back.dst_id);
    TEST_ASSERT_EQUAL(h.seq_id, back.seq_id);
    TEST_ASSERT_EQUAL_UINT32(h.tx_counter, back.tx_counter);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, doorbell_hdr_parse(wire, DOORBELL_HDR_SIZE - 1, &back));
    wire[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, doorbell_hdr_parse(wire, sizeof(wire), &back));
}

/* ---- AEAD frames ---- */

TEST_CASE("frame AEAD round trip; transport flags outside AAD; tampering rejected", "[protocol]")
{
    doorbell_hdr_t h = make_hdr(DOORBELL_MSG_HEARTBEAT_REQ, 5);
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_frame_encrypt(&h, KEY, payload, sizeof(payload), frame, sizeof(frame),
                                                     &frame_len));
    TEST_ASSERT_EQUAL(DOORBELL_HDR_SIZE + sizeof(payload) + DOORBELL_TAG_SIZE, frame_len);

    doorbell_hdr_t out;
    uint8_t plain[32];
    size_t plain_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_frame_decrypt(frame, frame_len, KEY, &out, plain, sizeof(plain), &plain_len));
    TEST_ASSERT_EQUAL(sizeof(payload), plain_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, plain, sizeof(payload));

    /* is_retry / is_bcast are not authenticated (docs/protocol.md §2.3): a resent frame still decrypts */
    doorbell_frame_set_transport_flags(frame, true, true);
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_frame_decrypt(frame, frame_len, KEY, &out, plain, sizeof(plain), &plain_len));

    /* Any authenticated header byte or ciphertext / tag byte changed: rejected */
    frame[4] ^= 0x01; /* src_id */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_MAC,
                      doorbell_frame_decrypt(frame, frame_len, KEY, &out, plain, sizeof(plain), &plain_len));
    frame[4] ^= 0x01;
    frame[frame_len - 1] ^= 0x80;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_MAC,
                      doorbell_frame_decrypt(frame, frame_len, KEY, &out, plain, sizeof(plain), &plain_len));
}

TEST_CASE("frame counters 0 and above the valid range are refused", "[protocol]")
{
    const uint8_t payload[4] = {0};
    uint8_t frame[DOORBELL_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    doorbell_hdr_t h = make_hdr(DOORBELL_MSG_RING_REQ, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      doorbell_frame_encrypt(&h, KEY, payload, sizeof(payload), frame, sizeof(frame), &frame_len));
    h.tx_counter = DOORBELL_COUNTER_MAX_VALID + 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      doorbell_frame_encrypt(&h, KEY, payload, sizeof(payload), frame, sizeof(frame), &frame_len));
}

/* ---- Payloads ---- */

TEST_CASE("OPEN_DOOR_ACK and HEARTBEAT_REQ payload round trips", "[protocol]")
{
    doorbell_pl_open_door_ack_t a = {
        .req_tx_counter = 0x11223344,
        .req_seq_id = 0x5566,
        .status = DOORBELL_STATUS_CHALLENGE,
        .sender_ipv4 = 0xC0A80102,
        .panel_boot_id = 0xA1B2C3D4,
        .challenge_nonce = 0x0102030405060708ULL,
    };
    uint8_t buf[37];
    TEST_ASSERT_EQUAL(23, doorbell_pack_open_door_ack(&a, buf));
    doorbell_pl_open_door_ack_t b;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_unpack_open_door_ack(buf, 23, &b));
    TEST_ASSERT_EQUAL_UINT32(a.req_tx_counter, b.req_tx_counter);
    TEST_ASSERT_EQUAL(a.req_seq_id, b.req_seq_id);
    TEST_ASSERT_EQUAL(a.status, b.status);
    TEST_ASSERT_EQUAL_UINT32(a.sender_ipv4, b.sender_ipv4);
    TEST_ASSERT_EQUAL_UINT32(a.panel_boot_id, b.panel_boot_id);
    TEST_ASSERT_TRUE(a.challenge_nonce == b.challenge_nonce);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, doorbell_unpack_open_door_ack(buf, 22, &b));

    doorbell_pl_heartbeat_req_t hb = {
        .sender_ipv4 = 0xC0A80105,
        .uptime_s = 86400,
        .ring_count = 3,
        .ap_rssi = -67,
        .ap_channel = 6,
        .ap_bssid = {1, 2, 3, 4, 5, 6},
        .fw_version_major = 1,
        .fw_version_minor = 1,
        .heartbeat_interval_s = 600,
    };
    TEST_ASSERT_EQUAL(37, doorbell_pack_heartbeat_req(&hb, buf));
    doorbell_pl_heartbeat_req_t hb2;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_unpack_heartbeat_req(buf, 37, &hb2));
    TEST_ASSERT_EQUAL_INT8(-67, hb2.ap_rssi);
    TEST_ASSERT_EQUAL_UINT32(86400, hb2.uptime_s);
    TEST_ASSERT_EQUAL_MEMORY(hb.ap_bssid, hb2.ap_bssid, 6);
    TEST_ASSERT_EQUAL(600, hb2.heartbeat_interval_s);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, doorbell_unpack_heartbeat_req(buf, 36, &hb2));
}

/* ---- Pairing ---- */

TEST_CASE("X25519 + HKDF: both sides derive the same k_pair, bound to the apartment", "[pairing]")
{
    doorbell_x25519_ctx_t panel = {0};
    doorbell_x25519_ctx_t unit = {0};
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_x25519_generate(&panel));
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_x25519_generate(&unit));

    uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE];
    uint8_t unit_nonce[DOORBELL_PAIR_NONCE_SIZE];
    for (size_t i = 0; i < DOORBELL_PAIR_NONCE_SIZE; i++) {
        panel_nonce[i] = (uint8_t)i;
        unit_nonce[i] = (uint8_t)(0xF0 + i);
    }
    const uint8_t panel_mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    const uint8_t unit_mac[6] = {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0};
    uint8_t k_panel[DOORBELL_KEY_SIZE];
    uint8_t k_unit[DOORBELL_KEY_SIZE];
    uint8_t k_other_apt[DOORBELL_KEY_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_x25519_derive_kpair(&panel, unit.pub_key, panel_nonce, unit_nonce, panel_mac,
                                                           unit_mac, 7, k_panel));
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_x25519_derive_kpair(&unit, panel.pub_key, panel_nonce, unit_nonce, panel_mac,
                                                           unit_mac, 7, k_unit));
    TEST_ASSERT_EQUAL_MEMORY(k_panel, k_unit, DOORBELL_KEY_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_x25519_derive_kpair(&panel, unit.pub_key, panel_nonce, unit_nonce, panel_mac,
                                                           unit_mac, 8, k_other_apt));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(k_panel, k_other_apt, DOORBELL_KEY_SIZE));

    /* All-zero (low-order) peer key must never produce a key */
    const uint8_t zero_pub[DOORBELL_X25519_PUB_SIZE] = {0};
    TEST_ASSERT_NOT_EQUAL(ESP_OK, doorbell_x25519_derive_kpair(&panel, zero_pub, panel_nonce, unit_nonce, panel_mac,
                                                               unit_mac, 7, k_other_apt));
    doorbell_x25519_free(&panel);
    doorbell_x25519_free(&unit);
}

TEST_CASE("PAIR_CHALLENGE provisioning blob round trip and wrong key rejected", "[pairing]")
{
    doorbell_provision_blob_t blob = {0};
    strcpy(blob.wifi_ssid, "Bina AP");
    strcpy(blob.wifi_pass, "password123");
    blob.ap_channel = 6;
    blob.panel_lan_ipv4 = 0xC0A80101;
    for (size_t i = 0; i < DOORBELL_KEY_SIZE; i++) {
        blob.k_apt[i] = (uint8_t)(0x40 + i);
    }
    blob.heartbeat_interval_s = 600;

    const uint8_t panel_mac[6] = {1, 2, 3, 4, 5, 6};
    uint8_t panel_nonce[DOORBELL_PAIR_NONCE_SIZE] = {9};
    uint8_t panel_pub[DOORBELL_X25519_PUB_SIZE] = {7};
    uint8_t k_pair[DOORBELL_KEY_SIZE];
    memcpy(k_pair, KEY, sizeof(k_pair));

    uint8_t frame[DOORBELL_HDR_SIZE + 55 + DOORBELL_PROVISION_BLOB_ENC_SIZE];
    size_t frame_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_pack_pair_challenge(panel_mac, panel_nonce, panel_pub, 7, &blob, k_pair, frame,
                                                           sizeof(frame), &frame_len));
    TEST_ASSERT_EQUAL(sizeof(frame), frame_len);

    doorbell_pair_challenge_t ch;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_unpack_pair_challenge_header(frame, frame_len, &ch));
    TEST_ASSERT_EQUAL(7, ch.apt_no);
    TEST_ASSERT_EQUAL_MEMORY(panel_mac, ch.panel_mac, 6);

    doorbell_provision_blob_t out;
    TEST_ASSERT_EQUAL(ESP_OK, doorbell_decrypt_provision_blob(frame, frame_len, k_pair, &out));
    TEST_ASSERT_EQUAL_STRING(blob.wifi_ssid, out.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING(blob.wifi_pass, out.wifi_pass);
    TEST_ASSERT_EQUAL(blob.ap_channel, out.ap_channel);
    TEST_ASSERT_EQUAL_UINT32(blob.panel_lan_ipv4, out.panel_lan_ipv4);
    TEST_ASSERT_EQUAL_MEMORY(blob.k_apt, out.k_apt, DOORBELL_KEY_SIZE);
    TEST_ASSERT_EQUAL(blob.heartbeat_interval_s, out.heartbeat_interval_s);

    k_pair[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_MAC, doorbell_decrypt_provision_blob(frame, frame_len, k_pair, &out));
}

/* ---- Captive portal DNS ---- */

static size_t make_dns_query(uint8_t *p, const char *name, uint8_t qtype, bool edns)
{
    memset(p, 0, 12);
    p[0] = 0x12;
    p[1] = 0x34;
    p[2] = 0x01; /* RD */
    p[5] = 1;    /* QDCOUNT */
    size_t pos = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t label = dot ? (size_t)(dot - s) : strlen(s);
        p[pos++] = (uint8_t)label;
        memcpy(&p[pos], s, label);
        pos += label;
        s += label + (dot ? 1 : 0);
    }
    p[pos++] = 0;
    p[pos++] = 0;
    p[pos++] = qtype;
    p[pos++] = 0;
    p[pos++] = 1; /* class IN */
    if (edns) {
        static const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
        p[11] = 1; /* ARCOUNT */
        memcpy(&p[pos], opt, sizeof(opt));
        pos += sizeof(opt);
    }
    return pos;
}

TEST_CASE("captive DNS: A query answered with the SoftAP address, AAAA empty, EDNS dropped", "[dns]")
{
    const uint32_t ip = (192U << 24) | (168U << 16) | (4U << 8) | 1U;
    uint8_t pkt[512];
    size_t q = make_dns_query(pkt, "connectivitycheck.gstatic.com", 1, true);
    size_t r = doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip);
    TEST_ASSERT_EQUAL(q - 11 + 16, r);
    TEST_ASSERT_EQUAL_HEX8(0x12, pkt[0]);
    TEST_ASSERT_EQUAL_HEX8(0x85, pkt[2]); /* QR | AA | RD */
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[3]); /* NOERROR */
    TEST_ASSERT_EQUAL(1, pkt[7]);          /* ANCOUNT */
    TEST_ASSERT_EQUAL(0, pkt[11]);         /* ARCOUNT */
    const uint8_t addr[4] = {192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(addr, &pkt[r - 4], 4);
    TEST_ASSERT_EQUAL(DOORBELL_CAPTIVE_DNS_TTL_S, pkt[r - 7]);

    q = make_dns_query(pkt, "captive.apple.com", 28, false); /* AAAA */
    r = doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip);
    TEST_ASSERT_EQUAL(q, r);
    TEST_ASSERT_EQUAL(0, pkt[7]);
}

TEST_CASE("captive DNS: malformed packets are dropped, never answered", "[dns]")
{
    const uint32_t ip = 0xC0A80401;
    uint8_t pkt[512];
    size_t q = make_dns_query(pkt, "a.b", 1, false);
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, 11, sizeof(pkt), ip)); /* short header */
    q = make_dns_query(pkt, "a.b", 1, false);
    pkt[2] |= 0x80; /* a response */
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip));
    q = make_dns_query(pkt, "a.b", 1, false);
    pkt[5] = 2; /* two questions */
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip));
    q = make_dns_query(pkt, "a.b", 1, false);
    pkt[12] = 0xC0; /* compression pointer inside the question */
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip));
    q = make_dns_query(pkt, "a.b", 1, false);
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, q - 2, sizeof(pkt), ip)); /* truncated */
    q = make_dns_query(pkt, "abc", 1, false);
    pkt[12] = 60; /* label running past the packet */
    TEST_ASSERT_EQUAL(0, doorbell_captive_dns_reply(pkt, q, sizeof(pkt), ip));
}

TEST_CASE("captive DNS: random packets never produce a reply longer than the buffer", "[dns]")
{
    uint32_t seed = 1234;
    for (int it = 0; it < 200000; it++) {
        seed = seed * 1103515245U + 12345U;
        size_t len = (seed >> 16) % 80;
        size_t cap = len + ((seed >> 8) % 40);
        uint8_t *p = malloc(cap ? cap : 1);
        TEST_ASSERT_NOT_NULL(p);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245U + 12345U;
            p[i] = (uint8_t)(seed >> 16);
        }
        if (len >= 6 && (seed & 1U)) {
            p[2] &= 0x07; /* standard query */
            p[4] = 0;
            p[5] = 1;
        }
        size_t out = doorbell_captive_dns_reply(p, len, cap, 0xC0A80401);
        TEST_ASSERT_TRUE(out <= cap);
        free(p);
    }
}

void app_main(void)
{
    if (doorbell_crypto_init() != ESP_OK) {
        printf("psa_crypto_init failed\n");
        exit(1);
    }
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    exit(failures == 0 ? 0 : 1); /* exit status for CI */
}
