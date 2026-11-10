# ADR 0015: Production Security Hardening (Secure Boot v2, Flash Encryption, NVS Encryption)

- **Status:** Accepted (2026-11-10) — Option B, implemented as the separate flasher **Burn** profile (§7); first
  hardware run on one spare board per §6. The **Development** and **Production** profiles stay unhardened.
- **Date:** 2026-11-10
- **Supersedes (if accepted):** parts of ADR 0013 (offline, no OTA, UART re-flash) and AGENTS.md §8 item 5
  (`I-2`, `I-16`, `I-17`: Secure Boot / flash encryption / NVS encryption stay disabled)
- **Related:** [production-checklist.md](../production-checklist.md), design review `I-2`, `I-16`, `I-17`, `I-56`

## 1. Context

Today every panel and module runs with ESP-IDF security defaults: no Secure Boot, no flash encryption, no NVS
encryption. Anyone who opens an enclosure and reads the SPI flash obtains the per-apartment keys `K_apt`, the admin and
user codes, and the building Wi-Fi passphrase, and can flash modified firmware. ADR 0013 accepted this as a physical
boundary in exchange for UART re-flashing in the field, because the system has no OTA.

The owner asked to move to production level, including hardening. The eFuse operations are **irreversible per chip**,
so the options and their consequences are recorded here before anything is enabled.

## 2. Verified facts (ESP-IDF v6.1 documentation and this project's builds)

| # | Fact | Source |
| :--- | :--- | :--- |
| F1 | In flash-encryption **release** mode the UART bootloader cannot perform flash-encryption operations; new plaintext images can only arrive via OTA | `docs/en/security/flash-encryption.rst:719` |
| F2 | With a key **pre-generated on the host** and burned into the chip, binaries can be pre-encrypted on the host and flashed as ciphertext over UART, also in release mode | `flash-encryption.rst:472`, `:690` |
| F3 | **Development** mode still allows flashing new plaintext images (re-encrypted by the bootloader); intended for development only. The enable/disable state is an odd/even bit count in the 7-bit `FLASH_CRYPT_CNT` eFuse, so the number of toggles is limited | `flash-encryption.rst:387`, `:89`, `:248` |
| F4 | Secure Boot v2 (RSA) is available on ESP32 from chip revision **v3.0**; requires `CONFIG_ESP32_REV_MIN >= v3.0`. ECDSA-based Secure Boot v2 is not recommended on ESP32 | `secure-boot-v2.rst:44`, `:59` |
| F5 | Classic ESP32 has no HMAC peripheral: NVS encryption must use the **flash-encryption-based scheme**, which needs an encrypted `nvs_keys` partition (4 KB) — i.e. a custom partition table | `api-reference/storage/nvs_encryption.rst:39` |
| F6 | Secure Boot and flash encryption enlarge the bootloader and may require moving the partition table offset | `secure-boot-v2.rst:433` |
| F7 | Current bootloader: `0x6640` bytes with only `0x9c0` (9 %) free below the partition table at `0x8000` | build output, 2026-11-10 |
| F8 | Current app: panel `-Og` 960,304 B (91.6 % of the 1 MB factory slot); `-Os` 0xd75a0 (16 % free). Flash is 2 MB, single `factory` slot, no OTA | build output, `sdkconfig.defaults` |
| F9 | Bench chips: `ESP32-D0WD-V3 (revision v3.1)` → F4 is satisfied | flasher probe log |

**Consequence of F6 + F7:** enabling Secure Boot and/or flash encryption will very likely push the partition table
offset beyond `0x8000`. That moves the NVS partition, so panels already in the field would lose their pairing database
on migration (no backup, `I-15`) and every module would have to be re-paired.

## 3. Options

