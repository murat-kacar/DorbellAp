# ADR 0013: Offline Operation, No OTA, UART Re-flash

- **Status:** Accepted; for the Burn profile superseded by [ADR 0015](0015-production-security-hardening.md)
  (no field updates at all).
- **Date:** recorded retroactively on 2026-11-10. The decision was cited in `AGENTS.md`, the user guides and the design
  review (`I-56`) before this file existed; the original discussion was not preserved, so this record states the
  decision and the facts that can be checked in this repository.
- **Related:** design review `I-2`, `I-16`, `I-17`, `I-56`; [ADR 0015](0015-production-security-hardening.md).

## Decision

1. The system works only on the building LAN: no internet, cloud or remote service (`AGENTS.md` §2).
2. No OTA update. Every device uses ESP-IDF's default single `factory` app partition table on 2 MB flash
   (`CONFIG_PARTITION_TABLE_SINGLE_APP`, `CONFIG_ESPTOOLPY_FLASHSIZE_2MB`, both ESP-IDF defaults).
3. Firmware and compile-time (Kconfig) settings change only by re-flashing over UART with the flasher (Production
   profile). Runtime settings (codes, building Wi-Fi, pairing) change on the device itself.

## Facts

| # | Fact | Source |
| :--- | :--- | :--- |
| F1 | The `factory` app slot is 1 MB; the panel app (`-Os`) is about 0xE0000 B | build output, ADR 0015 F8/F12 |
| F2 | Two OTA app slots of that size plus NVS do not fit in 2 MB flash; OTA would need 4 MB modules | ADR 0015 option D |
| F3 | Release-mode flash encryption accepts new plaintext images only via OTA | ADR 0015 F1 |

## Consequences

- Fixes and Kconfig changes need physical access to each device (design review `I-17`); a field update means a visit.
- Release flash encryption and UART re-flash exclude each other (F3). ADR 0015 resolves this per profile: Production
  stays unencrypted and re-flashable, Burn is encrypted and never updated.
- Revisiting this decision (OTA) requires 4 MB modules (F2) and a signed-update design.
