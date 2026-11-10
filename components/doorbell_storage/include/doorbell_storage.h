#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "doorbell_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOORBELL_NVS_NAMESPACE "doorbell"
#define DOORBELL_MAX_APT_SLOTS  100 /* 1..99 */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t pad[2];
    char admin_code[12];
    char user_code[12];
    char factory_admin_code[12];
    char factory_user_code[12];
    uint16_t wrong_code_count;
    uint16_t lockout_level;
    uint32_t lockout_remaining_s;
    uint32_t crc32;
} doorbell_panel_cfg_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t apt_no;
    uint8_t registered;
    uint8_t unit_mac[6];
    uint8_t reserved[2];
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    uint32_t last_ipv4;
    uint32_t tx_counter_ceiling;
    uint32_t rx_floor;
    uint32_t crc32;
} doorbell_panel_apt_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t paired;
    uint8_t apt_no;
    uint8_t panel_mac[6];
    uint8_t ap_channel;
    uint8_t reserved;
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t pad[2];
    uint32_t panel_ipv4;
    uint8_t k_apt[DOORBELL_KEY_SIZE];
    uint16_t heartbeat_interval_s;
    uint16_t pad2;
    uint32_t tx_counter_ceiling;
    uint32_t rx_floor;
    uint32_t crc32;
} doorbell_unit_state_blob_t;

/**
 * @brief Initialize NVS flash without auto-erasing on error (I-13).
 */
esp_err_t doorbell_storage_init(void);

/* ---- Panel Storage API ---- */
esp_err_t doorbell_storage_panel_load_cfg(doorbell_panel_cfg_blob_t *out_cfg);
esp_err_t doorbell_storage_panel_save_cfg(doorbell_panel_cfg_blob_t *cfg);
/** Building Wi-Fi as provisioned by the flasher (Burn / Flash), kept apart from the editable panel_cfg copy */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t pad[2];
    uint32_t crc32;
} doorbell_panel_factory_wifi_blob_t;

/** Read the factory Wi-Fi record (ESP_ERR_NVS_NOT_FOUND if none was recorded yet). */
esp_err_t doorbell_storage_panel_load_factory_wifi(doorbell_panel_factory_wifi_blob_t *out);

/**
 * Record @p cfg's Wi-Fi as the factory Wi-Fi if no record exists yet (first boot after provisioning). Never overwrites
 * an existing or unreadable record. Call only with a panel_cfg that was read from NVS.
 */
esp_err_t doorbell_storage_panel_snapshot_factory_wifi(const doorbell_panel_cfg_blob_t *cfg);

/**
 * Hidden-button factory reset: admin / user codes to the factory codes, building Wi-Fi to the factory Wi-Fi, lockout
 * cleared (committed to NVS, then to @p cfg), then every apartment registration in [min_apt, max_apt] erased.
 * Refuses without changing anything if the factory Wi-Fi record is unavailable or panel_cfg cannot be saved.
 * @param[out] out_nvs_changed true once panel_cfg was written: the caller must then reload its state from NVS
 *                             (restart), also when an error is returned because a registration could not be erased.
 */
esp_err_t doorbell_storage_panel_factory_reset(doorbell_panel_cfg_blob_t *cfg, uint8_t min_apt, uint8_t max_apt,
                                               bool *out_nvs_changed);

esp_err_t doorbell_storage_panel_load_apt(uint8_t apt_no, doorbell_panel_apt_blob_t *out_apt);
esp_err_t doorbell_storage_panel_save_apt(doorbell_panel_apt_blob_t *apt);
esp_err_t doorbell_storage_panel_erase_apt(uint8_t apt_no);
/** Erase every registration in [min_apt, max_apt]; continues past failures and returns the first one. */
esp_err_t doorbell_storage_panel_erase_all_apts(uint8_t min_apt, uint8_t max_apt);

/**
 * @brief RFC 8613 write-ahead counter allocation for Panel -> Unit frames (I-50).
 *        Commits a new +block ceiling to NVS before returning any counter above the current ceiling.
 */
esp_err_t doorbell_storage_panel_next_tx_counter(doorbell_panel_apt_blob_t *apt,
                                                 uint32_t *in_out_runtime_counter,
                                                 uint32_t block_size,
                                                 uint32_t *out_counter);

/* ---- Apartment Module Storage API ---- */
esp_err_t doorbell_storage_unit_load(doorbell_unit_state_blob_t *out_state);
esp_err_t doorbell_storage_unit_save(doorbell_unit_state_blob_t *state);
esp_err_t doorbell_storage_unit_erase(void);

/**
 * @brief RFC 8613 write-ahead counter allocation for Unit -> Panel frames (I-50).
 */
esp_err_t doorbell_storage_unit_next_tx_counter(doorbell_unit_state_blob_t *state,
                                                uint32_t *in_out_runtime_counter,
                                                uint32_t block_size,
                                                uint32_t *out_counter);

#ifdef __cplusplus
}
#endif

