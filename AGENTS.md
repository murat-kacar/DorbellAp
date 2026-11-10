# AGENTS.md — Engineering Principles & Architecture Decision Record

This document defines the binding engineering principles, architectural rules, and approved design decisions for all
developers and AI agents working on the **DorbellAp** project (Multi-Apartment Doorbell and Access Control System over
Wi-Fi AP + UDP).

Normative companion specifications:

- Wire protocol, AEAD nonce/AAD framing, state machines, keypad EBNF grammar, and LED priority table:
  [docs/protocol.md](docs/protocol.md)
- End-user manual (Turkish): [kullanim-kilavuzu.md](kullanim-kilavuzu.md)
- Configuration reference (Turkish): [yapilandirma-ayarlari.md](yapilandirma-ayarlari.md)

---

## 1. Core Engineering Principles (Mandatory)

1. **Data-Driven Engineering (No Guessing or Assumptions):**
   - Never guess, speculate, or make unverified assumptions.
   - Whenever a requirement or hardware/software behavior is ambiguous or unknown, rely strictly on verifiable data
     (existing documentation, hardware datasheets, ESP-IDF v6.1 upstream source code, IANA/IETF RFC standards, or bench
     measurements) or ask the user for clarification.
2. **Solution Hierarchy (Default → Standard → Canonical):**
   - **Step 1 (Default):** Always prefer the out-of-the-box **default** settings of the platform (`ESP-IDF v6.1`,
     `FreeRTOS`, `lwIP`).
   - **Step 2 (Standard Configuration):** If factory defaults do not satisfy a requirement, use standard configuration
     options.
   - **Step 3 (Canonical Patterns & Libraries):** If standard options are insufficient, use proven **canonical
     patterns** from the IP intercom industry and official Espressif components (`espressif/button`,
     `espressif/led_indicator`, PSA Crypto / TF-PSA-Crypto, etc.) with their default or standard configurations. Avoid
     custom, ad-hoc inventions.
3. **Language and Terminology Convention (`I-72`):**
   - **End-User & Installer Material (Turkish):** User-facing and installer-facing materials — specifically
     [kullanim-kilavuzu.md](kullanim-kilavuzu.md), [yapilandirma-ayarlari.md](yapilandirma-ayarlari.md), device labels,
     and the Setup Web Portal HTML strings — are written in Turkish.
   - **Engineering Artifacts (Conventional English):** All source code, identifiers (variables, functions, types,
     macros, enums), comments, docstrings, Kconfig symbol names, serial log messages (`ESP_LOG*`), CLI commands, build
     files, and technical/engineering documentation (`AGENTS.md`, `docs/protocol.md`, ADRs, reviews, test plans) MUST be
     written in **conventional English** using standard ESP-IDF and IP intercom terminology.
4. **Secret Redaction in Logs & UI (`I-25`, `I-47`):**
   - Never log, print, or echo admin codes, user codes, Wi-Fi passphrases, cryptographic keys, or nonces at any
     `ESP_LOG*` level or in HTTP responses. When adapting ESP-IDF Wi-Fi examples, strip all passphrase format strings.
5. **Single-Branch Git Policy (Mandatory):**
   - Maintain strictly a single Git branch (`master`) across the entire project lifecycle. Never create additional
     branches (`git branch`, `git checkout -b`, `git switch -c`, git worktrees, or branched subagent workspaces); all
     work and commits MUST be performed linearly on `master`.

---

## 2. System Topology, LAN Requirements & Project Structure

