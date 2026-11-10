#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*doorbell_net_state_cb_t)(bool connected, uint32_t ipv4_host, void *ctx);

/**
 * @brief Initialize Wi-Fi (RAM storage, PS_NONE), TCP/IP stack, RF entropy, and PSA Crypto.
 */
esp_err_t doorbell_net_init(const char *hostname, doorbell_net_state_cb_t cb, void *ctx);

/**
 * @brief Update STA network hostname dynamically (e.g. after apartment pairing).
 */
esp_err_t doorbell_net_set_hostname(const char *hostname);

/**
 * @brief Configure STA credentials and start connecting (never logs passphrase per I-25).
 */
esp_err_t doorbell_net_connect_sta(const char *ssid, const char *pass);

/**
 * @brief Disconnect STA and stop auto-reconnect.
 */
esp_err_t doorbell_net_disconnect_sta(void);

/**
 * @brief Pause or resume background STA auto-reconnect (Single-Radio Rule I-31).
 */
void doorbell_net_set_sta_reconnect_paused(bool paused);

/**
 * @brief Query current STA connection state and IPv4 address (host byte order).
 */
bool doorbell_net_get_sta_ipv4(uint32_t *out_ipv4_host);

/**
 * @brief Block until STA obtains an IPv4 lease or timeout_ms expires.
 */
esp_err_t doorbell_net_wait_for_sta_ip(uint32_t timeout_ms);

/**
 * @brief Query current STA channel, RSSI, BSSID, and total disconnect count.
 */
void doorbell_net_get_sta_telemetry(int8_t *out_rssi,
                                    uint8_t *out_channel,
                                    uint8_t out_bssid[6],
                                    uint16_t *out_disconnect_count);

/**
 * @brief Get local STA and SoftAP MAC addresses.
 */
esp_err_t doorbell_net_get_sta_mac(uint8_t out_mac[6]);
esp_err_t doorbell_net_get_ap_mac(uint8_t out_mac[6]);

/**
 * @brief Start temporary SoftAP (Open for pairing discovery, or WPA3-SAE for Setup Portal).
 *        Automatically avoids 192.168.4.0/24 subnet collision by switching to 10.195.4.1/24 (I-32).
 */
esp_err_t doorbell_net_start_softap(const char *ssid,
                                    const char *wpa3_sae_pass,
                                    uint8_t channel,
                                    uint32_t *out_ap_ipv4_host);

/**
 * @brief Stop SoftAP and return to WIFI_MODE_STA.
 */
esp_err_t doorbell_net_stop_softap(void);

/**
 * @brief Get the number of stations currently associated with the Panel SoftAP (for collision detection).
 */
uint16_t doorbell_net_get_softap_sta_count(void);

/**
 * @brief Scan for an open Pairing SoftAP matching prefix "Doorbell-Pair-" and connect STA to it.
 */
esp_err_t doorbell_net_scan_and_join_pair_ap(char out_ssid[33], uint32_t timeout_ms);

/** Maximum number of distinct networks kept from one asynchronous scan */
#define DOORBELL_NET_SCAN_MAX 15

/** One network (SSID) from the asynchronous scan, strongest BSS of that SSID */
typedef struct {
    char ssid[33];      /**< NUL-terminated SSID as received (may be non-UTF-8) */
    int8_t rssi;        /**< dBm of the strongest BSS with this SSID */
    bool secured;       /**< authmode != OPEN */
    bool supports_11b;  /**< some BSS advertises 802.11b rates (needed with CONFIG_DOORBELL_WIFI_STA_11B_ONLY) */
} doorbell_net_ap_info_t;

/**
 * @brief Start a non-blocking scan (setup portal network list). Results arrive via WIFI_EVENT_SCAN_DONE and are cached;
 *        hidden SSIDs are skipped, duplicates merged, entries sorted by signal strength. The scan returns to the home
 *        channel between channels so SoftAP clients stay connected.
 * @return ESP_OK, ESP_ERR_INVALID_STATE if a scan is already running, or the esp_wifi_scan_start() error.
 */
esp_err_t doorbell_net_scan_start_async(void);

/**
 * @brief Copy the cached scan results.
 * @param[out] out          Array of at least @p max entries.
 * @param[out] out_gen      Result generation, incremented on every completed scan (0 = no scan finished yet).
 * @param[out] out_scanning true while a scan is still running.
 * @return number of entries written.
 */
size_t doorbell_net_get_scan_results(doorbell_net_ap_info_t *out, size_t max, uint32_t *out_gen, bool *out_scanning);

/**
 * @brief Create a UDP socket with SO_BROADCAST and bounded SO_RCVTIMEO (I-46).
 *
 * @param port UDP port to bind (host byte order, 0 for ephemeral).
 * @param bind_ipv4_host Specific interface IPv4 to bind (host byte order, 0 for INADDR_ANY).
 * @param rcvtimeo_ms Receive timeout in milliseconds (must be <= 2000 ms for TWDT).
 * @return Socket fd >= 0 on success, -1 on error.
 */
int doorbell_udp_socket_create(uint16_t port, uint32_t bind_ipv4_host, uint32_t rcvtimeo_ms);

/**
 * @brief Get the lwIP interface name of the SoftAP netif (for SO_BINDTODEVICE / httpd_config_t.if_name).
 *
 * @param[out] out_name Buffer of at least 6 bytes (lwIP NETIF_NAMESIZE).
 */
esp_err_t doorbell_net_get_softap_if_name(char out_name[6]);

/**
 * @brief Bind a socket to the SoftAP interface with SO_BINDTODEVICE (I-27, I-47).
 *
 * An IP-only bind is not enough: lwIP uses the weak host model and accepts a packet for the SoftAP address that
 * arrives on the STA interface. A device-bound pcb drops anything not received on the SoftAP netif.
 */
esp_err_t doorbell_net_bind_socket_to_softap(int sock);

/**
 * @brief Send an encrypted protocol frame using unicast on Attempt 1 and dual-path
 *        (unicast + 255.255.255.255 broadcast) on retries (I-33).
 */
esp_err_t doorbell_udp_send_frame(int sock,
                                  uint8_t *frame,
                                  size_t frame_len,
                                  uint32_t peer_ipv4_host,
                                  uint16_t port,
                                  bool is_retry);

/**
 * @brief Send a raw UDP datagram directly to a specific IPv4 address (host byte order).
 */
esp_err_t doorbell_udp_send_direct(int sock,
                                   const uint8_t *buf,
                                   size_t len,
                                   uint32_t dst_ipv4_host,
                                   uint16_t dst_port);

#ifdef __cplusplus
}
#endif
