#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TTL of the captive answers: short, so they never outlive the 2-minute setup portal in client caches */
#define DOORBELL_CAPTIVE_DNS_TTL_S 10

/**
 * @brief Turn one DNS query in @p pkt into the captive-portal reply, in place.
 *
 * Only standard queries with exactly one question are answered. QTYPE A / class IN gets @p ap_ipv4_host; any other
 * type (e.g. AAAA) gets an empty NOERROR answer so the client falls back to IPv4. Additional records (EDNS) are
 * dropped. Every length is checked against the received size: malformed packets are ignored.
 *
 * @param pkt          received query; overwritten with the reply
 * @param len          received length
 * @param cap          capacity of @p pkt (the reply is up to 16 bytes longer than the query)
 * @param ap_ipv4_host address to answer with (host byte order)
 * @return reply length, or 0 to send nothing
 */
size_t doorbell_captive_dns_reply(uint8_t *pkt, size_t len, size_t cap, uint32_t ap_ipv4_host);

#ifdef __cplusplus
}
#endif