- **Network Topology & Building AP Installation Requirements (`I-9`, `I-10`, `I-36`, `I-38`):**
  - The building is equipped with an external **Dedicated Access Point (AP)** operating on a single IPv4 Layer-2
    broadcast domain (subnet).
  - **Multi-AP / High-Rise Coverage (`I-10`):** Wired-backhaul Access Points sharing the same SSID, WPA2/WPA3 passphrase,
    and Layer-2 VLAN/subnet on non-overlapping 2.4 GHz channels (`1`, `6`, `11`) are the canonical choice. Where wired
    backhaul is unavailable, a single-vendor mesh with dedicated backhaul or at most **1 wireless repeater hop** in
    transparent Layer-2 bridge mode (same SSID/passphrase, no NAT, no proxy-ARP, no `_EXT` SSID suffix) may be used and
    must be verified with a commissioning broadcast test.
  - **Mandatory AP Checklist (`I-9`, `I-36`, `I-38`):**
    1. **Visible SSID** on a fixed 2.4 GHz channel (`1`, `6`, or `11`; channels `12–13` require explicit country-code
       configuration `TR` because default country `"01"` scans channels `12–13` passively).
    2. **Client isolation (AP isolation) DISABLED**, multicast/broadcast filtering DISABLED, proxy-ARP DISABLED.
    3. **WPA2-PSK (AES-CCMP) or WPA2/WPA3 transition mode** (WEP/TKIP are rejected by ESP-IDF default authmode threshold).
       **802.11b rates enabled** (no "disable 802.11b" / minimum basic rate > 11 Mbps), because stations run
       802.11b-only by default (`I-70` item 5).
    4. **DHCP server enabled** with a lease pool and AP association table sized for `N_apartments + 1 (panel) +
       infrastructure` (use a `/23` subnet if $>240$ hosts). Assign a static DHCP reservation for the panel MAC.
- **Device Roles & Pinned Platform Versions (`I-49`, `I-55`):**
  - Pinned SDK: **ESP-IDF v6.1** with **PSA Crypto API** (`psa/crypto.h`; legacy `mbedtls_ccm_*` / `mbedtls_ecdh_*`
    headers are private in Mbed TLS 4.1 and MUST NOT be included).
  - Pinned Official Components: `espressif/button` (`^4.2.3`), `espressif/led_indicator` (`^2.1.2`), locked via
    `dependencies.lock`.
  - Target SoC: Classic **ESP32** (`IDF_TARGET_ESP32`). Default GPIO assignments in `yapilandirma-ayarlari.md` are
    specific to classic ESP32 (`I-49`); porting to `ESP32-C3` or `ESP32-S3` requires per-target Kconfig GPIO defaults
    and `GPIO_IS_VALID_OUTPUT_GPIO()` compile/boot checks.
  - Repository layout:
    - `firmware/panel` — Building entrance panel (door station)
    - `firmware/unit` — Apartment module (indoor unit)
    - `components/` — Shared components (`protocol`, NVS storage, LED indicator wrapper, button wrapper, etc.)
- **Offline Operation & Partition Table:** No internet connectivity and no OTA updates (ADR 0013). Uses the single
  `factory` application partition table on a 2 MB flash.
- **Normal Operating Mode & STA Reconnect Policy (`I-11`, `I-35`, `I-38`):**
  - Both panel and apartment modules operate in `WIFI_MODE_STA` during normal operation and obtain IPv4 addresses via
    DHCP (`CONFIG_LWIP_DHCP_RESTORE_LAST_IP=y`, `CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y`).
  - Set distinct network hostnames via `esp_netif_set_hostname()`: `"doorbell-panel"` on the panel and
    `"doorbell-apt-%03u"` on paired apartment modules (`I-38`).
  - **Multi-AP Strongest-Signal Scan (`I-35`):** Configure `wifi_sta_config_t` with `.scan_method =
    WIFI_ALL_CHANNEL_SCAN` and `.sort_method = WIFI_CONNECT_AP_BY_SIGNAL` so devices associate with the strongest AP/repeater
    rather than the first channel match.
  - **Infinite Reconnect with Backoff (`I-11`):** Never use the ESP-IDF station example's "stop after 5 retries" pattern.
    On `WIFI_EVENT_STA_DISCONNECTED`, reconnect indefinitely using exponential backoff with random jitter (`1 s` initial
    up to `60 s` maximum) whenever not paused by the Single-Radio Rule.

---

## 3. Wi-Fi Credentials Management, NVS Safety & Setup Web Portal

