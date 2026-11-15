#include "doorbell_storage.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "doorbell_storage";

#define PANEL_CFG_MAGIC    0x44425043UL /* 'DBPC' */
#define PANEL_APT_MAGIC    0x44425041UL /* 'DBPA' */
#define UNIT_STATE_MAGIC   0x44425553UL /* 'DBUS' */
#define FACTORY_WIFI_MAGIC 0x44424657UL /* 'DBFW' */
#define BLOB_VERSION       1U

#define KEY_PANEL_CFG    "panel_cfg"
#define KEY_FACTORY_WIFI "factory_wifi"
#define KEY_UNIT_STATE   "unit_pair"

static_assert(sizeof(doorbell_panel_cfg_blob_t) == 168, "doorbell_panel_cfg_blob_t must be 168 bytes");
static_assert(sizeof(doorbell_panel_factory_wifi_blob_t) == 112, "factory_wifi blob must be 112 bytes");
static_assert(sizeof(doorbell_panel_apt_blob_t) == 48,  "doorbell_panel_apt_blob_t must be 48 bytes");
static_assert(sizeof(doorbell_unit_state_blob_t) == 152, "doorbell_unit_state_blob_t must be 152 bytes");

/* Common record frame used by blob_load() / blob_save(): magic at 0, version at 4, crc32 as the last member */
typedef struct {
    uint32_t magic;
    uint16_t version;
} blob_head_t;

#define ASSERT_BLOB_FRAME(type)                                                                      \
    static_assert(offsetof(type, magic) == 0 && offsetof(type, version) == 4 &&                      \
                  offsetof(type, crc32) == sizeof(type) - sizeof(uint32_t), #type " record frame")
ASSERT_BLOB_FRAME(doorbell_panel_cfg_blob_t);
ASSERT_BLOB_FRAME(doorbell_panel_factory_wifi_blob_t);
ASSERT_BLOB_FRAME(doorbell_panel_apt_blob_t);
ASSERT_BLOB_FRAME(doorbell_unit_state_blob_t);

/* Set when an existing panel_cfg blob could not be validated (CRC/magic/version/size). The panel then runs on in-RAM
 * defaults, and saving would overwrite the stored record - including its unique per-device factory codes (I-5) - with
 * the shared built-in ones. Keep the stored blob intact instead (I-13 spirit); recovery is a full re-flash with the
 * flasher (I-14). */
static bool s_panel_cfg_write_protected = false;

static uint32_t blob_crc(const void *blob, size_t size)
{
    return esp_rom_crc32_le(0, (const uint8_t *)blob, size - sizeof(uint32_t));
}

/**
 * Read one record and validate size, magic, version and CRC. @p blob is zeroized unless ESP_OK is returned.
 * @return ESP_OK, ESP_ERR_NVS_NOT_FOUND (no record, or blank namespace), ESP_ERR_INVALID_SIZE (stored size differs),
 *         ESP_ERR_INVALID_CRC (magic / version / CRC mismatch) or another NVS error.
 */
static esp_err_t blob_load(const char *key, void *blob, size_t size, uint32_t magic)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = size;
        err = nvs_get_blob(h, key, blob, &sz);
        nvs_close(h);
        /* nvs_get_blob() succeeds for a stored blob shorter than the buffer and fails with INVALID_LENGTH for a longer
         * one; never accept a partial record */
        if (err == ESP_ERR_NVS_INVALID_LENGTH || (err == ESP_OK && sz != size)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err == ESP_OK) {
        blob_head_t head;
        uint32_t stored_crc;
        memcpy(&head, blob, sizeof(head));
        memcpy(&stored_crc, (const uint8_t *)blob + size - sizeof(stored_crc), sizeof(stored_crc));
        if (head.magic != magic || head.version != BLOB_VERSION || stored_crc != blob_crc(blob, size)) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(blob, size);
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "NVS record '%s' unreadable: %s", key, esp_err_to_name(err));
        }
    }
    return err;
}

