#include "doorbell_net.h"
#include "esp_netif.h"
#include "doorbell_config_checks.h"
#include "doorbell_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "doorbell_net";

#define NET_BIT_CONNECTED  BIT0
#define NET_BIT_FAIL       BIT1
#define NET_BIT_AP_STARTED BIT2 /* SoftAP running; its lwIP netif is added and up (set after esp_netif's own handler) */

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static EventGroupHandle_t s_net_evt = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

static doorbell_net_state_cb_t s_state_cb = NULL;
static void *s_state_cb_ctx = NULL;

static _Atomic bool s_sta_has_creds = false;
static _Atomic bool s_sta_paused = false;
static _Atomic bool s_sta_connected = false;
static _Atomic uint32_t s_sta_ipv4_host = 0;
static _Atomic uint32_t s_last_sta_ipv4_host = 0;
static _Atomic uint32_t s_backoff_ms = 1000;
static _Atomic uint16_t s_disconnect_count = 0;

/* Asynchronous scan for the setup portal network list. Only scans started by doorbell_net_scan_start_async() are
 * collected (s_scan_pending), so the blocking pairing scan keeps reading its own records. */
#define SCAN_RECORDS_MAX 32
static SemaphoreHandle_t s_scan_lock = NULL;
static _Atomic bool s_scan_pending = false;
static _Atomic int64_t s_scan_started_us = 0;
/* A scan whose SCAN_DONE never arrived (e.g. mode change) no longer blocks a new one after this time */
#define SCAN_STALE_US (10 * 1000000LL)
static uint32_t s_scan_gen = 0;
static size_t s_scan_count = 0;
static doorbell_net_ap_info_t s_scan_results[DOORBELL_NET_SCAN_MAX];
static doorbell_net_ap_info_t s_scan_tmp[DOORBELL_NET_SCAN_MAX]; /* event task only (keeps its stack small) */