1. **Panel Wi-Fi Credentials (`I-14`, `I-61`):**
   - The building AP's SSID and WPA2 passphrase are predetermined prior to installation and stored in the panel's **NVS**.
   - Leave the Kconfig default passphrase empty in committed `sdkconfig.defaults` so production credentials are not
     committed to source control (`I-61`).
   - **Field-Update Methods (`I-14`):** In the field, panel Wi-Fi credentials are updated **only** via:
     1. On-demand **Wi-Fi Setup Web Portal (`* <admin_code> * 5 #`)**
     2. Authenticated `esp_console` command over UART (`I-24`)
     - *Note (`I-14`):* The ESP-IDF `nvs_partition_gen.py` tool replaces the entire NVS partition image and would wipe
       existing apartment registrations if flashed in the field; therefore, `nvs_partition_gen.py` is restricted
       exclusively to initial factory provisioning of blank devices.
2. **NVS Integrity, Persistence & Secret Erasure Rules (`I-13`, `I-23`, `I-50`, `I-51`):**
   - **Never Auto-Erase NVS on Boot (`I-13`):** The canonical ESP-IDF example snippet that calls `nvs_flash_erase()` on
     `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` is **strictly forbidden** on the panel, as it would
     wipe the building's pairing database. On NVS init failure, log a critical storage fault and indicate error without
     erasing flash. Enable `CONFIG_NVS_FLASH_VERIFY_ERASE=y`.
   - **Wi-Fi Driver RAM Storage (`I-23`):** Call `esp_wifi_set_storage(WIFI_STORAGE_RAM)` (or set
     `CONFIG_ESP_WIFI_NVS_ENABLED=n`) on both targets so the Wi-Fi driver does not keep uneraseable plaintext copies of
     the building PSK or admin code in `nvs.net80211`. All credentials are managed exclusively by the application NVS
     module.
   - **Atomic Single-Blob Records & Write-Ahead Counter (`I-50`):** Store each apartment registration (panel) and
     pairing state (module) as a single versioned binary blob (`nvs_set_blob` + `nvs_commit`) with a CRC32 so power loss
     cannot leave partial multi-key records. The monotonic `tx_counter` block (`+100`) MUST be committed to NVS
     **before** any counter in that block is transmitted (RFC 8613 Appendix B.1.1 write-ahead rule); if the NVS commit
     fails, refuse to transmit.
3. **Wi-Fi Setup Web Portal (`* <admin_code> * 5 #`) (`I-4`, `I-32`, `I-40`, `I-47`):**
   - Upon valid command entry in `MODE_IDLE`, the panel status LED turns **solid ON for 2 seconds**, then blinks slowly
     (`1 Hz`) while the portal is active (`120 s`).
   - The panel opens a temporary **WPA3-Personal (`WIFI_AUTH_WPA3_PSK` / SAE, `pmf_cfg.required = true`, `I-4`)** SoftAP
     named `Doorbell-Setup` (`.max_connection = 4`) for **2 minutes (120 s)**. Using WPA3-SAE prevents offline dictionary
     / GPU cracking of the numeric admin code.
   - **Subnet Collision Check (`I-32`):** Before starting the SoftAP (`192.168.4.1/24` default), check the `STA`
     interface's last subnet; if the building LAN uses `192.168.4.0/24`, switch the SoftAP IP to `10.195.4.1/24`.
   - **Interface Binding (`I-47`):** Bind `esp_http_server` strictly to the SoftAP interface (`httpd_config_t.if_name` /
     SoftAP IP check) so the portal is unreachable from the building LAN. Never echo the stored passphrase in the form.
   - **Test-Before-Commit & Rollback (`I-40`):** When new credentials are submitted, test-associate on `STA` (15 s
     timeout) before closing the portal. Commit to NVS and close the portal only after `IP_EVENT_STA_GOT_IP`; on
     failure, roll back to the previous credentials, display the error in the browser, and flash the 1 s error LED.
   - **Admin Code Length:** Default **8 digits** (configurable range: **8–10 digits**, ADR 0014).
   - **Single-Radio Rule (`I-31`):** While the setup portal is active (until form submission) or while pairing discovery
     is active, background `STA` channel scanning and auto-reconnect timers are paused so the single ESP32 radio does
     not hop channels.