/* Stamp magic, version and CRC into @p blob and commit it as one NVS key (atomic per key, I-50) */
static esp_err_t blob_save(const char *key, void *blob, size_t size, uint32_t magic)
{
    blob_head_t head = {.magic = magic, .version = BLOB_VERSION};
    memcpy(blob, &head, sizeof(head));
    uint32_t crc = blob_crc(blob, size);
    memcpy((uint8_t *)blob + size - sizeof(crc), &crc, sizeof(crc));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, blob, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Erase one key; a missing key (or blank namespace) is already the wanted state */
static esp_err_t key_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/**
 * RFC 8613 Appendix B.1.1 write-ahead allocation shared by both roles (I-50): a counter above *ceiling is returned only
 * after a new ceiling (+block) has been committed with @p save. On a failed commit nothing changes.
 */
static esp_err_t next_tx_counter(uint32_t *ceiling, uint32_t *in_out_runtime_counter, uint32_t block_size,
                                 uint32_t *out_counter, esp_err_t (*save)(void *record), void *record)
{
    if (!in_out_runtime_counter || !out_counter || block_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*in_out_runtime_counter >= DOORBELL_COUNTER_MAX_VALID) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t next = *in_out_runtime_counter + 1U;
    if (next > *ceiling) {
        uint64_t new_ceiling = (uint64_t)next + (uint64_t)block_size - 1ULL;
        if (new_ceiling > DOORBELL_COUNTER_MAX_VALID) {
            new_ceiling = DOORBELL_COUNTER_MAX_VALID;
        }
        uint32_t prev_ceiling = *ceiling;
        *ceiling = (uint32_t)new_ceiling;
        esp_err_t err = save(record);
        if (err != ESP_OK) {
            *ceiling = prev_ceiling;
            ESP_LOGE(TAG, "Failed to commit write-ahead tx_counter block: %s", esp_err_to_name(err));
            return err;
        }
    }

    *in_out_runtime_counter = next;
    *out_counter = next;
    return ESP_OK;
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
    }
    return err;
}

/* ---- Panel Storage Implementation ---- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

esp_err_t doorbell_storage_panel_load_cfg(doorbell_panel_cfg_blob_t *out_cfg,
                                          const doorbell_panel_cfg_defaults_t *blank_nvs_defaults)
{
    if (!out_cfg || !blank_nvs_defaults) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = blob_load(KEY_PANEL_CFG, out_cfg, sizeof(*out_cfg), PANEL_CFG_MAGIC);
    if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_CRC) {
        s_panel_cfg_write_protected = true;
        return err;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    /* First boot on blank NVS (out_cfg is zeroized by blob_load) */
    const doorbell_panel_cfg_defaults_t *d = blank_nvs_defaults;
    copy_str(out_cfg->wifi_ssid, sizeof(out_cfg->wifi_ssid), d->wifi_ssid);
    copy_str(out_cfg->wifi_pass, sizeof(out_cfg->wifi_pass), d->wifi_pass);
    copy_str(out_cfg->admin_code, sizeof(out_cfg->admin_code), d->admin_code);
    copy_str(out_cfg->user_code, sizeof(out_cfg->user_code), d->user_code);
    copy_str(out_cfg->factory_admin_code, sizeof(out_cfg->factory_admin_code), d->admin_code);
    copy_str(out_cfg->factory_user_code, sizeof(out_cfg->factory_user_code), d->user_code);
    /* Codes are not logged (I-25); this only flags that the shared built-in codes are active (I-5) */
    ESP_LOGW(TAG, "Blank NVS: initializing panel_cfg with built-in default codes. "
             "Provision unique per-device codes (flasher 'Flash' with the card's code fields) "
             "before deployment.");
    return doorbell_storage_panel_save_cfg(out_cfg);
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
    return blob_save(KEY_PANEL_CFG, cfg, sizeof(*cfg), PANEL_CFG_MAGIC);
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
    esp_err_t err = blob_load(KEY_FACTORY_WIFI, out, sizeof(*out), FACTORY_WIFI_MAGIC);
    if (err == ESP_OK) {
        out->wifi_ssid[sizeof(out->wifi_ssid) - 1] = '\0';
        out->wifi_pass[sizeof(out->wifi_pass) - 1] = '\0';
    }
    return err;
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
    memcpy(blob.wifi_ssid, cfg->wifi_ssid, sizeof(blob.wifi_ssid));
    memcpy(blob.wifi_pass, cfg->wifi_pass, sizeof(blob.wifi_pass));
    err = blob_save(KEY_FACTORY_WIFI, &blob, sizeof(blob), FACTORY_WIFI_MAGIC);
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

