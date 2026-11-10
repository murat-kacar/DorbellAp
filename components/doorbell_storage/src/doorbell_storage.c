#include "doorbell_storage.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "doorbell_storage";

#define PANEL_CFG_MAGIC  0x44425043UL /* 'DBPC' */
#define PANEL_APT_MAGIC  0x44425041UL /* 'DBPA' */
#define UNIT_STATE_MAGIC 0x44425553UL /* 'DBUS' */
#define FACTORY_WIFI_MAGIC 0x44424657UL /* 'DBFW' */
#define BLOB_VERSION     1U

static_assert(sizeof(doorbell_panel_cfg_blob_t) == 168, "doorbell_panel_cfg_blob_t must be 168 bytes");
static_assert(sizeof(doorbell_panel_factory_wifi_blob_t) == 112, "factory_wifi blob must be 112 bytes");
static_assert(sizeof(doorbell_panel_apt_blob_t) == 48,  "doorbell_panel_apt_blob_t must be 48 bytes");
static_assert(sizeof(doorbell_unit_state_blob_t) == 152, "doorbell_unit_state_blob_t must be 152 bytes");

/* Set when an existing panel_cfg blob could not be validated (CRC/magic/version/size). The panel then runs on in-RAM
 * defaults, and saving would overwrite the stored record - including its unique per-device factory codes (I-5) - with
 * the shared built-in ones. Keep the stored blob intact instead (I-13 spirit); recovery is a full re-flash with the
 * flasher (I-14). */
static bool s_panel_cfg_write_protected = false;

static uint32_t compute_crc(const void *data, size_t len_without_crc)
{
    return esp_rom_crc32_le(0, (const uint8_t *)data, len_without_crc);
}

#if CONFIG_NVS_ENCRYPTION
/* Burn profile (ADR 0015): nvs_flash_init() regenerates the NVS keys on ANY key-partition read error, which would
 * orphan the provisioned (encrypted) NVS - an implicit erase, forbidden by I-13. Keys are generated only for a blank
 * nvs_keys partition (first boot of an apartment module); a corrupt key partition fails closed. */
static esp_err_t storage_secure_nvs_init(void)
{
    nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
    nvs_sec_cfg_t cfg = {0};
    esp_err_t err = nvs_flash_read_security_cfg_v2(scheme, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS key partition is blank: generating device NVS encryption keys");
        err = nvs_flash_generate_keys_v2(scheme, &cfg);
    }
    if (err == ESP_OK) {
        err = nvs_flash_secure_init(&cfg);
    } else {
        ESP_LOGE(TAG, "NVS key partition unusable (%s); keys are never regenerated over it", esp_err_to_name(err));
    }
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    return err;
}
#endif

esp_err_t doorbell_storage_init(void)
{
    /* Strictly forbid nvs_flash_erase() on boot errors per I-13 */
#if CONFIG_NVS_ENCRYPTION
    esp_err_t err = storage_secure_nvs_init();
#else
    esp_err_t err = nvs_flash_init();
#endif
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
        s_panel_cfg_write_protected = true;
        return ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* nvs_get_blob() succeeds for a stored blob shorter than the buffer and fails with INVALID_LENGTH for a longer
         * one; never accept a partial record */
        ESP_LOGE(TAG, "panel_cfg blob size mismatch (stored size differs from %u bytes)", (unsigned)sizeof(*out_cfg));
        s_panel_cfg_write_protected = true;
        return ESP_ERR_INVALID_SIZE;
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
        /* Codes are not logged (I-25); this only flags that the shared built-in codes are active (I-5) */
        ESP_LOGW(TAG, "Blank NVS: initializing panel_cfg with built-in default codes. "
                      "Provision unique per-device codes (flasher 'Flash' with the card's code fields) "
                      "before deployment.");
        return doorbell_storage_panel_save_cfg(out_cfg);
    }

    return err;
}

