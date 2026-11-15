#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "doorbell_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apartment Module Status LED Priority States (docs/protocol.md §7.2, 0 = highest priority); patterns in main.c.
 */
typedef enum {
    LED_UNIT_RINGING = 0,      /* Fast blink 200 ms period during bell relay pulse */
    LED_UNIT_BTN_CANCEL,       /* Repeating triple flash for two repeat periods on >= 15 s hold or invalid combo */
    LED_UNIT_PAIR_HOLD,        /* Medium blink 500 ms period while pairing button held 7-12 s */
    LED_UNIT_RESET_HOLD,       /* 900 ms ON / 100 ms OFF while reset button held 7-12 s */
    LED_UNIT_RESULT_OK,        /* Full brightness solid ON for 2000 ms (one-shot) */
    LED_UNIT_NO_RESPONSE,      /* Medium blink 500 ms period for 3000 ms (one-shot) */
    LED_UNIT_PAIRING_SEARCH,   /* Slow blink 1 Hz while searching/completing pairing */
    LED_UNIT_UNPAIRED,         /* Repeating double flash when unpaired */
    LED_UNIT_OUT_OF_COV,       /* Sparse single flash (3000 ms) when disconnected or 3 missed HEARTBEAT_ACKs */
    LED_UNIT_IN_COVERAGE,      /* Dim solid (10%) when paired, connected, and panel reachable */
    LED_UNIT_MAX
} unit_led_state_t;

/* Flags shared between the button callbacks (esp_timer task, unit_buttons.c) and unit_worker_task (main.c). Set in one
 * task and consumed in the other with atomic_exchange() (AGENTS.md §7). */

/** Mirror of the persisted pairing state, readable from any task */
extern _Atomic bool g_unit_paired;
/** The pairing client is running: hold / release events of the hidden buttons are ignored */
extern _Atomic bool g_unit_pairing_active;
/** An OPEN_DOOR transaction is running: further door presses are coalesced */
extern _Atomic bool g_unit_open_door_in_flight;

/* Requests from the buttons to unit_worker_task */
extern _Atomic bool g_unit_open_door_requested;
extern _Atomic bool g_unit_pregen_x25519_requested;
extern _Atomic bool g_unit_pairing_requested;
extern _Atomic bool g_unit_reset_requested;

/** Door-open, hidden pairing and hidden reset buttons (espressif/button) */
esp_err_t unit_buttons_init(void);

/* ---- Worker-task services of main.c used by unit_pairing.c ---- */

/** Periodic work while the worker task waits: task watchdog and bell relay safety clamp */
void unit_poll(void *ctx);

/** Waits for the *_ACK answering one request; RING_REQs arriving meanwhile are handled as usual */
typedef struct {
    int sock;
    uint8_t apt_no;
    uint8_t ack_type;
    const uint8_t *k_apt;
    uint32_t rx_floor;
    uint32_t req_tx_counter;
    uint16_t req_seq_id;
    /* Accepted ACK */
    doorbell_hdr_t ack_hdr;
    uint8_t plain[DOORBELL_PL_MAX_SIZE];
    size_t plain_len;
    uint8_t status;
    uint32_t from_ip;
} unit_ack_wait_t;

/** Send @p frame with @p retries and wait for the ACK described by @p w (doorbell_txn_run) */
esp_err_t unit_run_txn(uint8_t *frame, size_t frame_len, uint32_t peer_ip, uint32_t retries, unit_ack_wait_t *w,
                       uint32_t *out_rtt_ms);

uint16_t unit_next_seq_id(void);
void unit_set_apartment_hostname(uint8_t apt_no);

/* ---- Two-phase pairing client (unit_pairing.c) ---- */

/** What a completed pairing hands to main.c to commit */
typedef struct {
    uint8_t apt_no;
    uint8_t panel_mac[DOORBELL_MAC_SIZE];
    doorbell_provision_blob_t prov; /**< building Wi-Fi, channel, panel LAN IPv4 (as confirmed), K_apt, interval */
    uint32_t ack_rx_counter;        /**< tx_counter of PAIR_COMPLETE_ACK: the module's first replay floor */
    uint32_t panel_boot_id;
} unit_pairing_result_t;

/** Generate the X25519 keypair while the pairing button is held, so RF entropy is used and pairing starts fast */
void unit_pairing_pregenerate_key(void);

/**
 * Phase 1 (open SoftAP: PAIR_HELLO / PAIR_CHALLENGE / PAIR_CONFIRM) and phase 2 (building LAN: PAIR_COMPLETE /
 * PAIR_COMPLETE_ACK). Nothing is persisted here. The keypair is destroyed on return.
 * @return true with @p out filled (the caller zeroizes it after committing), false on any failure
 */
bool unit_pairing_run(int udp_sock, unit_pairing_result_t *out);

#ifdef __cplusplus
}
#endif