static bool apt_no_valid(unsigned apt_no)
{
    return apt_no >= 1 && apt_no <= DOORBELL_APT_NO_MAX;
}

static void format_apt_key(unsigned apt_no, char out_key[NVS_KEY_NAME_MAX_SIZE])
{
    snprintf(out_key, NVS_KEY_NAME_MAX_SIZE, "apt_%03u", apt_no);
}

esp_err_t doorbell_storage_panel_load_apt(uint8_t apt_no, doorbell_panel_apt_blob_t *out_apt)
{
    if (!out_apt || !apt_no_valid(apt_no)) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    format_apt_key(apt_no, key);
    esp_err_t err = blob_load(key, out_apt, sizeof(*out_apt), PANEL_APT_MAGIC);
    if (err == ESP_OK && out_apt->apt_no != apt_no) {
        ESP_LOGE(TAG, "NVS record '%s' holds apartment %u", key, (unsigned)out_apt->apt_no);
        mbedtls_platform_zeroize(out_apt, sizeof(*out_apt));
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

static esp_err_t save_apt_record(void *record)
{
    return doorbell_storage_panel_save_apt((doorbell_panel_apt_blob_t *)record);
}

esp_err_t doorbell_storage_panel_save_apt(doorbell_panel_apt_blob_t *apt)
{
    if (!apt || !apt_no_valid(apt->apt_no)) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    format_apt_key(apt->apt_no, key);
    return blob_save(key, apt, sizeof(*apt), PANEL_APT_MAGIC);
}

esp_err_t doorbell_storage_panel_erase_apt(uint8_t apt_no)
{
    if (!apt_no_valid(apt_no)) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    format_apt_key(apt_no, key);
    return key_erase(key);
}

esp_err_t doorbell_storage_panel_erase_all_apts(uint8_t min_apt, uint8_t max_apt)
{
    if (!apt_no_valid(min_apt) || !apt_no_valid(max_apt) || min_apt > max_apt) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOORBELL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* Erase every slot even if one fails, and report the first failure: a missing key is the normal empty slot */
    esp_err_t first_err = ESP_OK;
    for (unsigned apt = min_apt; apt <= max_apt; apt++) {
        char key[NVS_KEY_NAME_MAX_SIZE];
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
    if (!apt) {
        return ESP_ERR_INVALID_ARG;
    }
    return next_tx_counter(&apt->tx_counter_ceiling, in_out_runtime_counter, block_size, out_counter,
                           save_apt_record, apt);
}

/* ---- Apartment Module Storage Implementation ---- */

esp_err_t doorbell_storage_unit_load(doorbell_unit_state_blob_t *out_state)
{
    if (!out_state) {
        return ESP_ERR_INVALID_ARG;
    }
    return blob_load(KEY_UNIT_STATE, out_state, sizeof(*out_state), UNIT_STATE_MAGIC);
}

static esp_err_t save_unit_record(void *record)
{
    return doorbell_storage_unit_save((doorbell_unit_state_blob_t *)record);
}

esp_err_t doorbell_storage_unit_save(doorbell_unit_state_blob_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    return blob_save(KEY_UNIT_STATE, state, sizeof(*state), UNIT_STATE_MAGIC);
}

esp_err_t doorbell_storage_unit_erase(void)
{
    return key_erase(KEY_UNIT_STATE);
}

esp_err_t doorbell_storage_unit_next_tx_counter(doorbell_unit_state_blob_t *state,
                                                uint32_t *in_out_runtime_counter,
                                                uint32_t block_size,
                                                uint32_t *out_counter)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    return next_tx_counter(&state->tx_counter_ceiling, in_out_runtime_counter, block_size, out_counter,
                           save_unit_record, state);
}
