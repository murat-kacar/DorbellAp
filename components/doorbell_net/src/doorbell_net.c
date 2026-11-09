#include "doorbell_net.h"
#include "doorbell_config_checks.h"
#include "doorbell_protocol.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "doorbell_net";

#define NET_BIT_CONNECTED BIT0
#define NET_BIT_FAIL      BIT1

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static EventGroupHandle_t s_net_evt = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

static doorbell_net_state_cb_t s_state_cb = NULL;
static void *s_state_cb_ctx = NULL;

static volatile bool s_sta_has_creds = false;
static volatile bool s_sta_paused = false;
static volatile bool s_sta_connected = false;
static volatile uint32_t s_sta_ipv4_host = 0;
static volatile uint32_t s_last_sta_ipv4_host = 0;
static volatile uint32_t s_backoff_ms = 1000;
static volatile uint16_t s_disconnect_count = 0;

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_sta_has_creds && !s_sta_paused && !s_sta_connected) {
        esp_wifi_connect();
    }
}

static void schedule_reconnect(void)
{
    if (!s_sta_has_creds || s_sta_paused || !s_reconnect_timer) {
        return;
    }
    uint32_t base_ms = s_backoff_ms;
    if (base_ms < 1000) {
        base_ms = 1000;
    }
    /* Add +/- 15% jitter (I-11) */
    uint32_t jitter_span = base_ms / 6;
    int32_t jitter = (jitter_span > 0) ? ((int32_t)(esp_random() % (2 * jitter_span)) - (int32_t)jitter_span) : 0;
    int64_t delay_ms = (int64_t)base_ms + jitter;
    if (delay_ms < 500) {
        delay_ms = 500;
    }

    uint32_t next_backoff = base_ms * 2;
    if (next_backoff > 60000) {
        next_backoff = 60000;
    }
    s_backoff_ms = next_backoff;

    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            bool was_connected = s_sta_connected;
            s_sta_connected = false;
            s_sta_ipv4_host = 0;
            if (s_disconnect_count < 0xFFFFU) {
                s_disconnect_count++;
            }
            if (s_net_evt) {
                xEventGroupClearBits(s_net_evt, NET_BIT_CONNECTED);
                xEventGroupSetBits(s_net_evt, NET_BIT_FAIL);
            }
            if (was_connected && s_state_cb) {
                s_state_cb(false, 0, s_state_cb_ctx);
            }
            schedule_reconnect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        uint32_t ip_host = ntohl(event->ip_info.ip.addr);
        s_sta_ipv4_host = ip_host;
        s_last_sta_ipv4_host = ip_host;
        s_sta_connected = true;
        s_backoff_ms = 1000;
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        if (s_net_evt) {
            xEventGroupClearBits(s_net_evt, NET_BIT_FAIL);
            xEventGroupSetBits(s_net_evt, NET_BIT_CONNECTED);
        }
        ESP_LOGI(TAG, "STA acquired IPv4: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_state_cb) {
            s_state_cb(true, ip_host, s_state_cb_ctx);
        }
    }
}

esp_err_t doorbell_net_init(const char *hostname, doorbell_net_state_cb_t cb, void *ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = ctx;

    s_net_evt = xEventGroupCreate();
    if (!s_net_evt) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t t_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_FAIL;
    }

    if (hostname && hostname[0] != '\0') {
        esp_netif_set_hostname(s_sta_netif, hostname);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Mandatory rule (I-23): Never store credentials in nvs.net80211 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Mandatory rule (I-70): Disable Wi-Fi power save for bounded 400 ms ACK latency */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

#if defined(CONFIG_DOORBELL_WIFI_SET_COUNTRY) && CONFIG_DOORBELL_WIFI_SET_COUNTRY
    if (strlen(CONFIG_DOORBELL_WIFI_COUNTRY_CODE) == 2) {
        esp_wifi_set_country_code(CONFIG_DOORBELL_WIFI_COUNTRY_CODE, true);
        ESP_LOGI(TAG, "Applied Wi-Fi country code: %s", CONFIG_DOORBELL_WIFI_COUNTRY_CODE);
    }
#endif

    /* Initialize PSA Crypto only AFTER esp_wifi_start() has enabled RF hardware RNG entropy (I-26) */
    ESP_ERROR_CHECK(doorbell_crypto_init());

    return ESP_OK;
}

esp_err_t doorbell_net_set_hostname(const char *hostname)
{
    if (!s_sta_netif || !hostname) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_netif_set_hostname(s_sta_netif, hostname);
}

