#include "panel_state.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/task.h"
#include "esp_http_server.h"
#include "doorbell_captive_dns.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "panel_portal";

static httpd_handle_t s_httpd = NULL;
static uint32_t s_portal_ap_ipv4_host = 0;
static _Atomic int64_t s_portal_deadline_us = 0;
static _Atomic bool s_portal_close_pending = false;
static _Atomic bool s_portal_testing = false;
static _Atomic bool s_portal_abort = false;
/* True while the STA driver holds submitted-but-uncommitted credentials; panel_portal_stop() then restores the
 * committed ones from g_panel_cfg so an aborted test never leaves the panel chasing an untested network. */
static _Atomic bool s_sta_cfg_untested = false;

/**
 * @brief Take g_panel_mutex from an httpd handler without risking a deadlock.
 *
 * panel_portal_stop() runs with g_panel_mutex held and calls httpd_stop(), which waits with no timeout for the httpd
 * task to finish its current handler. A handler blocked on the mutex would therefore deadlock the worker (and trip
 * the TWDT). Poll instead and give up once the portal is being stopped.
 *
 * @return true if the mutex was taken, false if the portal is shutting down (mutex NOT taken).
 */
static bool portal_take_mutex_or_abort(void)
{
    while (xSemaphoreTake(g_panel_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (s_portal_abort) {
            return false;
        }
    }
    if (s_portal_abort) {
        panel_unlock();
        return false;
    }
    return true;
}

static bool is_request_on_softap_interface(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return false;
    }
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return false;
    }
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
        uint32_t local_ip_host = ntohl(sin->sin_addr.s_addr);
        return (local_ip_host == s_portal_ap_ipv4_host);
    }
    if (addr.ss_family == AF_INET6) {
        /* IPv4-mapped IPv6 address ::ffff:a.b.c.d */
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;
        uint32_t v4_net = 0;
        memcpy(&v4_net, &sin6->sin6_addr.s6_addr[12], 4);
        uint32_t local_ip_host = ntohl(v4_net);
        return (local_ip_host == s_portal_ap_ipv4_host);
    }
    return false;
}

/* Returns false if the value contains an encoded NUL (%00): it would silently truncate the C string, so the whole
 * submission is rejected instead (the flasher rejects NUL in SSID/passphrase the same way). */
static bool url_decode(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    size_t di = 0;
    bool ok = true;
    for (size_t si = 0; si < src_len && di + 1 < dst_cap; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && si + 2 < src_len &&
                   isxdigit((unsigned char)src[si + 1]) &&
                   isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            char c = (char)strtol(hex, NULL, 16);
            if (c == '\0') {
                ok = false;
                break;
            }
            dst[di++] = c;
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return ok;
}

/* Returns false if the field value is malformed (encoded NUL); a missing field yields "" and true */
static bool extract_form_field(const char *body, const char *key, char *out, size_t out_cap)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        if (pair_len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return url_decode(out, out_cap, p + key_len + 1, pair_len - key_len - 1);
        }
        p = amp ? (amp + 1) : NULL;
    }
    return true;
}

/* The page itself (firmware/panel/main/portal_index.html, EMBED_TXTFILES: NUL-terminated) is static; the network list
 * comes from GET /networks, so the browser refreshes it without reloading the page.
 *
 * HTTP contract with that page (docs/protocol.md §5.3 item 4), which no compiler or test checks: the GET /networks JSON
 * fields (scanning, gen, ttl, current, nets[].s/r/l), POST /scan, the POST /save form fields (gen, net = index or "m",
 * ssid, password) and the POST /save status codes (200, 400, 409, 502, 500). Change both files and the spec
 * together. */
extern const char portal_index_html_start[] asm("_binary_portal_index_html_start");
extern const char portal_index_html_end[] asm("_binary_portal_index_html_end");

