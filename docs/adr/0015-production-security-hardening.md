# ADR 0015: Production Security Hardening (Secure Boot v2, Flash Encryption, NVS Encryption)

- **Status:** Accepted — **Burn** profile (RSA-3072 Secure Boot v2, release flash encryption, NVS encryption); the
  **Production** profile stays unhardened (`-Os`, re-flashable over UART).
- **Date:** 2026-11-10
- **Supersedes:** for the Burn profile, parts of [ADR 0013](0013-offline-operation-no-ota.md) (UART re-flash)
- **Related:** [production-checklist.md](../production-checklist.md)

## 1. Context

In the **Production** profile, panels and apartment modules run with ESP-IDF security defaults (no Secure Boot, no
flash encryption, no NVS encryption), trading SPI flash readout resistance for UART re-flashability in the field
([ADR 0013](0013-offline-operation-no-ota.md)). To prevent physical flash readout of per-apartment keys `K_apt`, admin
and user codes, and building Wi-Fi credentials — as well as firmware replacement — locked deployments use the
irreversible **Burn** profile.

## 2. Verified facts (ESP-IDF v6.1 documentation and build measurements)

| # | Fact | Source |
| :--- | :--- | :--- |
| F1 | In flash-encryption **release** mode the UART bootloader cannot perform flash-encryption operations; new plaintext images can only arrive via OTA | `docs/en/security/flash-encryption.rst:719` |
| F2 | With a key **pre-generated on the host** and burned into the chip, binaries can be pre-encrypted on the host and flashed as ciphertext over UART, also in release mode | `flash-encryption.rst:472`, `:690` |
| F3 | **Development** mode still allows flashing new plaintext images (re-encrypted by the bootloader); intended for development only. The enable/disable state is an odd/even bit count in the 7-bit `FLASH_CRYPT_CNT` eFuse, so the number of toggles is limited | `flash-encryption.rst:387`, `:89`, `:248` |
| F4 | Secure Boot v2 (RSA) is available on ESP32 from chip revision **v3.0**; requires `CONFIG_ESP32_REV_MIN >= v3.0`. ECDSA-based Secure Boot v2 is not recommended on ESP32 | `secure-boot-v2.rst:44`, `:59` |
| F5 | Classic ESP32 has no HMAC peripheral: NVS encryption must use the **flash-encryption-based scheme**, which needs an encrypted `nvs_keys` partition (4 KB) — i.e. a custom partition table | `api-reference/storage/nvs_encryption.rst:39` |
| F6 | Secure Boot and flash encryption enlarge the bootloader and move the partition table offset beyond `0x8000` | `secure-boot-v2.rst:433` |
| F7 | Unhardened bootloader size (`0x6640` B) leaves only `0x9c0` B (9 %) free below `0x8000` | ESP-IDF build output |
| F8 | Panel `-Os` app fits within the 1 MB factory slot (`~0xd75a0` B unhardened, `~0xe0000` B Burn on 2 MB flash, no OTA slots) | ESP-IDF build output, `sdkconfig.defaults` |
| F9 | Target chips: `ESP32-D0WD-V3 (revision v3.x)` → F4 is satisfied | Chip spec / flasher probe |
| F10 | Burn bootloader (SBv2 + release FE) `0xb000` B; signed (+4 KB signature sector) `0xc000` B, ends at `0xd000` | panel `build_burn` |
| F11 | Burn partition table at `0x10000`: `nvs` `0x11000` (24 KB), `nvs_keys` `0x17000` (4 KB, encrypted), `factory` `0x20000` (1 MB) | `firmware/partitions_burn.csv` |
| F12 | `nvs_partition_gen --keygen` draws keys from Python `random`; the flasher generates `nvs_keys` with `secrets` and passes `--inputkey` | `esp_idf_nvs_partition_gen` source |
| F13 | `nvs_flash_init()` regenerates NVS keys on **any** key-partition read error, orphaning provisioned NVS | `nvs_api.cpp` `nvs_flash_init` |
| F14 | ESP32 has no Secure Download Mode; the choices are "permanently disabled" (IDF recommendation) or "enabled" | `bootloader/Kconfig.projbuild` |

