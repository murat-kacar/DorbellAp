#include "doorbell_storage.h"
#include "doorbell_config_checks.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "esp_log.h"

static const char *TAG = "doorbell_storage";

#define PANEL_CFG_MAGIC  0x44425043UL /* 'DBPC' */
#define PANEL_APT_MAGIC  0x44425041UL /* 'DBPA' */
#define UNIT_STATE_MAGIC 0x44425553UL /* 'DBUS' */
#define BLOB_VERSION     1U

_Static_assert(sizeof(doorbell_panel_cfg_blob_t) == 168, "doorbell_panel_cfg_blob_t must be 168 bytes");
_Static_assert(sizeof(doorbell_panel_apt_blob_t) == 48,  "doorbell_panel_apt_blob_t must be 48 bytes");
_Static_assert(sizeof(doorbell_unit_state_blob_t) == 152, "doorbell_unit_state_blob_t must be 152 bytes");

static uint32_t compute_crc(const void *data, size_t len_without_crc)
{
    return esp_rom_crc32_le(0, (const uint8_t *)data, len_without_crc);
}

esp_err_t doorbell_storage_init(void)
{
    /* Strictly forbid nvs_flash_erase() on boot errors per I-13 */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CRITICAL: NVS flash init failed (%s) — auto-erase is forbidden by I-13",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ---- Panel Storage Implementation ---- */

esp_err_t doorbell_storage_panel_load_cfg(doorbell_panel_cfg_blob_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t sz = sizeof(*out_cfg);
    err = nvs_get_blob(h, "panel_cfg", out_cfg, &sz);
    nvs_close(h);

    if (err == ESP_OK && sz == sizeof(*out_cfg)) {
        uint32_t expected_crc = compute_crc(out_cfg, offsetof(doorbell_panel_cfg_blob_t, crc32));
        if (out_cfg->magic == PANEL_CFG_MAGIC &&
            out_cfg->version == BLOB_VERSION &&
            out_cfg->crc32 == expected_crc) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "panel_cfg blob CRC/magic mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot on blank NVS: initialize from Kconfig defaults */
        memset(out_cfg, 0, sizeof(*out_cfg));
        out_cfg->magic = PANEL_CFG_MAGIC;
        out_cfg->version = BLOB_VERSION;
#if defined(CONFIG_DOORBELL_ROLE_PANEL)
        strncpy(out_cfg->wifi_ssid, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_SSID, sizeof(out_cfg->wifi_ssid) - 1);
        strncpy(out_cfg->wifi_pass, CONFIG_DOORBELL_PANEL_DEFAULT_WIFI_PASS, sizeof(out_cfg->wifi_pass) - 1);
        strncpy(out_cfg->admin_code, CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE, sizeof(out_cfg->admin_code) - 1);
        strncpy(out_cfg->user_code, CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE, sizeof(out_cfg->user_code) - 1);
        strncpy(out_cfg->factory_admin_code, CONFIG_DOORBELL_PANEL_DEFAULT_ADMIN_CODE,
                sizeof(out_cfg->factory_admin_code) - 1);
        strncpy(out_cfg->factory_user_code, CONFIG_DOORBELL_PANEL_DEFAULT_USER_CODE,
                sizeof(out_cfg->factory_user_code) - 1);
#endif
        ESP_LOGI(TAG, "Initializing default panel_cfg record in NVS");
        return doorbell_storage_panel_save_cfg(out_cfg);
    }

    return err;
}

esp_err_t doorbell_storage_panel_save_cfg(doorbell_panel_cfg_blob_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg->magic = PANEL_CFG_MAGIC;
    cfg->version = BLOB_VERSION;
    cfg->crc32 = compute_crc(cfg, offsetof(doorbell_panel_cfg_blob_t, crc32));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "panel_cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_panel_reset_codes_to_factory(doorbell_panel_cfg_blob_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cfg->admin_code, cfg->factory_admin_code, sizeof(cfg->admin_code));
    memcpy(cfg->user_code, cfg->factory_user_code, sizeof(cfg->user_code));
    cfg->wrong_code_count = 0;
    cfg->lockout_level = 0;
    cfg->lockout_remaining_s = 0;
    return doorbell_storage_panel_save_cfg(cfg);
}

static void format_apt_key(uint8_t apt_no, char out_key[12])
{
    snprintf(out_key, 12, "apt_%03u", (unsigned)apt_no);
}

esp_err_t doorbell_storage_panel_load_apt(uint8_t apt_no, doorbell_panel_apt_blob_t *out_apt)
{
    if (!out_apt || apt_no == 0 || apt_no >= DOORBELL_MAX_APT_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_apt, 0, sizeof(*out_apt));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[12];
    format_apt_key(apt_no, key);
    size_t sz = sizeof(*out_apt);
    err = nvs_get_blob(h, key, out_apt, &sz);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    if (sz != sizeof(*out_apt)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t expected_crc = compute_crc(out_apt, offsetof(doorbell_panel_apt_blob_t, crc32));
    if (out_apt->magic != PANEL_APT_MAGIC ||
        out_apt->version != BLOB_VERSION ||
        out_apt->apt_no != apt_no ||
        out_apt->crc32 != expected_crc) {
        ESP_LOGE(TAG, "Corrupted NVS record for apartment %u", (unsigned)apt_no);
        memset(out_apt, 0, sizeof(*out_apt));
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

esp_err_t doorbell_storage_panel_save_apt(doorbell_panel_apt_blob_t *apt)
{
    if (!apt || apt->apt_no == 0 || apt->apt_no >= DOORBELL_MAX_APT_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    apt->magic = PANEL_APT_MAGIC;
    apt->version = BLOB_VERSION;
    apt->crc32 = compute_crc(apt, offsetof(doorbell_panel_apt_blob_t, crc32));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[12];
    format_apt_key(apt->apt_no, key);
    err = nvs_set_blob(h, key, apt, sizeof(*apt));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_panel_erase_apt(uint8_t apt_no)
{
    if (apt_no == 0 || apt_no >= DOORBELL_MAX_APT_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[12];
    format_apt_key(apt_no, key);
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_panel_erase_all_apts(uint8_t min_apt, uint8_t max_apt)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (uint8_t apt = min_apt; apt <= max_apt; apt++) {
        char key[12];
        format_apt_key(apt, key);
        nvs_erase_key(h, key);
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_panel_next_tx_counter(doorbell_panel_apt_blob_t *apt,
                                                 uint32_t *in_out_runtime_counter,
                                                 uint32_t block_size,
                                                 uint32_t *out_counter)
{
    if (!apt || !in_out_runtime_counter || !out_counter || block_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*in_out_runtime_counter >= DOORBELL_COUNTER_MAX_VALID) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t next = *in_out_runtime_counter + 1U;
    if (next > apt->tx_counter_ceiling) {
        /* Write-ahead rule (I-50): reserve next block in NVS BEFORE using any counter in that block */
        uint64_t new_ceiling = (uint64_t)next + (uint64_t)block_size - 1ULL;
        if (new_ceiling > DOORBELL_COUNTER_MAX_VALID) {
            new_ceiling = DOORBELL_COUNTER_MAX_VALID;
        }
        uint32_t prev_ceiling = apt->tx_counter_ceiling;
        apt->tx_counter_ceiling = (uint32_t)new_ceiling;
        esp_err_t err = doorbell_storage_panel_save_apt(apt);
        if (err != ESP_OK) {
            apt->tx_counter_ceiling = prev_ceiling;
            ESP_LOGE(TAG, "Failed to commit write-ahead tx_counter for apt %u: %s",
                     (unsigned)apt->apt_no, esp_err_to_name(err));
            return err;
        }
    }

    *in_out_runtime_counter = next;
    *out_counter = next;
    return ESP_OK;
}

/* ---- Apartment Module Storage Implementation ---- */

esp_err_t doorbell_storage_unit_load(doorbell_unit_state_blob_t *out_state)
{
    if (!out_state) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t sz = sizeof(*out_state);
    err = nvs_get_blob(h, "unit_pair", out_state, &sz);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    if (sz != sizeof(*out_state)) {
        memset(out_state, 0, sizeof(*out_state));
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t expected_crc = compute_crc(out_state, offsetof(doorbell_unit_state_blob_t, crc32));
    if (out_state->magic != UNIT_STATE_MAGIC ||
        out_state->version != BLOB_VERSION ||
        out_state->crc32 != expected_crc) {
        ESP_LOGE(TAG, "unit_pair blob CRC/magic mismatch");
        memset(out_state, 0, sizeof(*out_state));
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

esp_err_t doorbell_storage_unit_save(doorbell_unit_state_blob_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    state->magic = UNIT_STATE_MAGIC;
    state->version = BLOB_VERSION;
    state->crc32 = compute_crc(state, offsetof(doorbell_unit_state_blob_t, crc32));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "unit_pair", state, sizeof(*state));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_unit_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, "unit_pair");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t doorbell_storage_unit_next_tx_counter(doorbell_unit_state_blob_t *state,
                                                uint32_t *in_out_runtime_counter,
                                                uint32_t block_size,
                                                uint32_t *out_counter)
{
    if (!state || !in_out_runtime_counter || !out_counter || block_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*in_out_runtime_counter >= DOORBELL_COUNTER_MAX_VALID) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t next = *in_out_runtime_counter + 1U;
    if (next > state->tx_counter_ceiling) {
        uint64_t new_ceiling = (uint64_t)next + (uint64_t)block_size - 1ULL;
        if (new_ceiling > DOORBELL_COUNTER_MAX_VALID) {
            new_ceiling = DOORBELL_COUNTER_MAX_VALID;
        }
        uint32_t prev_ceiling = state->tx_counter_ceiling;
        state->tx_counter_ceiling = (uint32_t)new_ceiling;
        esp_err_t err = doorbell_storage_unit_save(state);
        if (err != ESP_OK) {
            state->tx_counter_ceiling = prev_ceiling;
            ESP_LOGE(TAG, "Failed to commit write-ahead tx_counter on unit: %s", esp_err_to_name(err));
            return err;
        }
    }

    *in_out_runtime_counter = next;
    *out_counter = next;
    return ESP_OK;
}