/* Length (2..4) of the well-formed UTF-8 multi-byte sequence starting at @p p, or 1 if it does not start one */
static size_t utf8_seq_len(const uint8_t *p)
{
    uint8_t c = p[0];
    if (c >= 0xC2 && c <= 0xDF) {
        return (p[1] >= 0x80 && p[1] <= 0xBF) ? 2 : 1;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        uint8_t lo = (c == 0xE0) ? 0xA0 : 0x80;
        uint8_t hi = (c == 0xED) ? 0x9F : 0xBF;
        return (p[1] >= lo && p[1] <= hi && p[2] >= 0x80 && p[2] <= 0xBF) ? 3 : 1;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        uint8_t lo = (c == 0xF0) ? 0x90 : 0x80;
        uint8_t hi = (c == 0xF4) ? 0x8F : 0xBF;
        return (p[1] >= lo && p[1] <= hi && p[2] >= 0x80 && p[2] <= 0xBF && p[3] >= 0x80 && p[3] <= 0xBF) ? 4 : 1;
    }
    return 1;
}

/* JSON string body for an SSID (any bytes): well-formed UTF-8 passes through, every other byte becomes \u00XX so the
 * browser always receives valid JSON; HTML-special characters are escaped too. @p dst_cap >= 6 * 32 + 1. */