## 3. Decision

Adopt device-generated release Flash Encryption + Secure Boot v2 + NVS Encryption as a dedicated flasher profile
(**Burn**), keeping **Production** (`-Os`, unlocked) for bench validation and re-flashable installations:

- **Threat model:** Physical flash readout and firmware replacement are in scope for locked devices.
- **Update path:** None for burned devices (devices keep the firmware they ship with; two OTA slots cannot fit in 2 MB
  flash per [ADR 0013](0013-offline-operation-no-ota.md)).
- **Key custody:** Every Burn build generates a fresh one-time RSA-3072 Secure Boot v2 key, signs the bootloader and app,
  and destroys the private key immediately. Burned chips store only the public-key digest in eFuses and permanently
  disable UART download mode (F14), so retaining signing keys offers no update capability and adds key-custody risk.

### Considered options

| Option | What is enabled | Field update path | Outcome |
| :--- | :--- | :--- | :--- |
| A. Status quo | Nothing | UART plaintext re-flash | Kept as the **Production** profile; flash readout exposes keys and codes |
| **B. Release FE + SBv2, device-generated keys** | Release flash encryption, Secure Boot v2, NVS encryption | None (F1) | **Chosen** as the **Burn** profile |
| C. Release FE with host-generated per-device keys + SBv2 | Same as B, FE key generated and recorded per device | UART flashing of pre-encrypted, signed images (F2) | Rejected: needs a secret per-device key database and a stored signing key |
| D. LAN OTA + release FE + SBv2 | B plus signed OTA over the building LAN | OTA (F1) | Rejected: two OTA slots + NVS need 4 MB modules and reverse [ADR 0013](0013-offline-operation-no-ota.md) |

### Consequences

- A burned device can never be re-flashed: firmware fixes and compile-time (Kconfig) setting changes require hardware
  replacement. Runtime settings (codes, building Wi-Fi via portal/console, pairing) continue to work.
- Factory provisioning (unique codes, optional building Wi-Fi) must be written **before** the first boot of a Burn
  image, because the first boot burns eFuses, encrypts flash, and closes UART access.
- eFuse operations are irreversible; the flasher requires explicit confirmation (`BURN`) and a pre-check on fresh chips.
  Supply stability must be verified with the Production image on each hardware batch before burning
  ([production-checklist.md](../production-checklist.md) step 5.0), since brownouts on a locked board cannot be fixed
  in firmware.

## 4. Implementation

- **Build:** `firmware/sdkconfig.burn` overlay on a copy of the role's sdkconfig in `firmware/<role>/build_burn/`
  (`-Os`, `CONFIG_ESP32_REV_MIN_3`, SBv2 RSA, release FE, UART download mode permanently disabled (F14), NVS encryption
  with the flash-encryption scheme, custom table F11). The flasher verifies that kconfgen kept every overlay line,
  creates a one-time RSA-3072 key in a temporary folder outside the project, signs bootloader and app with
  `espsecure sign-data --version 2`, overwrites and deletes the key, verifies both signatures, checks the signed
  bootloader ends before the partition table, merges with flash parameters `keep` (a rewritten header would break the
  signature), and records the public-key digest in the build-info sidecar. Output: `<role>_<ts>_burn.bin`.
- **Flash (per device, typed confirmation):** `espefuse summary` pre-check refuses anything but a fresh rev ≥ v3.0 chip
  with coding scheme NONE, no key in BLOCK1/2, and FE/SB not yet enabled. Panel: host-generated `nvs_keys` (F12) and an
  NVS image encrypted with it, verified by decrypting it back to the plaintext image. One `write-flash --erase-all`
  writes bootloader + table, app, and (panel) NVS + `nvs_keys`; the module gets blank NVS and generates its own keys.
  The flasher then resets the chip with DTR released (no accidental download mode / reset during encryption) and
  follows the first-boot log until the app has started on encrypted flash, flagging brownouts or reboot loops as errors.
- **Firmware (F13):** with `CONFIG_NVS_ENCRYPTION` the storage component generates keys only for a **blank**
  `nvs_keys` partition; a corrupt one fails closed (no implicit erase).