4. **Apartment Module Wi-Fi Credentials:**
   - Apartment modules ship with a single generic firmware containing no building Wi-Fi credentials; they receive the
     building AP SSID, passphrase, channel hint, panel LAN IPv4, and `Heartbeat interval` from the panel **during
     pairing** (`I-39`) and store them in NVS.

---

## 4. Pairing Architecture (`I-26`, `I-27`, `I-30`, `I-31`, `I-45`)

- **Preconditions & User Flow (`I-31`):** The panel must be in `MODE_IDLE` and connected to the building AP with a valid
  IPv4 lease (`IP_EVENT_STA_GOT_IP`); otherwise `* <admin_code> * 1 * <apt_no> #` immediately returns the 1 s error LED.
  When valid, the panel opens a 20 s discovery window (repeating double flash). On the apartment module (next to the
  panel), the hidden pairing button is held for **7–12 s** and released (holding $\ge 15\text{ s}$ cancels with a triple
  flash).
- **Temporary Pairing Network (`I-27`, `I-30`):**
  - During the 20 s discovery window, the panel switches to `WIFI_MODE_APSTA` and opens an **open (unencrypted)**
    pairing SoftAP named `Doorbell-Pair-<MAC4>` on the same channel as its `STA` interface, with `.max_connection = 4`
    and background `STA` scans paused.
  - The pairing UDP listener is bound strictly to the SoftAP interface (`I-27`) and accepts `PAIR_HELLO` only from
    stations associated with the SoftAP. If multiple modules send `PAIR_HELLO` or associate within the 1.5 s
    collision-listen window, the panel aborts pairing.
- **Cryptographic Key Exchange (`X25519` + `HKDF-SHA256`, `I-8`, `I-26`):**
  - Ephemeral key exchange is performed using **PSA Crypto `X25519` (`PSA_ECC_FAMILY_MONTGOMERY`, 255-bit)** and
    **`PSA_ALG_HKDF(PSA_ALG_SHA_256)`** as specified in [docs/protocol.md](docs/protocol.md) §1–§3.2.
  - To ensure hardware RNG entropy (`I-26`), the apartment module starts `esp_wifi_start()` at boot and pre-generates
    its X25519 keypair during the 7-second button hold period.
- **Two-Phase Commit & Timing (`I-30`, `I-45`):**
  - The 20-second timer governs the **module discovery window**. Once a single module completes `PAIR_CONFIRM` on the
    SoftAP, the panel closes the SoftAP immediately, holds the registration as **pending in RAM only**, and starts a
    `+10 s` completion timer (Kconfig configurable) for the module to join the building AP, obtain DHCP, and send
    authenticated `PAIR_COMPLETE` over the building LAN.
  - The panel commits the new apartment record to NVS, removes any previous apartment registration sharing the same
    `unit_mac` (`I-30`, `I-45`), and replies with idempotent `PAIR_COMPLETE_ACK` **only after** verifying
    `PAIR_COMPLETE`. The module commits to NVS and shows success (`2 s` full brightness + 1 bell ring) **only after**
    receiving `PAIR_COMPLETE_ACK`.

---

## 5. UDP Communication Protocol & Security (`I-8`, `I-29`, `I-33`, `I-53`, `I-62`, `I-69`)

- **Normative Wire Specification:** See [docs/protocol.md](docs/protocol.md) for the exact 12-byte AAD header, 13-byte
  CCM nonce layout, 16-byte tag, message catalogue, and pre-crypto filtering rules.
- **Socket & Port:** Standard POSIX/BSD UDP sockets (`lwIP`) with `SO_RCVTIMEO` set to $\le \text{TWDT}/2$ (`I-46`).
  Default UDP port is **`19500`**.