esp_err_t doorbell_net_connect_sta(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        s_sta_has_creds = false;
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    }

    /* Strongest-signal multi-AP selection (I-35) */
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = (pass && pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    /* Never log passphrase (I-25) */
    ESP_LOGI(TAG, "Configuring STA for SSID '%s'", ssid);

    s_sta_has_creds = true;
    s_backoff_ms = 1000;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_net_evt) {
        xEventGroupClearBits(s_net_evt, NET_BIT_CONNECTED | NET_BIT_FAIL);
    }

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    if (err != ESP_OK) {
        return err;
    }

    if (!s_sta_paused) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t doorbell_net_disconnect_sta(void)
{
    s_sta_has_creds = false;
    s_sta_connected = false;
    s_sta_ipv4_host = 0;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    return esp_wifi_disconnect();
}

void doorbell_net_set_sta_reconnect_paused(bool paused)
{
    s_sta_paused = paused;
    if (paused) {
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
    } else if (s_sta_has_creds && !s_sta_connected) {
        s_backoff_ms = 1000;
        esp_wifi_connect();
    }
}

bool doorbell_net_get_sta_ipv4(uint32_t *out_ipv4_host)
{
    if (out_ipv4_host) {
        *out_ipv4_host = s_sta_ipv4_host;
    }
    return s_sta_connected && (s_sta_ipv4_host != 0);
}

esp_err_t doorbell_net_wait_for_sta_ip(uint32_t timeout_ms)
{
    if (s_sta_connected && s_sta_ipv4_host != 0) {
        return ESP_OK;
    }
    if (!s_net_evt) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_net_evt,
                                           NET_BIT_CONNECTED,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & NET_BIT_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void doorbell_net_get_sta_telemetry(int8_t *out_rssi,
                                    uint8_t *out_channel,
                                    uint8_t out_bssid[6],
                                    uint16_t *out_disconnect_count)
{
    wifi_ap_record_t ap_info = {0};
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (out_rssi) {
            *out_rssi = ap_info.rssi;
        }
        if (out_channel) {
            *out_channel = ap_info.primary;
        }
        if (out_bssid) {
            memcpy(out_bssid, ap_info.bssid, 6);
        }
    } else {
        if (out_rssi) {
            *out_rssi = -127;
        }
        if (out_channel) {
            *out_channel = 0;
        }
        if (out_bssid) {
            memset(out_bssid, 0, 6);
        }
    }
    if (out_disconnect_count) {
        *out_disconnect_count = s_disconnect_count;
    }
}

esp_err_t doorbell_net_get_sta_mac(uint8_t out_mac[6])
{
    return esp_wifi_get_mac(WIFI_IF_STA, out_mac);
}

esp_err_t doorbell_net_get_ap_mac(uint8_t out_mac[6])
{
    return esp_wifi_get_mac(WIFI_IF_AP, out_mac);
}

