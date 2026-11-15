#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pure syntax of a completed keypad sequence (docs/protocol.md §6.2, I-20), without secrets or side effects.
 *
 * Parsing never compares a code: it only classifies the sequence and checks its shape, so the caller can apply the
 * mode check and the lockout gate and reject malformed input BEFORE any secret comparison (I-20). Pure C without
 * ESP-IDF APIs: also built for the linux target by test/host_unit.
 */

typedef enum {
    DOORBELL_KP_MALFORMED = 0, /**< error LED, never counts as a wrong code */
    DOORBELL_KP_CANCEL,        /**< "*#" */
    DOORBELL_KP_RING,          /**< "<apt>#" */
    DOORBELL_KP_USER_CODE,     /**< "*<digits>#": door-open candidate */
    DOORBELL_KP_ADMIN,         /**< "*<admin>*<cmd>[*<arg>[*<arg>]]#" with a valid shape for <cmd> */
} doorbell_kp_kind_t;

/** Admin commands (the <cmd> digit) */
typedef enum {
    DOORBELL_KP_CMD_PAIR = '1',           /**< *<admin>*1*<apt># */
    DOORBELL_KP_CMD_DELETE = '2',         /**< *<admin>*2*<apt>*<apt># (both equal) */
    DOORBELL_KP_CMD_SET_USER = '3',       /**< *<admin>*3*<new>*<new># (both equal, user code length) */
    DOORBELL_KP_CMD_SET_ADMIN = '4',      /**< *<admin>*4*<new>*<new># (both equal, admin code length) */
    DOORBELL_KP_CMD_PORTAL = '5',         /**< *<admin>*5# */
    DOORBELL_KP_CMD_BUILDING_RESET = '6', /**< *<admin>*6*<admin># */
} doorbell_kp_cmd_id_t;

/** A run of digits inside the parsed sequence (not NUL-terminated) */
typedef struct {
    const char *ptr;
    size_t len;
} doorbell_kp_token_t;

typedef struct {
    doorbell_kp_kind_t kind;
    bool starred;               /**< the sequence starts with '*' (subject to the mode check and the lockout gate) */
    doorbell_kp_cmd_id_t cmd;   /**< DOORBELL_KP_ADMIN */
    uint8_t apt_no;             /**< DOORBELL_KP_RING, CMD_PAIR, CMD_DELETE */
    doorbell_kp_token_t code;   /**< USER_CODE: the candidate; ADMIN: the admin code candidate */
    doorbell_kp_token_t arg;    /**< CMD_SET_USER / CMD_SET_ADMIN: the new code; CMD_BUILDING_RESET: the admin repeat */
} doorbell_kp_cmd_t;

typedef struct {
    uint8_t min_apt_no;
    uint8_t max_apt_no;
    uint8_t user_code_len;
    uint8_t admin_code_len;
} doorbell_kp_limits_t;

/**
 * @brief Classify @p seq (the keys before '#', '#' excluded). @p out's tokens point into @p seq.
 */
void doorbell_keypad_parse(const char *seq, size_t len, const doorbell_kp_limits_t *limits, doorbell_kp_cmd_t *out);

#ifdef __cplusplus
}
#endif