static void scan_collect_results(void)
{
    uint16_t n = 0;
    size_t count = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > SCAN_RECORDS_MAX) {
        n = SCAN_RECORDS_MAX;
    }
    wifi_ap_record_t *recs = (n > 0) ? calloc(n, sizeof(*recs)) : NULL;
    if (recs == NULL || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        esp_wifi_clear_ap_list(); /* frees the driver's list in every path */
        n = 0;
    }

    /* Strongest first, so the first record of each SSID is its best BSS */
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = recs[i];
        int j = (int)i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }

    for (uint16_t i = 0; i < n; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue; /* hidden network: the form's manual entry covers it */
        }
        size_t k = 0;
        while (k < count && memcmp(s_scan_tmp[k].ssid, recs[i].ssid, sizeof(recs[i].ssid)) != 0) {
            k++;
        }
        if (k < count) {
            s_scan_tmp[k].supports_11b = s_scan_tmp[k].supports_11b || recs[i].phy_11b;
            continue;
        }
        if (count == DOORBELL_NET_SCAN_MAX) {
            continue;
        }
        memcpy(s_scan_tmp[count].ssid, recs[i].ssid, sizeof(s_scan_tmp[count].ssid));
        s_scan_tmp[count].ssid[sizeof(s_scan_tmp[count].ssid) - 1] = '\0';
        s_scan_tmp[count].rssi = recs[i].rssi;
        s_scan_tmp[count].secured = (recs[i].authmode != WIFI_AUTH_OPEN);
        s_scan_tmp[count].supports_11b = recs[i].phy_11b;
        count++;
    }
    free(recs);

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    memcpy(s_scan_results, s_scan_tmp, count * sizeof(s_scan_tmp[0]));
    s_scan_count = count;
    s_scan_gen = (s_scan_gen == UINT32_MAX) ? 1 : s_scan_gen + 1;
    s_scan_pending = false;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "Network scan finished: %u network(s)", (unsigned)count);
}

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
        } else if (event_id == WIFI_EVENT_AP_START) {
            /* esp_netif_create_default_wifi_ap() registered its AP_START handler (which adds and raises the lwIP
             * netif) before ours, so by now the SoftAP netif has its final name and index */
            if (s_net_evt) {
                xEventGroupSetBits(s_net_evt, NET_BIT_AP_STARTED);
            }
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            if (s_net_evt) {
                xEventGroupClearBits(s_net_evt, NET_BIT_AP_STARTED);
            }
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {
            if (s_scan_pending && s_scan_lock) {
                scan_collect_results();
            }
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

/* Optional 802.11b-only station (CONFIG_DOORBELL_WIFI_STA_11B_ONLY). Called after every esp_wifi_set_mode();
 * it only writes the protocol when the driver reports a different bitmap, so an already-correct station
 * interface is never touched while connected. */
static void apply_sta_protocol(void)
{
#if CONFIG_DOORBELL_WIFI_STA_11B_ONLY
    uint8_t current = 0;
    if (esp_wifi_get_protocol(WIFI_IF_STA, &current) == ESP_OK && current == WIFI_PROTOCOL_11B) {
        return;
    }
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Station restricted to 802.11b (was protocol bitmap 0x%02x)", (unsigned)current);
    } else {
        ESP_LOGW(TAG, "Failed to restrict station to 802.11b: %s", esp_err_to_name(err));
    }
#endif
}

esp_err_t doorbell_net_init(const char *hostname, doorbell_net_state_cb_t cb, void *ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = ctx;

    s_net_evt = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    if (!s_net_evt || !s_scan_lock) {
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
    apply_sta_protocol();
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

    /* Drop the old association state synchronously. The WIFI_EVENT_STA_DISCONNECTED event for the old link arrives
     * asynchronously, so without this doorbell_net_wait_for_sta_ip() would report the OLD connection as success and
     * the portal would commit untested credentials (I-40). */
    bool was_connected = s_sta_connected;
    s_sta_connected = false;
    s_sta_ipv4_host = 0;
    if (s_net_evt) {
        xEventGroupClearBits(s_net_evt, NET_BIT_CONNECTED | NET_BIT_FAIL);
    }
    if (was_connected && s_state_cb) {
        s_state_cb(false, 0, s_state_cb_ctx);
    }

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    memset_explicit(&wifi_cfg, 0, sizeof(wifi_cfg));
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

    xEventGroupClearBits(s_net_evt, NET_BIT_AP_STARTED);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    apply_sta_protocol();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    memset_explicit(&ap_cfg, 0, sizeof(ap_cfg));
    if (err != ESP_OK) {
        return err;
    }

    /* Callers bind sockets to this netif (SO_BINDTODEVICE, I-27/I-47) right after we return, so wait until the
     * SoftAP has started and its netif exists. Bounded well below the 5 s TWDT (I-46). */
    EventBits_t bits = xEventGroupWaitBits(s_net_evt, NET_BIT_AP_STARTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    if (!(bits & NET_BIT_AP_STARTED) || !esp_netif_is_netif_up(s_ap_netif)) {
        ESP_LOGE(TAG, "SoftAP '%s' did not start within 2 s", ssid);
        doorbell_net_stop_softap();
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Started SoftAP '%s' on channel %u (IP " IPSTR ")",
             ssid, (unsigned)primary_ch, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t doorbell_net_stop_softap(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    apply_sta_protocol();
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

esp_err_t doorbell_net_scan_start_async(void)
{
    if (!s_scan_lock || (s_scan_pending && esp_timer_get_time() - s_scan_started_us < SCAN_STALE_US)) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* Back to the home (SoftAP) channel between scanned channels so the installer's phone stays connected */
        .home_chan_dwell_time = WIFI_SCAN_HOME_CHANNEL_DWELL_DEFAULT_TIME,
    };
    s_scan_started_us = esp_timer_get_time();
    s_scan_pending = true;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        s_scan_pending = false;
        ESP_LOGW(TAG, "Network scan not started: %s", esp_err_to_name(err));
    }
    return err;
}

size_t doorbell_net_get_scan_results(doorbell_net_ap_info_t *out, size_t max, uint32_t *out_gen, bool *out_scanning)
{
    size_t n = 0;
    if (!s_scan_lock) {
        if (out_gen) {
            *out_gen = 0;
        }
        if (out_scanning) {
            *out_scanning = false;
        }
        return 0;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    n = (s_scan_count < max) ? s_scan_count : max;
    if (out && n > 0) {
        memcpy(out, s_scan_results, n * sizeof(out[0]));
    }
    if (out_gen) {
        *out_gen = s_scan_gen;
    }
    if (out_scanning) {
        *out_scanning = s_scan_pending && (esp_timer_get_time() - s_scan_started_us < SCAN_STALE_US);
    }
    xSemaphoreGive(s_scan_lock);
    return n;
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

    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
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

    err = doorbell_net_connect_sta(out_ssid, "");
    if (err != ESP_OK) {
        doorbell_net_set_sta_reconnect_paused(false);
        return err;
    }
    doorbell_net_set_sta_reconnect_paused(false);

    /* Wait in 250 ms bounded slices so TWDT-subscribed callers never block past the 5 s watchdog timeout (I-46) */
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        EventBits_t bits = xEventGroupWaitBits(s_net_evt,
                                               NET_BIT_CONNECTED,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(250));
        if (bits & NET_BIT_CONNECTED) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
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

esp_err_t doorbell_net_get_softap_if_name(char out_name[6])
{
    if (!s_ap_netif || !out_name || !esp_netif_is_netif_up(s_ap_netif)) {
        return ESP_ERR_INVALID_STATE;
    }
    out_name[0] = '\0';
    esp_err_t err = esp_netif_get_netif_impl_name(s_ap_netif, out_name);
    if (err == ESP_OK && out_name[0] == '\0') {
        /* An empty name would make SO_BINDTODEVICE unbind and httpd skip binding: fail closed instead */
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t doorbell_net_bind_socket_to_softap(int sock)
{
    struct ifreq ifr = {0};
    esp_err_t err = doorbell_net_get_softap_if_name(ifr.ifr_name);
    if (err != ESP_OK) {
        return err;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        ESP_LOGE(TAG, "SO_BINDTODEVICE(%s) failed: errno=%d", ifr.ifr_name, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
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