esp_err_t doorbell_net_start_softap(const char *ssid,
                                    const char *wpa3_sae_pass,
                                    uint8_t channel,
                                    uint32_t *out_ap_ipv4_host)
{
    if (!ssid || ssid[0] == '\0' || !s_ap_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Subnet Collision Check (I-32): if STA subnet is 192.168.4.0/24, switch SoftAP to 10.195.4.1/24 */
    uint32_t check_ip = s_sta_ipv4_host ? s_sta_ipv4_host : s_last_sta_ipv4_host;
    bool collision = ((check_ip & 0xFFFFFF00UL) == 0xC0A80400UL); /* 192.168.4.0/24 */

    esp_netif_ip_info_t ip_info = {0};
    if (collision) {
        IP4_ADDR(&ip_info.ip, 10, 195, 4, 1);
        IP4_ADDR(&ip_info.gw, 10, 195, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        ESP_LOGW(TAG, "STA subnet 192.168.4.0/24 detected — switching SoftAP to 10.195.4.1/24 (I-32)");
    } else {
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    }

    esp_netif_dhcps_stop(s_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    esp_netif_dhcps_start(s_ap_netif);

    if (out_ap_ipv4_host) {
        *out_ap_ipv4_host = ntohl(ip_info.ip.addr);
    }

    /* Determine operating channel: match STA channel if connected */
    uint8_t primary_ch = (channel >= 1 && channel <= 13) ? channel : 1;
    wifi_ap_record_t sta_ap = {0};
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK && sta_ap.primary >= 1) {
        primary_ch = sta_ap.primary;
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel = primary_ch;
    ap_cfg.ap.max_connection = 4; /* Mandatory rule (I-70) */

    if (wpa3_sae_pass && wpa3_sae_pass[0] != '\0') {
        /* Setup Web Portal: WPA3-Personal (SAE with PMF required, I-4) */
        strncpy((char *)ap_cfg.ap.password, wpa3_sae_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA3_PSK;
        ap_cfg.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ap_cfg.ap.pmf_cfg.required = true;
    } else {
        /* Pairing Discovery SoftAP: Open (I-27) */
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Started SoftAP '%s' on channel %u (IP " IPSTR ")",
             ssid, (unsigned)primary_ch, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t doorbell_net_stop_softap(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Stopped SoftAP, returned to WIFI_MODE_STA");
    return err;
}

uint16_t doorbell_net_get_softap_sta_count(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return (uint16_t)sta_list.num;
    }
    return 0;
}

esp_err_t doorbell_net_scan_and_join_pair_ap(char out_ssid[33], uint32_t timeout_ms)
{
    if (!out_ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    out_ssid[0] = '\0';

    doorbell_net_set_sta_reconnect_paused(true);
    esp_wifi_disconnect();

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        doorbell_net_set_sta_reconnect_paused(false);
        return err;
    }

    uint16_t ap_num = 16;
    wifi_ap_record_t ap_records[16];
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (err != ESP_OK || ap_num == 0) {
        doorbell_net_set_sta_reconnect_paused(false);
        return ESP_ERR_NOT_FOUND;
    }

    int found_idx = -1;
    uint16_t match_count = 0;
    for (uint16_t i = 0; i < ap_num; i++) {
        if (strncmp((const char *)ap_records[i].ssid, "Doorbell-Pair-", 14) == 0 &&
            ap_records[i].authmode == WIFI_AUTH_OPEN) {
            if (found_idx < 0) {
                found_idx = (int)i;
            }
            match_count++;
        }
    }

    if (found_idx < 0) {
        doorbell_net_set_sta_reconnect_paused(false);
        return ESP_ERR_NOT_FOUND;
    }
    if (match_count > 1) {
        ESP_LOGW(TAG, "Multiple Doorbell-Pair-* SoftAPs found (%u) — aborting to prevent mis-pairing",
                 (unsigned)match_count);
        doorbell_net_set_sta_reconnect_paused(false);
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(out_ssid, (const char *)ap_records[found_idx].ssid, 32);
    out_ssid[32] = '\0';

    ESP_ERROR_CHECK(doorbell_net_connect_sta(out_ssid, ""));
    doorbell_net_set_sta_reconnect_paused(false);

    EventBits_t bits = xEventGroupWaitBits(s_net_evt,
                                           NET_BIT_CONNECTED,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & NET_BIT_CONNECTED)) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

int doorbell_udp_socket_create(uint16_t port, uint32_t bind_ipv4_host, uint32_t rcvtimeo_ms)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    if (rcvtimeo_ms > 0) {
        struct timeval tv = {
            .tv_sec = (time_t)(rcvtimeo_ms / 1000U),
            .tv_usec = (suseconds_t)((rcvtimeo_ms % 1000U) * 1000U),
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(bind_ipv4_host),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind(port=%u) failed: errno=%d", (unsigned)port, errno);
        close(sock);
        return -1;
    }

    return sock;
}

esp_err_t doorbell_udp_send_direct(int sock,
                                   const uint8_t *buf,
                                   size_t len,
                                   uint32_t dst_ipv4_host,
                                   uint16_t dst_port)
{
    if (sock < 0 || !buf || len == 0 || dst_ipv4_host == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(dst_port),
        .sin_addr.s_addr = htonl(dst_ipv4_host),
    };
    ssize_t sent = sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    return (sent == (ssize_t)len) ? ESP_OK : ESP_FAIL;
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

    /* Attempt 1 (Initial): unicast if peer IP known, else limited broadcast 255.255.255.255 (I-33) */
    if (!is_retry) {
        if (peer_ipv4_host != 0 && peer_ipv4_host != 0xFFFFFFFFUL) {
            doorbell_frame_set_transport_flags(frame, false, false);
            return doorbell_udp_send_direct(sock, frame, frame_len, peer_ipv4_host, port);
        } else {
            doorbell_frame_set_transport_flags(frame, false, true);
            return doorbell_udp_send_direct(sock, frame, frame_len, 0xFFFFFFFFUL, port);
        }
    }

    /* Attempt 2+ (Retry): Dual-path send to BOTH last known unicast IP AND limited broadcast 255.255.255.255 (I-33) */
    esp_err_t err1 = ESP_OK;
    if (peer_ipv4_host != 0 && peer_ipv4_host != 0xFFFFFFFFUL) {
        doorbell_frame_set_transport_flags(frame, true, false);
        err1 = doorbell_udp_send_direct(sock, frame, frame_len, peer_ipv4_host, port);
    }

    doorbell_frame_set_transport_flags(frame, true, true);
    esp_err_t err2 = doorbell_udp_send_direct(sock, frame, frame_len, 0xFFFFFFFFUL, port);

    return (err1 == ESP_OK || err2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}