| Option | What is enabled | Field update path | Main cost / risk |
| :--- | :--- | :--- | :--- |
| **A. Status quo** | Nothing (ADR 0013 unchanged) | UART plaintext re-flash | Flash readout exposes keys and codes; firmware can be replaced |
| **B. Release FE + SBv2, device-generated keys** | Flash encryption (release), Secure Boot v2, NVS encryption | **None** (F1): devices are frozen at the shipped firmware | Any bug fix requires replacing hardware |
| **C. Release FE with host-generated per-device keys + SBv2** | Same as B, but FE key generated and recorded per device at the factory | UART flashing of **pre-encrypted, signed** images (F2) | A secret per-device key database and the signing private key must be kept offline and backed up; losing them = no updates; leaking them = no protection |
| **D. LAN OTA + release FE + SBv2** | B plus signed OTA over the building LAN | OTA (F1) | New feature (two OTA slots, rollback); 2 MB flash cannot hold two 1 MB slots + NVS → **4 MB modules** required; reverses ADR 0013 |

All of B–D also require: `CONFIG_ESP32_REV_MIN` ≥ v3.0 (F4), a custom partition table with `nvs_keys` (F5), a new
partition table offset (F6/F7), a new flasher flow (eFuse burning, key/signature handling), and a migration plan for
fielded panels (re-provision + re-pair).

## 4. Decisions required from the owner

1. **Threat model:** is physical flash readout by someone who opens a panel or module enclosure in scope?
   (If no → Option A, close this ADR as Rejected.)
2. **Update path:** frozen devices (B), factory-held keys with UART updates (C), or LAN OTA with 4 MB hardware (D)?
3. **Key custody:** who stores the Secure Boot signing key (and, for C, the per-device FE key database), where, and
   how it is backed up.
4. **Fielded devices:** migrate (full re-flash, new partition table, re-provision codes, re-pair every module) or leave
   existing installations on the unhardened release.

## 5. Decision (owner, 2026-11-10)

| Question | Answer |
| :--- | :--- |
| 1. Threat model | **In scope** — physical flash readout and firmware replacement must be prevented |
| 2. Update path | **None (Option B)** — devices keep the firmware they ship with |
| 3. Key custody | **No stored key** (revised 2026-11-10, was: owner keeps it offline): every Burn build creates a one-time Secure Boot v2 key, signs with it and destroys it. Burned chips only keep its public-key digest and can never be re-flashed, so a kept key would have no use and would only be something to steal or lose |
| 4. Fielded devices | **None in the field** — no migration needed |

Consequences of Option B:

- A Production device can never be re-flashed: bug fixes and **compile-time (Kconfig) setting changes** require new
  hardware. Runtime settings (codes, building Wi-Fi via portal/console, pairing) keep working.
- Factory provisioning (unique codes, optional Wi-Fi) must be written **before** the first boot of a Production image,
  because the first boot encrypts flash and closes UART access. With NVS encryption the provisioned NVS data must
  match the `nvs_keys` partition prepared at the factory; the exact sequence is validated in the prototype.
- Flashing a Production image to a chip and powering it is irreversible for that chip. The flasher must require an
  explicit confirmation and must keep Development boards on the Development profile.
- One-time keys: each Burn build has its own key digest (recorded in the build-info sidecar), and all chips burned
  from the same `_burn.bin` share it. Knowing a key would not help an attacker: burned chips accept no new image
  (UART download mode disabled, release flash encryption), and a blank chip accepts any key anyway.

## 6. Next step (one irreversible action, on one spare board only)

The bootloader size and partition layout were measured on the host instead (§7, F10–F12), so no development-mode
prototype is needed. The first hardware run is the Burn flow itself on **one spare ESP32-D0WD-V3 rev v3.x board**
(not the XMC-flash board), signed with the build's one-time key: confirm the first-boot log (Secure Boot digest burned,
"Flash encryption completed", app starts, panel boots with the provisioned codes / module generates its NVS keys),
then the acceptance rows of the production checklist. Only after that run are further boards burned.