static void json_escape(const char *src, char *dst, size_t dst_cap)
{
    const uint8_t *s = (const uint8_t *)src;
    size_t di = 0;
    for (size_t si = 0; s[si] != '\0' && di + 7 <= dst_cap;) {
        size_t seq = utf8_seq_len(&s[si]);
        if (seq > 1) {
            memcpy(&dst[di], &s[si], seq);
            di += seq;
            si += seq;
            continue;
        }
        uint8_t c = s[si++];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20 || c >= 0x7F || c == '<' || c == '>' || c == '&') {
            di += (size_t)snprintf(&dst[di], 7, "\\u%04x", (unsigned)c);
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static esp_err_t portal_get_root_handler(httpd_req_t *req)
{
    /* Strict interface binding check: reject requests from building LAN */
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_index_html_start, portal_index_html_end - portal_index_html_start - 1);
}

/* Network list for the page: {"scanning":b,"gen":n,"ttl":s,"current":"ssid","nets":[{"s":"ssid","r":dBm,"l":0|1}]}.
 * The index in "nets" is what the form submits as net=<index> together with gen. Never contains the passphrase. */
static esp_err_t portal_get_networks_handler(httpd_req_t *req)
{
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }

    char current_ssid[33] = {0};
    if (!portal_take_mutex_or_abort()) {
        return ESP_FAIL;
    }
    memcpy(current_ssid, g_panel_cfg.wifi_ssid, sizeof(current_ssid) - 1);
    panel_unlock();

    doorbell_net_ap_info_t nets[DOORBELL_NET_SCAN_MAX];
    uint32_t gen = 0;
    bool scanning = false;
    size_t count = doorbell_net_get_scan_results(nets, DOORBELL_NET_SCAN_MAX, &gen, &scanning);
    int64_t left_us = s_portal_deadline_us - esp_timer_get_time();
    unsigned ttl_s = (left_us > 0) ? (unsigned)(left_us / 1000000LL) : 0U;

    char esc[200]; /* 32-byte SSID, every byte escaped to at most 6 characters */
    char row[288];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    json_escape(current_ssid, esc, sizeof(esc));
    snprintf(row, sizeof(row), "{\"scanning\":%s,\"gen\":%lu,\"ttl\":%u,\"current\":\"%s\",\"nets\":[",
             scanning ? "true" : "false", (unsigned long)gen, ttl_s, esc);
    httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; i < count; i++) {
        json_escape(nets[i].ssid, esc, sizeof(esc));
        snprintf(row, sizeof(row), "%s{\"s\":\"%s\",\"r\":%d,\"l\":%d}", (i > 0) ? "," : "", esc,
                 (int)nets[i].rssi, nets[i].secured ? 1 : 0);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Page load and the refresh button start a new asynchronous scan (one at a time; not while a credential test owns
 * the station). The page then polls GET /networks until the scan has finished. */
static esp_err_t portal_post_scan_handler(httpd_req_t *req)
{
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }
    if (!s_portal_testing) {
        doorbell_net_scan_start_async();
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Result of POST /save. The page's script uses the HTTP status (200 saved, 400 invalid input, 409 stale network list,
 * 502 test association failed, 500 storage error); the short HTML body serves the no-script fallback form. */
static esp_err_t portal_send_save_result(httpd_req_t *req, const char *status, const char *title, const char *text,
                                         bool back_link)
{
    char page[600];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>%s</title></head>"
             "<body style=\"font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px\">"
             "<h2>%s</h2><p>%s</p>%s</body></html>",
             title, title, text, back_link ? "<p><a href=\"/\">Forma geri dön</a></p>" : "");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t portal_post_save_handler(httpd_req_t *req)
{
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }

    /* Worst case: 32-byte SSID and 63-byte passphrase fully percent-encoded (3x) plus field names */
    char body[512] = {0};
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Geçersiz istek boyutu");
        return ESP_FAIL;
    }

    /* recv_wait_timeout is 2 s (panel_portal_start), so each recv is short. Retry on timeout like the ESP-IDF
     * http_server examples, but give up as soon as the portal is stopping: panel_portal_stop() runs in the
     * TWDT-subscribed worker and httpd_stop() waits for this handler to return. */
    int received = 0;
    int64_t body_deadline_us = esp_timer_get_time() + 10000000LL;
    while (received < total_len) {
        if (s_portal_abort || esp_timer_get_time() >= body_deadline_us) {
            return ESP_FAIL;
        }
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    /* Decode into oversized buffers so over-long input is detected instead of silently truncated */
    char new_ssid[48] = {0};
    char new_pass[80] = {0};
    char net_sel[8] = {0};
    char gen_str[12] = {0};
    bool fields_ok = extract_form_field(body, "ssid", new_ssid, sizeof(new_ssid));
    fields_ok = extract_form_field(body, "password", new_pass, sizeof(new_pass)) && fields_ok;
    fields_ok = extract_form_field(body, "net", net_sel, sizeof(net_sel)) && fields_ok;
    fields_ok = extract_form_field(body, "gen", gen_str, sizeof(gen_str)) && fields_ok;
    mbedtls_platform_zeroize(body, sizeof(body));

    /* A network picked from the list is identified by its index in the scan generation the page was built from,
     * so the exact SSID bytes are used (no re-encoding through the browser) and a list refreshed in the meantime
     * can never select a different network. "m" or no selection = the manually typed SSID. */
    if (fields_ok && net_sel[0] != '\0' && strcmp(net_sel, "m") != 0) {
        char *end = NULL;
        unsigned long idx = strtoul(net_sel, &end, 10);
        unsigned long page_gen = strtoul(gen_str, NULL, 10);
        doorbell_net_ap_info_t nets[DOORBELL_NET_SCAN_MAX];
        uint32_t gen = 0;
        size_t count = doorbell_net_get_scan_results(nets, DOORBELL_NET_SCAN_MAX, &gen, NULL);
        if (end == net_sel || *end != '\0' || idx >= count || page_gen != gen) {
            mbedtls_platform_zeroize(new_pass, sizeof(new_pass));
            doorbell_led_start(LED_PANEL_RESULT_ERR);
            return portal_send_save_result(req, "409 Conflict", "Ağ listesi değişti",
                                           "Listeden ağı yeniden seçin.", true);
        }
        memset(new_ssid, 0, sizeof(new_ssid));
        memcpy(new_ssid, nets[idx].ssid, sizeof(nets[idx].ssid));
    }

    /* SSID: 1..32 bytes. Passphrase: empty (open AP) or 8..63 characters (IEEE 802.11i WPA2/WPA3 passphrase) */
    if (!fields_ok || !doorbell_net_wifi_creds_valid(new_ssid, new_pass)) {
        mbedtls_platform_zeroize(new_pass, sizeof(new_pass));
        doorbell_led_start(LED_PANEL_RESULT_ERR);
        return portal_send_save_result(req, "400 Bad Request", "Geçersiz giriş",
                                       "Wi-Fi adı 1–32 karakter, şifre ise boş (açık ağ) veya 8–63 karakter olmalıdır.",
                                       true);
    }

    /* Test-Before-Commit: attempt STA connection for up to 15 seconds while keeping SoftAP open */
    ESP_LOGI(TAG, "Testing submitted Wi-Fi credentials for SSID '%s' (15 s timeout)", new_ssid);
    s_portal_testing = true;
    s_portal_deadline_us = esp_timer_get_time() + 30000000LL;
    s_sta_cfg_untested = true;
    doorbell_net_set_sta_reconnect_paused(false);
    doorbell_net_connect_sta(new_ssid, new_pass);

    esp_err_t wait_err = ESP_ERR_TIMEOUT;
    int64_t test_deadline_us = esp_timer_get_time() + 15000000LL;
    while (!s_portal_abort && g_panel_mode == MODE_PORTAL && esp_timer_get_time() < test_deadline_us) {
        if (doorbell_net_wait_for_sta_ip(250) == ESP_OK) {
            wait_err = ESP_OK;
            break;
        }
    }
    s_portal_testing = false;
    if (s_portal_abort || g_panel_mode != MODE_PORTAL) {
        /* Portal is being stopped: panel_portal_stop() restores the committed credentials (s_sta_cfg_untested) */
        mbedtls_platform_zeroize(new_pass, sizeof(new_pass));
        return ESP_FAIL;
    }
    if (wait_err == ESP_OK) {
        /* Connection succeeded: commit new credentials to NVS and close portal */
        if (!portal_take_mutex_or_abort()) {
            mbedtls_platform_zeroize(new_pass, sizeof(new_pass));
            return ESP_FAIL;
        }
        char prev_ssid[33];
        char prev_pass[65];
        memcpy(prev_ssid, g_panel_cfg.wifi_ssid, sizeof(prev_ssid));
        memcpy(prev_pass, g_panel_cfg.wifi_pass, sizeof(prev_pass));

        memset(g_panel_cfg.wifi_ssid, 0, sizeof(g_panel_cfg.wifi_ssid));
        memset(g_panel_cfg.wifi_pass, 0, sizeof(g_panel_cfg.wifi_pass));
        strncpy(g_panel_cfg.wifi_ssid, new_ssid, sizeof(g_panel_cfg.wifi_ssid) - 1);
        strncpy(g_panel_cfg.wifi_pass, new_pass, sizeof(g_panel_cfg.wifi_pass) - 1);
        esp_err_t save_err = doorbell_storage_panel_save_cfg(&g_panel_cfg);
        mbedtls_platform_zeroize(new_pass, sizeof(new_pass));

        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS save failed for new Wi-Fi credentials (%s) — rolling back", esp_err_to_name(save_err));
            memcpy(g_panel_cfg.wifi_ssid, prev_ssid, sizeof(g_panel_cfg.wifi_ssid));
            memcpy(g_panel_cfg.wifi_pass, prev_pass, sizeof(g_panel_cfg.wifi_pass));
            mbedtls_platform_zeroize(prev_pass, sizeof(prev_pass));
            doorbell_net_set_sta_reconnect_paused(true);
            if (g_panel_cfg.wifi_ssid[0] != '\0') {
                doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
            } else {
                doorbell_net_disconnect_sta();
            }
            s_sta_cfg_untested = false;
            s_portal_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S * 1000000LL;
            panel_unlock();

            doorbell_led_start(LED_PANEL_RESULT_ERR);
            return portal_send_save_result(req, "500 Internal Server Error", "Kaydedilemedi",
                                           "Wi-Fi bağlantısı kuruldu ancak ayarlar panele kaydedilemedi.", true);
        }

        mbedtls_platform_zeroize(prev_pass, sizeof(prev_pass));
        s_sta_cfg_untested = false;
        panel_unlock();

        doorbell_led_start(LED_PANEL_RESULT_OK);
        portal_send_save_result(req, "200 OK", "Bağlantı başarılı",
                                "Yeni bina Wi-Fi ayarları kaydedildi. Kurulum ağı kapatılıyor.", false);

        s_portal_close_pending = true;
        return ESP_OK;
    }

    /* Connection failed: roll back to previous NVS credentials and keep portal open */
    mbedtls_platform_zeroize(new_pass, sizeof(new_pass));
    ESP_LOGW(TAG, "Test association to '%s' failed — rolling back to previous Wi-Fi credentials", new_ssid);

    doorbell_net_set_sta_reconnect_paused(true);
    if (!portal_take_mutex_or_abort()) {
        return ESP_FAIL;
    }
    if (g_panel_cfg.wifi_ssid[0] != '\0') {
        doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
    } else {
        doorbell_net_disconnect_sta();
    }
    s_sta_cfg_untested = false;
    s_portal_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S * 1000000LL;
    panel_unlock();

    doorbell_led_start(LED_PANEL_RESULT_ERR);
    return portal_send_save_result(req, "502 Bad Gateway", "Bağlanamadı",
                                   "Panel girilen Wi-Fi ağına bağlanamadı veya IP adresi alamadı. "
                                   "Wi-Fi adını ve şifresini kontrol edip tekrar deneyin.", true);
}