esp_err_t doorbell_storage_panel_save_cfg(doorbell_panel_cfg_blob_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_panel_cfg_write_protected) {
        ESP_LOGW(TAG, "panel_cfg save refused: stored record is unreadable and kept intact "
                      "(re-flash the panel with the flasher)");
        return ESP_ERR_INVALID_STATE;
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

/* ---- Factory building Wi-Fi (the values provisioned by the flasher: Flash / Burn) ----
 * panel_cfg's Wi-Fi fields change with the portal / console, so the provisioned values are kept in their own record.
 * It is written once, on the first boot after provisioning (the flasher erases the chip and writes a whole NVS image,
 * so the record is absent exactly then), and only read afterwards by the hidden-button factory reset. */

esp_err_t doorbell_storage_panel_load_factory_wifi(doorbell_panel_factory_wifi_blob_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(*out);
    err = nvs_get_blob(h, "factory_wifi", out, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || (err == ESP_OK && sz != sizeof(*out))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out->magic != FACTORY_WIFI_MAGIC || out->version != BLOB_VERSION ||
        out->crc32 != compute_crc(out, offsetof(doorbell_panel_factory_wifi_blob_t, crc32))) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_CRC;
    }
    out->wifi_ssid[sizeof(out->wifi_ssid) - 1] = '\0';
    out->wifi_pass[sizeof(out->wifi_pass) - 1] = '\0';
    return ESP_OK;
}

esp_err_t doorbell_storage_panel_snapshot_factory_wifi(const doorbell_panel_cfg_blob_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    doorbell_panel_factory_wifi_blob_t blob;
    esp_err_t err = doorbell_storage_panel_load_factory_wifi(&blob);
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err; /* ESP_OK: already recorded; anything else: never overwrite an unreadable record */
    }
    blob.magic = FACTORY_WIFI_MAGIC;
    blob.version = BLOB_VERSION;
    memcpy(blob.wifi_ssid, cfg->wifi_ssid, sizeof(blob.wifi_ssid));
    memcpy(blob.wifi_pass, cfg->wifi_pass, sizeof(blob.wifi_pass));
    blob.crc32 = compute_crc(&blob, offsetof(doorbell_panel_factory_wifi_blob_t, crc32));

    nvs_handle_t h;
    err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "factory_wifi", &blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Recorded the provisioned building Wi-Fi as factory Wi-Fi (SSID '%s')", cfg->wifi_ssid);
    }
    return err;
}

esp_err_t doorbell_storage_panel_factory_reset(doorbell_panel_cfg_blob_t *cfg, uint8_t min_apt, uint8_t max_apt,
                                               bool *out_nvs_changed)
{
    if (!cfg || !out_nvs_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_nvs_changed = false;
    doorbell_panel_factory_wifi_blob_t fw;
    esp_err_t fw_err = doorbell_storage_panel_load_factory_wifi(&fw);
    if (fw_err != ESP_OK) {
        /* Without a readable factory Wi-Fi record the reset would invent one: refuse, nothing is changed */
        ESP_LOGE(TAG, "Factory reset refused: factory Wi-Fi record unavailable (%s)", esp_err_to_name(fw_err));
        mbedtls_platform_zeroize(&fw, sizeof(fw));
        return fw_err;
    }

    /* Codes, Wi-Fi and lockout first (commit to NVS, then to RAM); registrations only after that succeeded, so a
     * failed save leaves the panel exactly as it was. Each NVS write is atomic per key: once panel_cfg is saved the
     * caller must reload from NVS (restart) even if erasing a registration fails, and a retry repeats both steps. */
    doorbell_panel_cfg_blob_t next = *cfg;
    memcpy(next.admin_code, next.factory_admin_code, sizeof(next.admin_code));
    memcpy(next.user_code, next.factory_user_code, sizeof(next.user_code));
    memcpy(next.wifi_ssid, fw.wifi_ssid, sizeof(next.wifi_ssid));
    memcpy(next.wifi_pass, fw.wifi_pass, sizeof(next.wifi_pass));
    next.wrong_code_count = 0;
    next.lockout_level = 0;
    next.lockout_remaining_s = 0;
    mbedtls_platform_zeroize(&fw, sizeof(fw));
    esp_err_t err = doorbell_storage_panel_save_cfg(&next);
    if (err == ESP_OK) {
        *cfg = next;
    }
    mbedtls_platform_zeroize(&next, sizeof(next));
    if (err != ESP_OK) {
        return err;
    }
    *out_nvs_changed = true;
    return doorbell_storage_panel_erase_all_apts(min_apt, max_apt);
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
    /* Erase every slot even if one fails, and report the first failure: a missing key is the normal empty slot */
    esp_err_t first_err = ESP_OK;
    for (uint8_t apt = min_apt; apt <= max_apt; apt++) {
        char key[12];
        format_apt_key(apt, key);
        esp_err_t e = nvs_erase_key(h, key);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Erasing registration %s failed: %s", key, esp_err_to_name(e));
            if (first_err == ESP_OK) {
                first_err = e;
            }
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
    return (first_err != ESP_OK) ? first_err : err;
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

