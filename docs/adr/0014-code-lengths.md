# ADR 0014: Admin and User Code Lengths Are Compile-Time Settings

- **Status:** Accepted.
- **Date:** 2026-11-10
- **Related:** [ADR 0013](0013-offline-operation-no-ota.md).

## Decision

1. The admin code has `CONFIG_DOORBELL_PANEL_ADMIN_CODE_LEN` digits (default 8, range 8–10); the shared user code has
   `CONFIG_DOORBELL_PANEL_USER_CODE_LEN` digits (default 4, range 4–8). Both are fixed at compile time.
2. The keypad grammar accepts only codes of exactly these lengths; a different length is a syntax error, not a wrong
   code (no lockout count).
3. The flasher writes codes of the lengths recorded in the selected image's build-info file and refuses others, so the
   provisioned factory codes always match the firmware.

## Facts

| # | Fact | Source |
| :--- | :--- | :--- |
| F1 | The setup portal SoftAP is WPA3-SAE with the admin code as passphrase; an IEEE 802.11 passphrase is 8–63 characters, hence the minimum of 8 digits | `firmware/panel/main/Kconfig.projbuild`, `AGENTS.md` |
| F2 | Codes are stored in fixed 12-byte fields (`doorbell_panel_cfg_blob_t`), enough for 10 digits + NUL | `components/doorbell_storage/include/doorbell_storage.h` |

## Consequences

- Changing a length needs a rebuild and a full Flash with new factory codes (ADR 0013); for a burned device it is fixed
  for its lifetime (ADR 0015).
- A firmware built with other lengths rejects the stored codes, which is why the flasher derives the lengths from the
  image instead of the GUI form.
