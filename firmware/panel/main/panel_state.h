#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "doorbell_config_checks.h"
#include "doorbell_relay.h"
#include "doorbell_protocol.h"
#include "doorbell_storage.h"
#include "doorbell_led.h"
#include "doorbell_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Panel mutually exclusive operating modes (docs/protocol.md §5.1, I-43).
 */
typedef enum {
    MODE_IDLE = 0,
    MODE_PAIR_DISCOVER,
    MODE_PAIR_COMPLETE,
    MODE_PORTAL,
    MODE_RESETTING,
} panel_mode_t;

/**
 * @brief Per-apartment runtime state on the Panel.
 */
typedef struct {
    doorbell_panel_apt_blob_t nvs;
    uint32_t runtime_tx_counter;
    uint32_t persisted_rx_floor;
    int64_t last_heartbeat_us;
    bool no_report_warned;
    uint64_t active_challenge_nonce;
    int64_t challenge_deadline_us;
    doorbell_dedup_cache_t last_ack;
} panel_apt_slot_t;

extern SemaphoreHandle_t g_panel_mutex;
extern doorbell_panel_cfg_blob_t g_panel_cfg;
extern panel_apt_slot_t g_apt_slots[DOORBELL_MAX_APT_SLOTS];
extern volatile panel_mode_t g_panel_mode;
extern uint32_t g_panel_boot_id;
extern doorbell_relay_t g_door_relay;

/* Actions invoked by keypad, portal, or UART console (queued safely to panel_worker_task) */
void panel_action_open_door_local(void);
void panel_action_ring_apartment(uint8_t apt_no);
void panel_action_start_pairing(uint8_t apt_no);
void panel_action_start_portal(void);
void panel_action_cancel_active_mode(void);
void panel_action_delete_apartment(uint8_t apt_no);
void panel_action_building_reset(void);
void panel_action_reset_codes_to_factory(void);

/* Lockout & Credential Verification (panel_keypad.c) */
esp_err_t panel_keypad_init(void);
/* Drains queued key presses; must be called from panel_worker_task (never from a button/esp_timer callback) */
void panel_keypad_process(void);
void panel_lockout_tick_1s(void);
bool panel_is_locked_out(void);
bool panel_verify_admin_code_ct(const char *candidate);

/* Setup Web Portal (panel_portal.c) */
esp_err_t panel_portal_start(void);
void panel_portal_stop(void);
void panel_portal_tick(void);

/* UART Console (panel_console.c) */
esp_err_t panel_console_init(void);

#ifdef __cplusplus
}
#endif

