#include "doorbell_keypad.h"
#include <string.h>

#define MAX_TOKENS 4 /* <admin> * <cmd> * <arg1> * <arg2> */

static bool is_all_digits(const char *s, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

/* No leading zeros ("0", "01") and value within [min_apt_no .. max_apt_no] (docs/protocol.md §6.2 step 5) */
static bool parse_apt_number(const doorbell_kp_token_t *tok, const doorbell_kp_limits_t *limits, uint8_t *out_apt)
{
    if (tok->len == 0 || tok->len > 3 || !is_all_digits(tok->ptr, tok->len) || tok->ptr[0] == '0') {
        return false;
    }
    unsigned val = 0;
    for (size_t i = 0; i < tok->len; i++) {
        val = val * 10U + (unsigned)(tok->ptr[i] - '0');
    }
    if (val < limits->min_apt_no || val > limits->max_apt_no) {
        return false;
    }
    *out_apt = (uint8_t)val;
    return true;
}

/* Both new-code entries: the given length and equal, compared in constant time */
static bool new_code_pair_ok(const doorbell_kp_token_t *a, const doorbell_kp_token_t *b, size_t required_len)
{
    if (a->len != required_len || b->len != required_len) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < required_len; i++) {
        diff |= (unsigned char)(a->ptr[i] ^ b->ptr[i]);
    }
    return diff == 0;
}

void doorbell_keypad_parse(const char *seq, size_t len, const doorbell_kp_limits_t *limits, doorbell_kp_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = DOORBELL_KP_MALFORMED;
    if (!seq || !limits) {
        return;
    }
    out->starred = (len > 0 && seq[0] == '*');

    /* Step 1: cancel ("*#") */
    if (len == 1 && seq[0] == '*') {
        out->kind = DOORBELL_KP_CANCEL;
        return;
    }
    if (len == 0) {
        return;
    }

    /* Visitor ring: does not start with '*' */
    if (!out->starred) {
        doorbell_kp_token_t tok = {.ptr = seq, .len = len};
        if (parse_apt_number(&tok, limits, &out->apt_no)) {
            out->kind = DOORBELL_KP_RING;
        }
        return;
    }

    /* Step 3: tokenize by '*' after the leading '*'; every token must be non-empty digits */
    doorbell_kp_token_t tok[MAX_TOKENS] = {0};
    size_t num_tok = 0;
    const char *p = seq + 1;
    const char *end = seq + len;
    for (;;) {
        if (num_tok == MAX_TOKENS) {
            return; /* too many '*' segments */
        }
        const char *star = memchr(p, '*', (size_t)(end - p));
        const char *tok_end = star ? star : end;
        tok[num_tok].ptr = p;
        tok[num_tok].len = (size_t)(tok_end - p);
        if (!is_all_digits(tok[num_tok].ptr, tok[num_tok].len)) {
            return;
        }
        num_tok++;
        if (!star) {
            break;
        }
        p = star + 1;
    }

    out->code = tok[0];

    /* Case A: user door-open candidate "* <digits> #" */
    if (num_tok == 1) {
        out->kind = DOORBELL_KP_USER_CODE;
        return;
    }

    /* Case B: admin command "* <admin> * <cmd> [* <arg1> [* <arg2>]] #" */
    if (tok[1].len != 1) {
        return;
    }
    uint8_t apt2 = 0;
    bool shape_valid = false;
    switch (tok[1].ptr[0]) {
    case DOORBELL_KP_CMD_PAIR:
        shape_valid = (num_tok == 3) && parse_apt_number(&tok[2], limits, &out->apt_no);
        break;
    case DOORBELL_KP_CMD_DELETE:
        shape_valid = (num_tok == 4) && parse_apt_number(&tok[2], limits, &out->apt_no) &&
                      parse_apt_number(&tok[3], limits, &apt2) && (out->apt_no == apt2);
        break;
    case DOORBELL_KP_CMD_SET_USER:
        shape_valid = (num_tok == 4) && new_code_pair_ok(&tok[2], &tok[3], limits->user_code_len);
        break;
    case DOORBELL_KP_CMD_SET_ADMIN:
        shape_valid = (num_tok == 4) && new_code_pair_ok(&tok[2], &tok[3], limits->admin_code_len);
        break;
    case DOORBELL_KP_CMD_PORTAL:
        shape_valid = (num_tok == 2);
        break;
    case DOORBELL_KP_CMD_BUILDING_RESET:
        shape_valid = (num_tok == 3);
        break;
    default:
        break;
    }
    if (!shape_valid) {
        out->apt_no = 0;
        return;
    }
    out->kind = DOORBELL_KP_ADMIN;
    out->cmd = (doorbell_kp_cmd_id_t)tok[1].ptr[0];
    if (num_tok >= 3) {
        out->arg = tok[2];
    }
}