**First run, 2026-11-10 (panel, COM33, ESP32-D0WD-V3 rev v3.1, generic 4 MB flash):** burn succeeded — every boot
shows `secure boot v2 enabled`, `Signature verified successfully!`, `Flash encryption mode is RELEASE`
(`0 plaintext flashes left`), the F11 partition table, and the app reads the encrypted NVS (`Loaded 0 registered
apartment(s)`, no blank-NVS / unreadable-cfg warning). The board then browns out at the first full RF calibration
(`phy_init: falling back to full calibration` → `Brownout detector was triggered`) on every boot, including power-on;
the owner attributes it to the board's on-board 3.3 V regulator. Not a Burn defect, but on a burned board it can only
be fixed in hardware, hence checklist step 5.0 (power check before burning). The flasher now reports brownouts and
reboot loops during the first boot as errors. Repeat the run on another board with an adequate supply.

## 7. Implementation (2026-11-10)

On the owner's request the hardening is a **third flasher profile, Burn**, instead of changing Production: Production
remains the unlocked `-Os` release build used to bench-test exactly the code that is burned.

| # | Fact (host measurement, ESP-IDF v6.1) | Source |
| :--- | :--- | :--- |
| F10 | Burn bootloader (SBv2 + release FE) `0xb000` B; signed (+4 KB signature sector) `0xc000` B, ends at `0xd000` | panel `build_burn` |
| F11 | Partition table moved to `0x10000`: `nvs` `0x11000` (24 KB), `nvs_keys` `0x17000` (4 KB, encrypted), `factory` `0x20000` (1 MB) | `firmware/partitions_burn.csv` |
| F12 | Panel app `-Os` with SBv2/FE/NVS encryption `0xe0000` B unsigned (12 % of the slot free) | panel `build_burn` |
| F13 | `nvs_partition_gen --keygen` draws keys from Python `random`; the flasher generates `nvs_keys` with `secrets` and passes `--inputkey` | `esp_idf_nvs_partition_gen` source |
| F14 | `nvs_flash_init()` regenerates NVS keys on **any** key-partition read error, orphaning provisioned NVS | `nvs_api.cpp` `nvs_flash_init` |
| F15 | ESP32 has no Secure Download Mode; the choices are "permanently disabled" (IDF recommendation) or "enabled" | `bootloader/Kconfig.projbuild` |

Design:

- **Build:** `firmware/sdkconfig.burn` overlay on a copy of the role's sdkconfig in `firmware/<role>/build_burn/`
  (`-Os`, `CONFIG_ESP32_REV_MIN_3`, SBv2 RSA, release FE, UART download mode permanently disabled (F15), NVS
  encryption with the flash-encryption scheme, custom table F11). The flasher checks that kconfgen kept every overlay
  line, creates a one-time RSA-3072 key in a temporary folder outside the project, signs bootloader and app with
  `espsecure sign-data --version 2`, overwrites and deletes the key, verifies both signatures, checks the signed bootloader ends before the table,
  merges with flash parameters `keep` (a rewritten header would break the signature) and records the public-key digest
  in the build-info sidecar. Output: `<role>_<ts>_burn.bin`.
- **Flash (per device, typed confirmation):** `espefuse summary` pre-check refuses anything but a fresh rev ≥ v3.0 chip
  with coding scheme NONE, no key in BLOCK1/2 and FE/SB not yet enabled. Panel: host-generated `nvs_keys` (F13) and an
  NVS image encrypted with it, verified by decrypting it back to the plaintext image. One `write-flash --erase-all`
  writes bootloader + table, app and (panel) NVS + `nvs_keys`; the module gets blank NVS and generates its own keys.
  The flasher then resets the chip with DTR released (no accidental download mode / reset during encryption) and
  follows the first-boot log until the app has started on encrypted flash.
- **Firmware (F14):** with `CONFIG_NVS_ENCRYPTION` the storage component generates keys only for a **blank**
  `nvs_keys` partition; a corrupt one fails closed (I-13: no implicit erase).