- **Unicast + Dual-Path Broadcast Retry (`I-29`, `I-33`, `I-69`):**
  - Attempt 1 is sent via **unicast** to the peer's last known IPv4 address (or limited broadcast `255.255.255.255` if
    unknown).
  - If no `ACK` arrives within `Application ACK timeout` (`400 ms` $\pm 25\text{ ms}$ jitter — validated on the
    hardware bench and in a field installation with firmware `v1.0`, 802.11b-only stations), retries (`RING retries = 2`, `OPEN_DOOR retries = 1`, `HEARTBEAT retries = 1`) send the **byte-identical
    frame** to **both** the last known unicast IP **and** the limited broadcast address (`255.255.255.255`, RFC 1122
    §3.2.1.3).
  - Receivers cache the last `ACK` per peer and re-send it on duplicate `(src_id, tx_counter, seq_id)` without
    re-executing the relay pulse or incrementing `rejected_messages` (`I-29`).
- **Pre-Crypto Destination Filtering & Authenticated IP Updates (`I-53`, `I-62`):**
  - Frames whose cleartext `dst_id` does not match the local device are dropped before decryption and do not increment
    `rejected_messages` (`I-53`).
  - Peer IP routing table entries are updated **only** after AES-128-CCM authentication and monotonic replay checks pass
    **and** the authenticated payload's `sender_ipv4` matches the packet's `recvfrom` source IPv4 address (`I-62`).

---

## 6. Confirmable Heartbeat & System Health Monitoring (`I-12`, `I-41`, `I-42`, `I-52`, `I-58`, `I-64`)

- **Interval & Boot De-synchronization (`I-64`):** Default **`300 s`** (5 minutes, with $\pm 10\%$ random jitter;
  provisioned to modules during pairing `I-39`). After boot, the first heartbeat is sent after a uniform random delay in
  $[5\text{ s}, \text{Heartbeat interval}]$ to prevent a post-power-outage packet storm.
