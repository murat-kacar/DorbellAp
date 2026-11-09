#include "panel_state.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "panel_portal";

static httpd_handle_t s_httpd = NULL;
static uint32_t s_portal_ap_ipv4_host = 0;
static volatile int64_t s_portal_deadline_us = 0;
static volatile bool s_portal_close_pending = false;
static volatile bool s_portal_testing = false;
static volatile bool s_portal_abort = false;
/* True while the STA driver holds submitted-but-uncommitted credentials; panel_portal_stop() then restores the
 * committed ones from g_panel_cfg so an aborted test never leaves the panel chasing an untested network. */
static volatile bool s_sta_cfg_untested = false;

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
        xSemaphoreGive(g_panel_mutex);
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

static void html_escape(const char *src, char *dst, size_t dst_cap)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 6 < dst_cap; si++) {
        char c = src[si];
        if (c == '&') {
            memcpy(&dst[di], "&amp;", 5);
            di += 5;
        } else if (c == '<') {
            memcpy(&dst[di], "&lt;", 4);
            di += 4;
        } else if (c == '>') {
            memcpy(&dst[di], "&gt;", 4);
            di += 4;
        } else if (c == '"') {
            memcpy(&dst[di], "&quot;", 6);
            di += 6;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

static esp_err_t portal_get_root_handler(httpd_req_t *req)
{
    /* Strict interface binding check (I-47): reject requests from building LAN */
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }

    char current_ssid_esc[192] = {0};
    if (!portal_take_mutex_or_abort()) {
        return ESP_FAIL;
    }
    html_escape(g_panel_cfg.wifi_ssid, current_ssid_esc, sizeof(current_ssid_esc));
    xSemaphoreGive(g_panel_mutex);

    /* Never echo stored passphrase in HTML (I-47) */
    char page[1600];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Zil Paneli Wi-Fi Kurulumu</title>"
             "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;}"
             "label{display:block;margin-top:14px;font-weight:bold;}"
             "input{width:100%%;padding:8px;margin-top:4px;box-sizing:border-box;}"
             "button{margin-top:18px;width:100%%;padding:10px;font-size:16px;}</style></head>"
             "<body><h2>Zil Paneli Bina Wi-Fi Ayarları</h2>"
             "<form method=\"POST\" action=\"/save\">"
             "<label>Bina Wi-Fi Adı (SSID):</label>"
             "<input type=\"text\" name=\"ssid\" maxlength=\"32\" value=\"%s\" required>"
             "<label>Bina Wi-Fi Şifresi (WPA2/WPA3):</label>"
             "<input type=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"Yeni şifreyi girin\">"
             "<p>Not: Panel farklı kanaldaki bir ağa bağlanırken bu kurulum ağı kısa süre kopabilir. "
             "Sonucu panel LED'inden takip edin: 2 sn sabit = başarılı, 1 sn hızlı yanıp sönme = başarısız.</p>"
             "<button type=\"submit\">Kaydet ve Bağlan</button>"
             "</form></body></html>",
             current_ssid_esc);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_post_save_handler(httpd_req_t *req)
{
    if (!is_request_on_softap_interface(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }

    char body[256] = {0};
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Geçersiz istek boyutu");
        return ESP_FAIL;
    }

    /* recv_wait_timeout is 2 s (panel_portal_start), so each recv is short. Retry on timeout like the ESP-IDF
     * http_server examples, but give up as soon as the portal is stopping: panel_portal_stop() runs in the
     * TWDT-subscribed worker and httpd_stop() waits for this handler to return (I-46). */
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
    bool fields_ok = extract_form_field(body, "ssid", new_ssid, sizeof(new_ssid));
    fields_ok = extract_form_field(body, "password", new_pass, sizeof(new_pass)) && fields_ok;
    memset(body, 0, sizeof(body));

    /* SSID: 1..32 bytes. Passphrase: empty (open AP) or 8..63 characters (IEEE 802.11i WPA2/WPA3 passphrase) */
    size_t ssid_len = strlen(new_ssid);
    size_t pass_len = strlen(new_pass);
    if (!fields_ok || ssid_len == 0 || ssid_len > 32 || (pass_len != 0 && (pass_len < 8 || pass_len > 63))) {
        memset(new_pass, 0, sizeof(new_pass));
        doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\"></head>"
            "<body><h3>Hata: Wi-Fi adı 1-32 karakter, şifre ise 8-63 karakter olmalıdır.</h3>"
            "<p><a href=\"/\">Geri dön</a></p></body></html>");
        return ESP_OK;
    }

    /* Test-Before-Commit (I-40): attempt STA connection for up to 15 seconds while keeping SoftAP open */
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
        memset(new_pass, 0, sizeof(new_pass));
        return ESP_FAIL;
    }
    if (wait_err == ESP_OK) {
        /* Connection succeeded: commit new credentials to NVS and close portal */
        if (!portal_take_mutex_or_abort()) {
            memset(new_pass, 0, sizeof(new_pass));
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
        memset(new_pass, 0, sizeof(new_pass));

        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS save failed for new Wi-Fi credentials (%s) — rolling back", esp_err_to_name(save_err));
            memcpy(g_panel_cfg.wifi_ssid, prev_ssid, sizeof(g_panel_cfg.wifi_ssid));
            memcpy(g_panel_cfg.wifi_pass, prev_pass, sizeof(g_panel_cfg.wifi_pass));
            memset(prev_pass, 0, sizeof(prev_pass));
            doorbell_net_set_sta_reconnect_paused(true);
            if (g_panel_cfg.wifi_ssid[0] != '\0') {
                doorbell_net_connect_sta(g_panel_cfg.wifi_ssid, g_panel_cfg.wifi_pass);
            } else {
                doorbell_net_disconnect_sta();
            }
            s_sta_cfg_untested = false;
            s_portal_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S * 1000000LL;
            xSemaphoreGive(g_panel_mutex);

            doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            return httpd_resp_sendstr(req,
                "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
                "<body><h3>Kayıt hatası!</h3>"
                "<p>Wi-Fi bağlantısı kuruldu ancak ayarlar kalıcı hafızaya (NVS) kaydedilemedi.</p>"
                "<p><a href=\"/\">Forma geri dön</a></p></body></html>");
        }

        memset(prev_pass, 0, sizeof(prev_pass));
        s_sta_cfg_untested = false;
        xSemaphoreGive(g_panel_mutex);

        doorbell_led_panel_start(LED_PANEL_RESULT_OK);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
            "<body><h3>Bağlantı başarılı!</h3>"
            "<p>Yeni bina Wi-Fi ayarları kaydedildi. Kurulum ağı kapatılıyor.</p></body></html>");

        s_portal_close_pending = true;
        return ESP_OK;
    }

    /* Connection failed: roll back to previous NVS credentials and keep portal open (I-40) */
    memset(new_pass, 0, sizeof(new_pass));
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
    xSemaphoreGive(g_panel_mutex);

    doorbell_led_panel_start(LED_PANEL_RESULT_ERR);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!DOCTYPE html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
        "<body><h3>Bağlantı başarısız!</h3>"
        "<p>Panel girilen Wi-Fi ağına bağlanamadı veya IP adresi alamadı. "
        "Wi-Fi adını ve şifresini kontrol edip tekrar deneyin.</p>"
        "<p><a href=\"/\">Forma geri dön</a></p></body></html>");
}

