#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "panel_config.h"
#include "doorbell_relay.h"
#include "doorbell_reset_reason.h"
#include "doorbell_protocol.h"
#include "doorbell_storage.h"
#include "doorbell_led.h"
#include "doorbell_net.h"
#include "doorbell_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Panel Status LED Priority States (docs/protocol.md §7.1, 0 = highest priority); patterns in main.c.
 */
typedef enum {
    LED_PANEL_KEY_PRESS = 0,   /* 50 ms single short flash (one-shot) */
    LED_PANEL_DOOR_OPEN,       /* Solid ON for door relay pulse (one-shot) */
    LED_PANEL_RESULT_OK,       /* Solid ON for 2000 ms (one-shot) */
    LED_PANEL_RESULT_ERR,      /* Fast blink 200 ms period for 1000 ms (one-shot) */
    LED_PANEL_NO_RESPONSE,     /* Medium blink 500 ms period for 3000 ms (one-shot) */
    LED_PANEL_RESET_HOLD,      /* 900 ms ON / 100 ms OFF while reset button held 10-60 s */
    LED_PANEL_TYPING,          /* Slow blink 1 Hz while keypad buffer non-empty */
    LED_PANEL_PAIRING,         /* Repeating double flash during pairing discovery/completion */
    LED_PANEL_PORTAL,          /* Slow blink 1 Hz while setup web portal is open */
    LED_PANEL_LOCKOUT,         /* Sparse single flash (3000 ms) during keypad lockout */
    LED_PANEL_NET_LOST,        /* Sparse single flash (3000 ms) when STA disconnected from AP */
    LED_PANEL_IDLE,            /* OFF (0%) steady state */
    LED_PANEL_MAX
} panel_led_state_t;

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
extern panel_apt_slot_t g_apt_slots[DOORBELL_APT_SLOT_COUNT];
extern _Atomic panel_mode_t g_panel_mode;
extern uint32_t g_panel_boot_id;
extern doorbell_relay_t g_door_relay;

/* g_panel_mutex guards g_panel_cfg, g_apt_slots and the pairing / portal / lockout state. Every *_locked function
 * runs with it held by the calling task; PANEL_ASSERT_LOCKED() checks that at runtime (assertions stay enabled in
 * both build profiles), so the convention is enforced rather than only documented. */
static inline void panel_lock(void)
{
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
}

static inline void panel_unlock(void)
{
    xSemaphoreGive(g_panel_mutex);
}

#define PANEL_ASSERT_LOCKED() configASSERT(xSemaphoreGetMutexHolder(g_panel_mutex) == xTaskGetCurrentTaskHandle())

/* Keypad actions executed immediately; the caller holds g_panel_mutex (panel_worker_task) */
void panel_action_open_door_local_locked(void);
void panel_action_ring_apartment_locked(uint8_t apt_no);
void panel_action_delete_apartment_locked(uint8_t apt_no);

/* Requests from the keypad, portal, console and button tasks, executed later by panel_worker_task (any context) */
void panel_action_start_pairing(uint8_t apt_no);
void panel_action_start_portal(void);
void panel_action_cancel_active_mode(void);
void panel_action_building_reset(void);
void panel_action_factory_reset(void);

/* Module frame helpers shared by main.c and panel_pairing.c; the caller holds g_panel_mutex */
/** An authenticated frame arrived from @p slot's module: liveness, silence-restart backoff reset (I-52) */
void panel_note_module_rx_locked(panel_apt_slot_t *slot, int64_t now_us);
/** Encrypt the ACK for @p req_hdr into the slot's deduplication cache (I-29) and send it */
void panel_send_ack_locked(int lan_sock, panel_apt_slot_t *slot, uint8_t ack_msg_type, const doorbell_hdr_t *req_hdr,
                           const uint8_t *req_frame, size_t req_len, uint32_t ack_counter, const uint8_t *ack_plain,
                           size_t ack_plain_len, uint32_t to_ip_host);

/* Two-phase pairing server (panel_pairing.c) */
void panel_pairing_start_locked(uint8_t apt_no);
void panel_pairing_abort_locked(bool show_error_led);
/** One step of the pairing state machine (worker task, takes g_panel_mutex; waits at most the SoftAP rcv timeout) */
void panel_pairing_step(void);
/** PAIR_COMPLETE of the running session (I-30, I-45); @return false if @p clear_hdr is not that frame */
bool panel_pairing_handle_complete_locked(int lan_sock, const uint8_t *buf, size_t len, uint32_t from_ip_host,
                                          const doorbell_hdr_t *clear_hdr);

/* Lockout & Credential Verification (panel_keypad.c) */
esp_err_t panel_keypad_init(void);
/* Drains queued key presses; must be called from panel_worker_task (never from a button/esp_timer callback) */
void panel_keypad_process(void);
void panel_lockout_tick_1s(void);
/** Clear the active in-RAM keypad lockout (deadline + LED). Caller holds g_panel_mutex. */
void panel_lockout_clear_locked(void);
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
