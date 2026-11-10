# ADR 0014: Admin and User Code Lengths Are Compile-Time Settings

- **Status:** Accepted.
- **Date:** recorded retroactively on 2026-11-10. The decision was cited in `AGENTS.md` and `yapilandirma-ayarlari.md`
  before this file existed (design review `I-56`); the original discussion was not preserved, so this record states the
  decision and the facts that can be checked in this repository.
- **Related:** design review `I-4`, `I-5`, `I-6`, `I-56`; [ADR 0013](0013-offline-operation-no-ota.md).

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
| F1 | The setup portal SoftAP is WPA3-SAE with the admin code as passphrase; an IEEE 802.11 passphrase is 8–63 characters, hence the minimum of 8 digits | `components/doorbell_common/Kconfig.projbuild`, `AGENTS.md` (`I-4`) |
| F2 | Codes are stored in fixed 12-byte fields (`doorbell_panel_cfg_blob_t`), enough for 10 digits + NUL | `components/doorbell_storage/include/doorbell_storage.h` |
| F3 | The built-in blank-NVS fallback codes must match the configured lengths; checked with `static_assert` | `components/doorbell_common/include/doorbell_config_checks.h` |

## Consequences

- Changing a length needs a rebuild and a full Flash with new factory codes (ADR 0013); for a burned device it is fixed
  for its lifetime (ADR 0015).
- A firmware built with other lengths rejects the stored codes, which is why the flasher derives the lengths from the
  image instead of the GUI form.