esp_err_t panel_portal_start(void)
{
    if (g_panel_mode != MODE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Single-Radio Rule (I-31): pause background STA channel scan while portal is open */
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
     * at 2 s: bounded well inside the 5 s TWDT window (I-46), yet tolerant of a slow or briefly interrupted SoftAP
     * link while request headers arrive or the result page is sent. */
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;

    /* Bind the listener to the SoftAP netif (SO_BINDTODEVICE, I-47). The getsockname() check in the handlers stays as
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
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &save_uri);

    g_panel_mode = MODE_PORTAL;
    s_portal_close_pending = false;
    s_portal_testing = false;
    s_portal_abort = false;
    s_sta_cfg_untested = false;
    s_portal_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S * 1000000LL;

    /* 2 s solid confirmation followed by 1 Hz slow blink while portal is open (I-40, §7.1) */
    doorbell_led_panel_start(LED_PANEL_PORTAL);
    doorbell_led_panel_start(LED_PANEL_RESULT_OK);

    ESP_LOGI(TAG, "Setup web portal active on Doorbell-Setup for %d s", CONFIG_DOORBELL_PANEL_PORTAL_TIMEOUT_S);
    return ESP_OK;
}

void panel_portal_stop(void)
{
    s_portal_abort = true;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    doorbell_led_panel_stop(LED_PANEL_PORTAL);
    doorbell_net_stop_softap();

    /* httpd_stop() has returned, so no handler is running. If a credential test was interrupted, the STA driver still
     * holds the submitted (uncommitted) network: switch back to the committed credentials BEFORE unpausing STA
     * reconnect so the driver never initiates a spurious connection attempt to the aborted network. Callers hold
     * g_panel_mutex, so g_panel_cfg is safe to read here. */
    if (s_sta_cfg_untested) {
        s_sta_cfg_untested = false;
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
        xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
        panel_portal_stop();
        xSemaphoreGive(g_panel_mutex);
    }
}