/* ---- Captive portal: DNS responder + HTTP redirect on the SoftAP only ----
 * Phones probe a well-known URL right after joining a network (Android: connectivitycheck.gstatic.com/generate_204,
 * iOS: captive.apple.com/hotspot-detect.html, Windows: msftconnecttest.com/connecttest.txt). The SoftAP's DHCP server
 * already offers the panel IP as DNS server; answering every A query with that IP and redirecting unknown paths to
 * the form makes the phone show its "sign in to network" page with the form, instead of routing the setup page to
 * mobile data. Bound to the SoftAP interface and running only while the portal is open. */

#define PORTAL_DNS_PORT        53
#define PORTAL_DNS_BUF_LEN     512  /* classic DNS-over-UDP maximum */
#define PORTAL_DNS_RCV_TMO_US  250000

static _Atomic bool s_dns_run = false;
static SemaphoreHandle_t s_dns_done = NULL;

static void portal_dns_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    uint8_t buf[PORTAL_DNS_BUF_LEN];
    while (s_dns_run) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(100)); /* unexpected socket error: do not spin */
            }
            continue;
        }
        size_t reply_len = doorbell_captive_dns_reply(buf, (size_t)n, sizeof(buf), s_portal_ap_ipv4_host);
        if (reply_len > 0) {
            sendto(sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        }
    }
    close(sock);
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