- **Confirmable Transport (`I-12`, `I-41`, `I-42`):** Every `HEARTBEAT_REQ` is acknowledged by the panel with an
  authenticated `HEARTBEAT_ACK` (refreshing the module's cached panel IP). If a paired module receives no authenticated
  `HEARTBEAT_ACK` for 3 consecutive intervals, its steady-state LED switches from `Dim solid (10%)` to `Sparse single
  flash (3000 ms)` to indicate that the panel is unreachable or the registration was deleted.
- **Payload Telemetry (`I-58`, `I-68`):** Uptime (`uptime_s`), last reset reason (`esp_reset_reason()` — note that on
  classic ESP32, `EN` pin resets report as `ESP_RST_POWERON`, `I-68`), received ring count, unacknowledged door-open
  requests, rejected messages, Wi-Fi disconnect count, AP signal strength (`rssi`), AP `bssid` and `channel`, current
  free heap, minimum free heap, firmware version, and configured `heartbeat_interval_s`.
- **Silence Restart & Persistent Backoff (`I-52`):**
  - If an apartment module misses 3 consecutive heartbeat intervals, the panel logs a `no report` warning.
  - When at least one apartment is registered and **no** module reports for 3 heartbeat intervals (**15 minutes** by
    default), the panel restarts itself. The backoff level is stored in `RTC_NOINIT_ATTR` protected by a magic word and
    CRC32 (`I-52`) so it survives software/watchdog restarts and doubles on each consecutive silence restart (30 min,
    1 h, 2 h … capped at 48 hours), resetting to 15 min upon the first authenticated module packet.

---

## 7. Hardware Peripherals, Safety Rules & ESP-IDF Configuration (`I-22`, `I-46`, `I-65`, `I-66`, `I-70`)

- **No Buzzer Hardware:** Neither the panel nor the apartment module has a buzzer.
- **Door & Bell Relay Boot-Safe Initialization (`I-22`):**
  - Before calling `gpio_config()` on the door relay (GPIO 23) or bell relay (GPIO 4), explicitly write the inactive
    output level via `gpio_set_level()` so the relay never glitches active during boot or brownout reset.
  - Relay de-energize (`500 ms` default) is driven by a dedicated one-shot `esp_timer` plus an independent safety clamp
    checked in the TWDT-monitored main loop so a single callback hang cannot leave the door unlocked.
- **Timebase & Compile-Time Validation (`I-65`, `I-66`):**
  - All long-duration timers (keypad lockout, silence restart backoff, heartbeat tracking) MUST use 64-bit microseconds
    from `esp_timer_get_time()` (`int64_t`) rather than 32-bit FreeRTOS tick counters that wrap at ~497 days (`I-65`).
  - Cross-parameter Kconfig constraints (e.g., `min_hold < max_hold < cancel_after`, `admin_code_len >= 8`, LED step
    $\ge \text{tick}$) MUST be enforced at compile time using `_Static_assert` in a shared header (`I-66`).
- **Task Watchdog (TWDT) Architecture (`I-46`):**
  - `CONFIG_ESP_TASK_WDT_PANIC=y`, timeout `5 s` (default).
  - Every task subscribed to TWDT MUST use bounded blocking calls (e.g., `SO_RCVTIMEO` $\le 2\text{ s}$) and feed the
    watchdog on each iteration. Register `esp_task_wdt_add_user()` health handles for periodic `esp_timer` and FreeRTOS
    timer pipelines so timer-task deadlocks also trigger a watchdog reset.
- **Summary of Intentional `sdkconfig` & Runtime Settings (`I-70`):**
  1. `CONFIG_ESP_TASK_WDT_PANIC=y` (default `n`; required for restart-on-hang).
  2. `CONFIG_LWIP_DHCP_RESTORE_LAST_IP=y` (default `n`; speeds up post-reboot IP recovery).
  3. `CONFIG_NVS_FLASH_VERIFY_ERASE=y` (default `n`; storage reliability).
  4. Runtime Wi-Fi API settings: `esp_wifi_set_ps(WIFI_PS_NONE)`, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`,
     `WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`, and `.max_connection = 4` on SoftAP.
  5. `CONFIG_DOORBELL_WIFI_STA_11B_ONLY=y` (ESP-IDF default protocol is 802.11b/g/n): the station interface runs
     `esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B)` so links use the low-SNR DSSS/CCK rates for longer
     reach. The building AP must accept 802.11b clients (no "disable 802.11b" / minimum basic rate > 11 Mbps).
  6. Flasher **Production** profile builds with `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`); the **Development**
     profile keeps the ESP-IDF default `-Og`. The `-Og` panel app fills 91.6 % of the 1 MB factory partition.
     Assertions stay enabled in both. The Production profile does not burn eFuses.
  7. Flasher **Burn** profile = Production `-Os` + the overlay `firmware/sdkconfig.burn` (Secure Boot v2 RSA-3072,
     release flash encryption, UART ROM download mode permanently disabled, NVS encryption with the flash-encryption
     key scheme, `CONFIG_ESP32_REV_MIN_3`) and the partition table `firmware/partitions_burn.csv` (table at
     `0x10000`, `nvs`, encrypted `nvs_keys`, factory app at `0x20000`). It builds in `firmware/<role>/build_burn/`
     and never touches the Development / Production build state. ADR 0015.

---

## 8. Finalized Security & Product Scope Decisions (ADR Summary)

All product-level and security-scope trade-offs from `docs/reviews/2026-11-06-design-review.md` have been reviewed and
finalized:

1. **`I-4` — Setup Portal Wi-Fi Security (Resolved — WPA3-SAE):**
   - `Doorbell-Setup` SoftAP uses **`WIFI_AUTH_WPA3_PSK` (SAE with `pmf_cfg.required = true`)** as in the canonical
     ESP-IDF v6.1 SoftAP example, neutralizing offline brute-force attacks against the 8-digit numeric admin code.
2. **`I-1` — Zero-Code Button Pairing Flow (Accepted Operational Scope):**
   - The manual's zero-code pairing workflow (`* <admin_code> * 1 * <apt_no> #` + physical button hold on the module at
     the panel + `X25519` + `HKDF-SHA256` + 1.5 s multi-module collision abort) is retained to keep manufacturing and
     field installation simple. The 20-second unauthenticated discovery window is recorded as an accepted operational
     risk mitigated by physical installer presence and the collision check.
