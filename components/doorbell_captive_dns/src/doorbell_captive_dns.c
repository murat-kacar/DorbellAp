#include "doorbell_captive_dns.h"
#include <stdbool.h>
#include <string.h>

size_t doorbell_captive_dns_reply(uint8_t *pkt, size_t len, size_t cap, uint32_t ap_ipv4_host)
{
    if (len < 12 || len > cap) {
        return 0;
    }
    if ((pkt[2] & 0x80) != 0 || ((pkt[2] >> 3) & 0x0F) != 0) {
        return 0; /* a response, or not a standard QUERY */
    }
    if (pkt[4] != 0 || pkt[5] != 1) {
        return 0; /* QDCOUNT must be 1 */
    }

    size_t pos = 12;
    size_t name_len = 0;
    for (;;) {
        if (pos >= len) {
            return 0;
        }
        uint8_t label = pkt[pos];
        if (label == 0) {
            pos++;
            break;
        }
        if (label > 63) {
            return 0; /* compression pointers / extended labels are not valid in a question */
        }
        name_len += (size_t)label + 1;
        if (name_len > 255) {
            return 0;
        }
        pos += 1 + (size_t)label;
    }
    if (pos + 4 > len) {
        return 0;
    }
    bool answer = (pkt[pos] == 0 && pkt[pos + 1] == 1 && pkt[pos + 2] == 0 && pkt[pos + 3] == 1); /* A, IN */
    pos += 4;
    if (answer && pos + 16 > cap) {
        return 0;
    }

    pkt[2] = (uint8_t)(0x80 | 0x04 | (pkt[2] & 0x01)); /* QR, AA, keep RD; opcode QUERY */
    pkt[3] = 0x00;                                      /* RA = 0, RCODE = NOERROR */
    pkt[6] = 0;
    pkt[7] = answer ? 1 : 0;                            /* ANCOUNT */
    memset(&pkt[8], 0, 4);                              /* NSCOUNT, ARCOUNT */
    if (answer) {
        uint8_t *a = &pkt[pos];
        a[0] = 0xC0;
        a[1] = 0x0C; /* name: pointer to the question */
        a[2] = 0x00;
        a[3] = 0x01; /* TYPE A */
        a[4] = 0x00;
        a[5] = 0x01; /* CLASS IN */
        a[6] = 0x00;
        a[7] = 0x00;
        a[8] = 0x00;
        a[9] = DOORBELL_CAPTIVE_DNS_TTL_S;
        a[10] = 0x00;
        a[11] = 0x04; /* RDLENGTH */
        a[12] = (uint8_t)(ap_ipv4_host >> 24);
        a[13] = (uint8_t)(ap_ipv4_host >> 16);
        a[14] = (uint8_t)(ap_ipv4_host >> 8);
        a[15] = (uint8_t)ap_ipv4_host;
        pos += 16;
    }
    return pos;
}