/* Best effort: the portal still works by typing the address if the DNS responder cannot start */
static void portal_dns_start(void)
{
    if (s_dns_done == NULL) {
        s_dns_done = xSemaphoreCreateBinary();
        if (s_dns_done == NULL) {
            ESP_LOGW(TAG, "Captive DNS not started: no memory");
            return;
        }
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Captive DNS not started: socket errno=%d", errno);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval tmo = {.tv_sec = 0, .tv_usec = PORTAL_DNS_RCV_TMO_US};
    if (doorbell_net_bind_socket_to_softap(sock) != ESP_OK ||
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo)) < 0 ||
            bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "Captive DNS not started: bind failed (errno=%d)", errno);
        close(sock);
        return;
    }
    xSemaphoreTake(s_dns_done, 0); /* clear a stale completion */
    s_dns_run = true;
    if (xTaskCreate(portal_dns_task, "portal_dns", 3072, (void *)(intptr_t)sock, 4, NULL) != pdPASS) {
        s_dns_run = false;
        close(sock);
        ESP_LOGW(TAG, "Captive DNS not started: task creation failed");
    }
}

static void portal_dns_stop(void)
{
    if (!s_dns_run) {
        return;
    }
    s_dns_run = false;
    /* The task notices within one receive timeout (250 ms) and closes its socket; bounded well inside the TWDT */
    if (xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Captive DNS task did not stop within 1 s");
    }
}

/* Unknown paths (the phones' connectivity probes) are redirected to the form. iOS shows its captive-portal sheet only
 * when the probe response has a body, so a short one is sent. */
static esp_err_t portal_captive_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    char location[32];
    uint32_t ip = s_portal_ap_ipv4_host;
    snprintf(location, sizeof(location), "http://%u.%u.%u.%u/", (unsigned)((ip >> 24) & 0xFFU),
             (unsigned)((ip >> 16) & 0xFFU), (unsigned)((ip >> 8) & 0xFFU), (unsigned)(ip & 0xFFU));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Doorbell setup", HTTPD_RESP_USE_STRLEN);
}

