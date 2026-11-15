#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return a human-readable English description of an esp_reset_reason_t code.
 */
const char *doorbell_reset_reason_str(uint8_t reason);

#ifdef __cplusplus
}
#endif