3. **`I-7` & `I-18` — `OPEN_DOOR` Reboot Replay & Freshness Protection (Resolved — `panel_boot_id` + Challenge Nonce):**
   - The panel generates a random 32-bit `panel_boot_id` at every boot. If an `OPEN_DOOR_REQ` arrives with a stale
     `panel_boot_id` or requires freshness verification, the panel responds with `OPEN_DOOR_ACK (STATUS_CHALLENGE)`
     carrying `(panel_boot_id, challenge_nonce_64bit)` valid for `2000 ms`, and the apartment module transparently
     re-sends `OPEN_DOOR_REQ` echoing the challenge within the same button press (`docs/protocol.md` §3.1). This
     eliminates both post-reboot replay and withheld-packet replay with zero NVS wear.
4. **`I-3` & `I-5` — Two-Device Hardware Scope & Unique Factory Codes (Accepted Physical Scope):**
   - The two-device hardware architecture (door relay on Panel GPIO 23, hidden code-reset button on Panel GPIO 19, no
     third indoor relay controller or tamper switch GPIO) is fixed and recorded as an accepted physical enclosure
     boundary. Factory user and admin codes printed on each panel's label MUST be generated randomly per device during
     initial factory provisioning (never a shared static code across devices).
5. **`I-2`, `I-16`, `I-17` — Offline Operation, No OTA, Unencrypted Flash/NVS (Accepted Architectural Scope):**
   - **Development profile:** in accordance with ADR 0013 (offline system, no OTA, UART re-flash), Release-mode Flash
     Encryption, NVS Encryption, and Secure Boot v2 remain disabled (ESP-IDF defaults).
   - **Production profile:** superseded by [ADR 0015](docs/adr/0015-production-security-hardening.md) (Accepted,
     Option B) and implemented as the flasher **Burn** profile: Secure Boot v2 (RSA), release-mode flash encryption
     and NVS encryption, **no field updates**; no stored signing key (each Burn build signs with a one-time key and destroys it). The Production profile itself
     stays unhardened for bench testing of the release build. First hardware run: ADR 0015 §6.
6. **`I-6`, `I-19`, `I-44` — Shared 4-Digit User Code & Lockout Policy (Accepted Product Scope):**
   - The single building-wide 4-digit user code (`I-6`), reset-on-success lockout behavior (`I-19`), and shared `*`-prefix
     keypad lockout counter (`I-44`) are intentionally retained to preserve the exact end-user behavior specified in
     [kullanim-kilavuzu.md](kullanim-kilavuzu.md).
7. **`I-15` — No Panel Backup / Restore (Accepted Product Scope):**
   - The panel holds the only copy of the pairing database (apartment registrations and per-module keys). No backup,
     export, or restore mechanism will be implemented. If the panel is replaced or its NVS is lost, every apartment
     module must be reset and re-paired.
8. **`I-28`, `I-54`, `I-59`, `I-60` — Out of Project Scope:**
   - **`I-28` Building Wi-Fi credential rotation:** No credential-push mechanism. If the building AP SSID/passphrase
     changes, either keep the old SSID/passphrase on the new AP or reset and re-pair every module
     ([kullanim-kilavuzu.md](kullanim-kilavuzu.md) §4.2).
   - **`I-54` Event log / time source:** No persistent audit log and no real-time clock or SNTP; events are reported
     only on the panel serial monitor.
   - **`I-59` Visitor UX / accessibility:** No audible feedback (no buzzer hardware), no ring rate limiting, and no
     do-not-disturb mode on apartment modules.
   - **`I-60` Regulatory compliance:** CRA, RED Delegated Act / EN 18031, ETSI EN 303 645 and related radio/EMC/safety
     assessments are outside the scope of this project.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- When graphify-out/graph.json exists, run `graphify query "<question>"` before your first grep, search or source-file read for a codebase question, and put this rule in every subagent prompt that explores code. Read raw files only for lines the graph or an earlier read already located. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- A committed graphify-out/graph.json is rewritten whole on every build, so a large diff to it in a pull request is expected. Do not flag it as churn or suggest skipping the graph rebuild or its hooks.