esp_err_t panel_portal_start(void)
{
    if (g_panel_mode != MODE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Single-Radio Rule: pause background STA channel scan while portal is open */
    doorbell_net_set_sta_reconnect_paused(true);

    uint32_t ap_ip = 0;
    esp_err_t err = doorbell_net_start_softap("Doorbell-Setup", g_panel_cfg.admin_code, 1, &ap_ip);
    if (err != ESP_OK) {
        doorbell_net_set_sta_reconnect_paused(false);
        return err;
    }
    s_portal_ap_ipv4_host = ap_ip;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    /* httpd_stop() (called from the TWDT-subscribed worker) waits for an in-flight handler, so keep each socket wait
     * at 2 s: bounded well inside the 5 s TWDT window, yet tolerant of a slow or briefly interrupted SoftAP
     * link while request headers arrive or the result page is sent. */
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;

    /* Bind the listener to the SoftAP netif (SO_BINDTODEVICE). The getsockname() check in the handlers stays as
     * defense in depth, but on its own it can be bypassed: lwIP's weak host model accepts a packet for the SoftAP IP
     * that arrives on the STA (building LAN) interface. Static: httpd keeps the if_name pointer in its config copy. */
    static struct ifreq ap_ifr;
    memset(&ap_ifr, 0, sizeof(ap_ifr));
    err = doorbell_net_get_softap_if_name(ap_ifr.ifr_name);
    if (err != ESP_OK) {
        doorbell_net_stop_softap();
        doorbell_net_set_sta_reconnect_paused(false);
        return err;
    }
    config.if_name = &ap_ifr;

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        doorbell_net_stop_softap();
        doorbell_net_set_sta_reconnect_paused(false);
        return err;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_get_root_handler,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = portal_post_save_handler,
    };
    static const httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_POST,
        .handler = portal_post_scan_handler,
    };
    static const httpd_uri_t networks_uri = {
        .uri = "/networks",
        .method = HTTP_GET,
        .handler = portal_get_networks_handler,
    };
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &save_uri);
    httpd_register_uri_handler(s_httpd, &scan_uri);
    httpd_register_uri_handler(s_httpd, &networks_uri);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, portal_captive_redirect_handler);
    /* Every connectivity probe is a 404 before the redirect; keep them out of the log */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    portal_dns_start();
    /* Network list for the form; usually finished before the installer's phone has joined (one-shot scan,
     * returning to the SoftAP channel between scanned channels, never a background scan) */
    doorbell_net_scan_start_async();

    g_panel_mode = MODE_PORTAL;
    s_portal_close_pending = false;
    s_portal_testing = false;
    s_portal_abort = false;
    s_sta_cfg_untested = false;
    s_portal_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S * 1000000LL;

    /* 2 s solid confirmation followed by 1 Hz slow blink while portal is open (docs/protocol.md §7.1) */
    doorbell_led_start(LED_PANEL_PORTAL);
    doorbell_led_start(LED_PANEL_RESULT_OK);

    ESP_LOGI(TAG, "Setup web portal active on Doorbell-Setup for %d s", CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S);
    return ESP_OK;
}

void panel_portal_stop(void)
{
    s_portal_abort = true;
    portal_dns_stop();
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    doorbell_led_stop(LED_PANEL_PORTAL);
    doorbell_net_stop_softap();

    /* httpd_stop() has returned, so no handler is running. If a credential test was interrupted, the STA driver still
     * holds the submitted (uncommitted) network: switch back to the committed credentials BEFORE unpausing STA
     * reconnect so the driver never initiates a spurious connection attempt to the aborted network. Callers hold
     * g_panel_mutex, so g_panel_cfg is safe to read here. */
    if (atomic_exchange(&s_sta_cfg_untested, false)) {
        /* The credential test un-paused STA reconnect. Pause it so connect_sta() only rewrites the config; the single
         * set_sta_reconnect_paused(false) below then starts exactly one esp_wifi_connect() */
        doorbell_net_set_sta_reconnect_paused(true);
        ESP_LOGW(TAG, "Credential test interrupted — restoring committed Wi-Fi credentials");
        if (g_panel_cfg.wifi_ssid[0] != '\0') {
            doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
        } else {
            doorbell_net_disconnect_sta();
        }
    }

    doorbell_net_set_sta_reconnect_paused(false);

    s_portal_deadline_us = 0;
    s_portal_close_pending = false;
    s_portal_testing = false;
    if (g_panel_mode == MODE_PORTAL) {
        g_panel_mode = MODE_IDLE;
    }
    ESP_LOGI(TAG, "Setup web portal closed");
}

void panel_portal_tick(void)
{
    if (g_panel_mode != MODE_PORTAL || s_portal_testing) {
        return;
    }
    if (s_portal_close_pending || (s_portal_deadline_us > 0 && esp_timer_get_time() >= s_portal_deadline_us)) {
        panel_lock();
        panel_portal_stop();
        panel_unlock();
    }
}
