# DorbellAp Design Review — 2026-11-06

Scope: design-phase review of `AGENTS.md`, `kullanim-kilavuzu.md` and `yapilandirma-ayarlari.md` (no code exists yet). Platform claims were checked against the locally installed ESP-IDF v6.1 source tree and authoritative external sources.

## Method

- Seven independent reviewers: crypto/pairing security, device/physical security, networking, ESP-IDF platform, protocol state machines, documentation, product/operations.
- Each reviewer's findings were checked by a separate adversarial verifier (confirm / adjust / refute, severity recalibrated).
- A completeness critic searched for missed issues; its findings were verified the same way.
- A synthesis pass merged duplicates across dimensions and ranked the result.

Raw findings: 117 (+5 from the gap hunt); after merging: **72 issues** — 3 critical, 14 high, 43 medium, 12 low.

## Fix first (in order)

1. **I-8** — No wire-protocol specification: CCM nonce/AAD/tag/direction, message catalogue, ACK rules and version field are undefined, and modules cannot be updated in the field
2. **I-1** — Pairing is unauthenticated: open SoftAP plus anonymous X25519 hands the building PSK and an apartment key to an attacker or MITM relay
3. **I-2** — No flash/NVS encryption, Secure Boot or download-mode lockdown; every apartment key and the PSK sit in plaintext on the street panel and in every flat
4. **I-16** — Platform hardening is incompatible with the decided workflows (no OTA, reflash to change settings, NVS-image updates, default partition table)
5. **I-3** — Door-lock relay sits inside the street-side panel with no tamper detection
6. **I-4** — Setup-portal WPA2 passphrase is the 8-digit admin code; one captured handshake gives the admin code offline in under a minute
7. **I-5** — Factory-code provisioning undefined (possible shared defaults) and a hidden in-box reset gives admin rights and a persistent rogue-module backdoor
8. **I-13** — Copying the canonical nvs_flash_init() erase-on-error handler would wipe the building's pairing database
9. **I-7** — Receiver replay floor not persisted: a recorded OPEN_DOOR is accepted again after any panel reboot
10. **I-15** — The panel holds the only copy of pairing state; there is no backup, restore or panel-replacement procedure

## Strengths to keep

- Peer IP tables are updated only after AES-128-CCM authentication and the replay-counter check pass (AGENTS.md:121-123). Unauthenticated packets cannot redirect traffic; only the narrow relay case in I-62 remains.
- Every module has its own random 128-bit key, so deleting a registration revokes that module at the application layer at once (AGENTS.md:125-129).
- Pairing never overwrites an occupied apartment number, and it aborts when a second unpaired module shows up during the 1.5 s collision window (AGENTS.md:88-90, 104).
- tx_counter is persisted in blocks with skip-ahead after reboot (yapilandirma-ayarlari.md:58-59). This is the RFC 8613 App. B.1.1 pattern and only needs a normative write-ahead sentence.
- The deviations from IDF defaults are few and justified. WIFI_PS_NONE removes DTIM latency on mains-powered devices, and TWDT panic gives restart-on-hang (AGENTS.md:171-176).
- UDP port 19500 is IANA-unassigned and outside lwIP's ephemeral range. Limited broadcast needs no SO_BROADCAST in IDF 6.1 lwIP (IP_SOF_BROADCAST=0), so the fallback works with stock settings.
- Ringing keeps working during keypad lockout (kullanim-kilavuzu.md:91), which keeps the core visitor path available.
- Unverified values (400 ms ACK timeout, TWDT against X25519, LED polarity) are flagged 'to be measured' rather than presented as facts, in line with the data-driven rule.

## Issue index

| ID | Severity | Category | Title |
| :--- | :--- | :--- | :--- |
| [I-1](#i-1) | critical | security | Pairing is unauthenticated: open SoftAP plus anonymous X25519 hands the building PSK and an apartment key to an attacker or MITM relay |
| [I-2](#i-2) | critical | security | No flash/NVS encryption, Secure Boot or download-mode lockdown; every apartment key and the PSK sit in plaintext on the street panel and in every flat |
| [I-3](#i-3) | critical | security | Door-lock relay sits inside the street-side panel with no tamper detection |
| [I-4](#i-4) | high | security | Setup-portal WPA2 passphrase is the 8-digit admin code; one captured handshake gives the admin code offline in under a minute |
| [I-5](#i-5) | high | security | Factory-code provisioning undefined (possible shared defaults) and a hidden in-box reset gives admin rights and a persistent rogue-module backdoor |
| [I-6](#i-6) | high | security | Single building-wide 4-digit user code: guessable, leaks to everyone, cannot be revoked per person |
| [I-7](#i-7) | high | security | Receiver replay floor not persisted: a recorded OPEN_DOOR is accepted again after any panel reboot |
| [I-8](#i-8) | high | protocol | No wire-protocol specification: CCM nonce/AAD/tag/direction, message catalogue, ACK rules and version field are undefined, and modules cannot be updated in the field |
| [I-9](#i-9) | high | network | No installation requirements for the building AP/LAN the design depends on |
| [I-10](#i-10) | high | network | 'Wireless repeaters in bridge/AP mode every ~3 floors' is ambiguous, non-standard and degrades with each hop |
| [I-11](#i-11) | high | platform | STA reconnect policy undefined; the canonical IDF example gives up after 5 retries |
| [I-12](#i-12) | high | protocol | Heartbeat transport and IP rediscovery undefined; after a panel IP change heartbeats vanish and the silence-restart loop cannot fix it |
| [I-13](#i-13) | high | platform | Copying the canonical nvs_flash_init() erase-on-error handler would wipe the building's pairing database |
| [I-14](#i-14) | high | product-ops | The documented 'NVS partition generator' Wi-Fi update replaces the whole NVS partition, and the three docs list different update methods |
| [I-15](#i-15) | high | product-ops | The panel holds the only copy of pairing state; there is no backup, restore or panel-replacement procedure |
| [I-16](#i-16) | high | platform | Platform hardening is incompatible with the decided workflows (no OTA, reflash to change settings, NVS-image updates, default partition table) |
| [I-17](#i-17) | high | product-ops | No firmware update path; 2 MB flash with a 1 MB factory slot leaves thin headroom and makes adding OTA impractical |
| [I-18](#i-18) | medium | security | No freshness for OPEN_DOOR: a withheld, never-delivered request can be released later |
| [I-19](#i-19) | medium | security | Any resident's correct code resets the shared lockout escalation, so an outsider's guessing never escalates |
| [I-20](#i-20) | medium | protocol | Keypad grammar, verification order and wrong-attempt counting are undefined (possible lockout bypass, user/admin code overlap) |
| [I-21](#i-21) | medium | documentation | No hardware specification or lock-type scope; 'door stays locked on power loss' depends on an unspecified fail-secure lock |
| [I-22](#i-22) | medium | platform | Door-relay safe state across boot, reset, brownout and hangs is unspecified, and on-time has no independent bound |
| [I-23](#i-23) | medium | security | Secret deletion undefined: NVS erase leaves data in flash, the Wi-Fi driver keeps its own plaintext copies, and there is no decommission runbook |
| [I-24](#i-24) | medium | security | Unauthenticated UART esp_console with an unspecified command set; the canonical example exposes generic nvs_get/nvs_set |
| [I-25](#i-25) | medium | security | Canonical IDF Wi-Fi examples log the passphrase at INFO; copied code would print the admin code and building PSK |
| [I-26](#i-26) | medium | security | X25519 keypair may be generated while RF is off, when ESP32 RNG output is only pseudo-random |
| [I-27](#i-27) | medium | security | Pairing listener interface is unspecified; a LAN-side claimant would bypass the SoftAP-only collision check |
| [I-28](#i-28) | medium | product-ops | Building PSK cannot be rotated without re-pairing every module at the panel |
| [I-29](#i-29) | medium | protocol | Retries have no request identity, dedup rule or bound ACK, causing double rings and a false 'door not opened' |
| [I-30](#i-30) | medium | protocol | Pairing state machine undefined: commit point, rollback, PAIR_COMPLETE reply, module timeout, SSID and timing disagree across docs |
| [I-31](#i-31) | medium | network | Single-radio rule does not cover pairing, and pairing is allowed while the panel STA is disconnected |
| [I-32](#i-32) | medium | network | SoftAP default 192.168.4.0/24 can collide with the building LAN, so lwIP sends LAN traffic out of the SoftAP |
| [I-33](#i-33) | medium | network | Retries switch to broadcast only, the least reliable 802.11 path, and that path is never tested |
| [I-34](#i-34) | medium | network | 400 ms ACK timeout has no latency budget; ARP misses (10-entry cache, 1 s retry), DTIM and hops are unaccounted |
| [I-35](#i-35) | medium | network | Default WIFI_FAST_SCAN joins the first matching AP, not the strongest, and stationary modules never roam |
| [I-36](#i-36) | medium | network | Default country '01' scans channels 12-13 only passively; hidden SSIDs there are never found |
| [I-37](#i-37) | medium | network | No PMF/WPA3 guidance; deauthentication frames can silence ring and door-open |
| [I-38](#i-38) | medium | network | Network capacity limits undocumented: 254 apartments vs /24, AP association limits, DHCP pool, identical hostnames |
| [I-39](#i-39) | medium | product-ops | Settings that must match on all devices are compile-time Kconfig on a generic, non-updatable module firmware, with no mismatch detection |
| [I-40](#i-40) | medium | protocol | Setup portal applies untested credentials with no rollback and no user-visible result |
| [I-41](#i-41) | medium | product-ops | No liveness indication: the panel LED cannot show STA/AP loss, and the module's 'dim solid' reflects only AP association |
| [I-42](#i-42) | medium | protocol | Orphaned modules (deleted, building reset, panel replaced) are never told and keep looking healthy |
| [I-43](#i-43) | medium | protocol | No panel mode state machine: concurrency of pairing, portal, resets, code changes and lockout is unspecified |
| [I-44](#i-44) | medium | security | Keypad lockout is a building-wide DoS lever that also blocks the admin |
| [I-45](#i-45) | medium | security | Unauthenticated MAC address triggers automatic deletion of another apartment's registration |
| [I-46](#i-46) | medium | platform | TWDT plan targets X25519 (a non-issue) and misses blocking subscribed tasks and unwatched timer contexts |
| [I-47](#i-47) | medium | security | Setup-portal web server is reachable from the building LAN, bypassing the admin-code gate |
| [I-48](#i-48) | medium | product-ops | Installation acceptance test does not verify the apartment-number mapping |
| [I-49](#i-49) | medium | platform | ESP32-C3/S3 portability claim conflicts with the ESP32-only default GPIO map |
| [I-50](#i-50) | medium | platform | NVS write atomicity, failure handling and tx_counter write-ahead are unspecified |
| [I-51](#i-51) | medium | platform | Default 24 KB NVS cannot hold the documented 254 apartments plus IDF's own NVS users |
| [I-52](#i-52) | medium | protocol | Persistence of runtime state across reboot is unspecified (silence backoff, lockout, IP table) |
| [I-53](#i-53) | medium | protocol | No cleartext destination/key-id: broadcast retries pollute rejected_messages on every module and force trial decryption on the panel |
| [I-54](#i-54) | medium | product-ops | No persistent security event log, time source or registration audit |
| [I-55](#i-55) | medium | platform | No ESP-IDF or component versions pinned; crypto is named in legacy Mbed TLS terms although IDF 6.1 exposes it only via PSA |
| [I-56](#i-56) | medium | documentation | Traceability gaps: missing ADR 0013/0014, SPEC 9 and flashing GUI; no VCS, test strategy or bench plan |
| [I-57](#i-57) | medium | documentation | LED patterns lack priority/preemption rules and are inconsistent or defined in only one document |
| [I-58](#i-58) | medium | product-ops | Heartbeat and pairing carry no firmware/protocol version or BSSID/channel data |
| [I-59](#i-59) | medium | product-ops | Visitor-side UX and accessibility: no audible cue, one LED for 9+ states, no ring rate limit or do-not-disturb |
| [I-60](#i-60) | medium | product-ops | Regulatory applicability not decided (CRA, RED DA/EN 18031, EN 303 645, radio/EMC/safety) |
| [I-61](#i-61) | low | security | Default building PSK is a Kconfig string, so it lands in plaintext in sdkconfig and the app image |
| [I-62](#i-62) | low | security | Authenticated IP-table update can be redirected by relaying a not-yet-received frame from another IP |
| [I-63](#i-63) | low | protocol | Module button edge cases unspecified (pairing when already paired, both buttons, door button when unpaired or pending) |
| [I-64](#i-64) | low | network | Synchronized boot after a building power restore causes an association/DHCP storm and a heartbeat burst |
| [I-65](#i-65) | low | platform | Time base for long timers unspecified; 32-bit ticks at 100 Hz wrap after ~497 days |
| [I-66](#i-66) | low | platform | 'Impossible settings fail the build' has no mechanism; Kconfig falls back to the default and sub-tick LED steps assert at runtime |
| [I-67](#i-67) | low | documentation | 'Dim 10%' is ambiguous under led_indicator's default gamma correction (~0.4% duty) |
| [I-68](#i-68) | low | documentation | On ESP32 an EN-button restart is reported as power-on, and the 300 ms interrupt-WDT figure applies only without PSRAM |
| [I-69](#i-69) | low | documentation | 255.255.255.255 is mislabelled as the 'subnet broadcast address' |
| [I-70](#i-70) | low | documentation | The 'Only These Apply' list of IDF deviations is incomplete and includes a non-deviation |
| [I-71](#i-71) | low | documentation | Config reference conventions are inconsistent (Wi-Fi defaults in a 'panel ve modül' menu, missing defaults, no symbol table) |
| [I-72](#i-72) | low | documentation | Language rule leaves no room for Turkish installer-facing material, and an engineering requirement is traced to the manual |

## Critical

### I-1

**Pairing is unauthenticated: open SoftAP plus anonymous X25519 hands the building PSK and an apartment key to an attacker or MITM relay**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:82-103; kullanim-kilavuzu.md:115-121, 126-129; yapilandirma-ayarlari.md:53-56
- **Problem:** The pairing SoftAP is open and the X25519 exchange has no PoP, code or key confirmation. The documented order (panel command first, then a 7-12 s button hold) gives an automated station a head start of several seconds that the 1.5 s collision check cannot catch. With a rogue SoftAP the attacker can also relay both legs (default WIFI_FAST_SCAN takes the first SSID match), and both devices report success. The spec also names no KDF, AEAD, transcript binding or key confirmation (raw ECDH output in security1 style would be malleable).
- **Scenario:** During installation a laptop in the street claims the pairing window, or relays it, and receives the building PSK plus the apt-12 AES key. In the relay case the installer sees success on both devices, and the attacker can later forge OPEN_DOOR at will.
- **Recommendation:** Authenticate pairing with a per-module label code (≥8 digits, factory NVS) through PSA EC J-PAKE: CONFIG_MBEDTLS_ECJPAKE_C, PSA_ALG_JPAKE(PSA_ALG_SHA_256) on secp256r1. The built-in driver requires the IDs to be literally "client"/"server", so bind panel_id/module_id/apt_no in the HKDF info and in explicit key-confirmation MACs. Release the PSK and AES key only after confirmation, under psa_aead_encrypt with a version byte.
- **Evidence:** AGENTS.md:86-87 'open (unencrypted) pairing SoftAP'; :92 X25519 with no authentication; :103 no RSSI filter; station-scenarios.rst:346 (FAST_SCAN takes the first match); mbedtls/Kconfig:1131-1135 ECJPAKE default n; crypto_extra.h:694-714; security1.c:119,287,302-330; RFC 7748 §6.1; RFC 5869.
- **Source findings:** CRY-1, CRY-2, CRY-3

### I-2

**No flash/NVS encryption, Secure Boot or download-mode lockdown; every apartment key and the PSK sit in plaintext on the street panel and in every flat**

- **Category / kind:** security / missing
- **Locations:** AGENTS.md:45-46, 72-76, 125-129, 171-177; kullanim-kilavuzu.md:104-106, 151-153, 167
- **Problem:** The 'Only These Apply' deviation list leaves flash encryption, NVS encryption, Secure Boot v2, ROM download mode and JTAG at insecure defaults. `esptool read_flash` on the panel yields every apartment key, the codes and the PSK, and NVS can also be altered or new entries added. Every module also holds the PSK in a resident's flat, so deleting a registration revokes only the app key, not LAN access.
- **Scenario:** Someone opens the panel box for a few minutes, dumps the flash, and from then on sends authenticated OPEN_DOOR as any apartment from Wi-Fi range. Or an ex-tenant dumps their module and keeps the PSK and their apartment key.
- **Recommendation:** Before the first production flash, enable CONFIG_SECURE_FLASH_ENC_ENABLED (Release), CONFIG_NVS_ENCRYPTION (flash-enc scheme) with CONFIG_PARTITION_TABLE_SINGLE_APP_ENCRYPTED_NVS, CONFIG_SECURE_BOOT_V2_ENABLED (needs ESP32 ECO3, CONFIG_ESP32_REV_MIN_3) and SECURE_DISABLE_ROM_DL_MODE, on both targets, recorded in an ADR. Make 'delete + re-pair on tenant change' an explicit rule. Note the residual risk CVE-2023-35818 and evaluate C3/S3 (HMAC NVS keys, secure DL mode) for the panel.
- **Evidence:** nvs_flash.rst:102-103 ('read, alter, erase, or add'); bootloader/Kconfig.projbuild:860-862, 1239, 1283, 472, 499; nvs_flash/Kconfig:3-6 (no SOC_HMAC_SUPPORTED on ESP32); partitions_singleapp_encr_nvs.csv; security.rst:51,329; vulnerabilities.rst:213-220.
- **Source findings:** DEV-2, CRY-13

### I-3

**Door-lock relay sits inside the street-side panel with no tamper detection**

- **Category / kind:** security / weakness
- **Locations:** kullanim-kilavuzu.md:12, 144; yapilandirma-ayarlari.md:83-84; AGENTS.md:151-152
- **Problem:** The panel at the entrance contains the lock relay (GPIO23) and the strike wiring. Removing the faceplate lets anyone bridge the relay or strike wires and open the door with no credential. No tamper switch, tamper event or secure fasteners are specified, and the same access exposes the hidden reset button, UART and EN.
- **Scenario:** An intruder unscrews the panel front and shorts the strike wires. The door opens and nothing is logged.
- **Recommendation:** Use the canonical protected-side relay: a door-controller unit inside the building (e.g. a third ESP32 role paired like a module) that energizes the strike only on an authenticated, replay-checked OPEN. Add an enclosure tamper switch on a GPIO that logs, alerts and locks admin functions, plus tamper-resistant fasteners. If the relay stays in the panel, record it as an accepted risk.
- **Evidence:** kullanim-kilavuzu.md:12 (panel contains 'kapı kilidi rölesi'); yapilandirma-ayarlari.md:83 (relay GPIO23); no 'tamper'/'kurcalama' in any doc; 2N Security Relay pattern (wiki.2n.com).
- **Source findings:** DEV-4


## High

### I-4

**Setup-portal WPA2 passphrase is the 8-digit admin code; one captured handshake gives the admin code offline in under a minute**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:61-68; kullanim-kilavuzu.md:98-103; yapilandirma-ayarlari.md:25, 108
- **Problem:** Doorbell-Setup uses WPA2-PSK with the numeric admin code (10^8 by default) and the same SSID on every panel. Capturing the installer's 4-way handshake allows GPU cracking in about 40-45 s, and the keypad lockout does not apply. The form is plain HTTP inside the link, so the decrypted capture also reveals the new building PSK.
- **Scenario:** A sniffer near the entrance records a portal session and cracks the admin code with hashcat -m 22000. With it the attacker sets a user code and opens the door, or pairs a rogue module.
- **Recommendation:** Configure the setup AP like the IDF v6.1 softAP example: WIFI_AUTH_WPA3_PSK (SAE) with pmf_cfg.required=true, no WPA2 transition, and a per-device SSID (MAC suffix). Alternatively use espressif/network_provisioning with protocomm security2. Never echo stored secrets in the form, and remove the WPA2-length rationale at AGENTS.md:66.
- **Evidence:** AGENTS.md:63-67; wpa_auth.c:1786-1793 (no PMKID for WPA2-PSK, so a handshake capture is needed); softap_example_main.c:75-83 (WPA3_PSK, PMF required); esp_wifi/Kconfig:324-331; hashcat 22000 RTX 4090 ≈2.2-2.5 MH/s.
- **Source findings:** DEV-1

### I-5

**Factory-code provisioning undefined (possible shared defaults) and a hidden in-box reset gives admin rights and a persistent rogue-module backdoor**

- **Category / kind:** security / missing
- **Locations:** kullanim-kilavuzu.md:85, 143-145, 216; yapilandirma-ayarlari.md:3-4, 103-112; AGENTS.md:65-68, 167
- **Problem:** No doc requires factory codes to be random and unique per device, or says how they are generated and matched to the label, and changing them after installation is only advice. Holding the hidden button inside the street box restores the label codes and keeps all registrations, and the reset leaves no persistent record. Because the admin code also gates pairing and the portal, anyone who knows a shared default or has box access can pair a rogue module that survives later code changes.
- **Scenario:** An attacker opens the box, holds the reset for 15 s, and enters '* <factory admin> * 1 * 98 #' to pair their own module. Apartment 98 keeps opening the door after the owner resets the codes again.
- **Recommendation:** Generate per-device random factory codes at manufacture into a factory NVS partition (nvs_partition_gen / mfg_gen, encrypted per I-2), and print the label from the same record, placed inside the enclosure or on an owner card. Force a change on first use, log code resets persistently, and gate the reset button behind the tamper switch (I-3).
- **Evidence:** kullanim-kilavuzu.md:85, 144-145; yapilandirma-ayarlari.md:111-112 (codes loaded via the GUI form, no uniqueness rule); ETSI EN 303 645 5.1-1 (secondary source).
- **Source findings:** DEV-5, DOC-4, OPS-6

### I-6

**Single building-wide 4-digit user code: guessable, leaks to everyone, cannot be revoked per person**

- **Category / kind:** security / weakness
- **Locations:** kullanim-kilavuzu.md:11-13, 58-60, 88-90; yapilandirma-ayarlari.md:109, 114-124; AGENTS.md:104, 168
- **Problem:** The door code has 10^4 values and is shared by every resident, guest and courier, and it is typed in public where it can be seen or filmed. Under the lockout policy the full space falls in about 21 days, and the top-20 human PINs cover about 27%. Revoking one person means changing the code for the whole building, and the scope limits behind this choice are not recorded as decisions.
- **Scenario:** An ex-tenant or courier keeps entering for months. Or an attacker tries 1234/0000/building-number in one 16-minute session with about 1-in-4 odds against a human-chosen code.
- **Recommendation:** Use per-apartment door codes stored with each registration ('* <apt> * <code> #'), at least 6 digits, with trivial and sequential codes rejected. Write a tenant-turnover runbook. If one shared code is kept, default to 6 digits and record the risk in an ADR.
- **Evidence:** kullanim-kilavuzu.md:60 'binanın tamamı için tektir (4 hane)'; yapilandirma-ayarlari.md:118-121; lockout arithmetic ≈ 1.8×10^6 s; DataGenetics PIN distribution.
- **Source findings:** DEV-6, OPS-15

### I-7

**Receiver replay floor not persisted: a recorded OPEN_DOOR is accepted again after any panel reboot**

- **Category / kind:** security / missing
- **Locations:** AGENTS.md:120-128, 141-145, 172; kullanim-kilavuzu.md:162-168; yapilandirma-ayarlari.md:44, 58-59, 138-143
- **Problem:** Only the sender's tx_counter is persisted. If the panel keeps its per-module last-accepted counter in RAM, a captured OPEN_DOOR replays successfully after a power cut, a TWDT panic or the designed silence restart, which jamming can trigger. The window lasts until that module's next authenticated frame, and the attacker needs the PSK (I-2) to inject.
- **Scenario:** An ex-tenant with the PSK records a broadcast OPEN_DOOR retry and jams for 15 min to force the silence restart. They then replay the frame to UDP 19500 and the relay fires.
- **Recommendation:** Persist the per-module rx floor to NVS before acting on OPEN_DOOR (write-ahead), or require an RFC 9175 Echo-style challenge after boot. The OPEN_DOOR challenge-response in I-18 covers this as well.
- **Evidence:** AGENTS.md:127-128 (only tx_counter persisted); :143-145 silence restart; RFC 8613 §7.5 and line 2929 ('induce a device reboot for the purpose of replaying').
- **Source findings:** CRY-5

### I-8

**No wire-protocol specification: CCM nonce/AAD/tag/direction, message catalogue, ACK rules and version field are undefined, and modules cannot be updated in the field**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:22-23, 45, 100, 112-129; yapilandirma-ayarlari.md:3-5, 9-10, 33-34, 44
- **Problem:** Only message names and 'AES-128-CCM, one key per module' are given. With one key for both directions and a counter-only nonce, panel message #n and module message #n reuse a nonce, which RFC 3610 says destroys CCM security. Missing direction or sender fields also allow reflection of a module's own ACK. With no version field or catalogue and no OTA, any later wire-format fix breaks every installed module.
- **Scenario:** Panel RING #5 and module heartbeat #5 are encrypted under the same key and nonce, so the keystream is reused. Separately, a panel reflash that adds a request_id makes all 99 modules fail parsing and needs a visit to every flat.
- **Recommendation:** Write docs/protocol.md: a cleartext header {magic, version, msg_type, src/dst id (key-id), direction, 32-bit counter} carried as AAD; a 13-byte nonce = sender_id||direction||counter, or per-direction keys via HKDF as in OSCORE §3.2.1; a 16-byte tag (≥8). Include the message catalogue, ACK/dedup rules, a re-pair-before-exhaustion rule, and forward-compatibility rules (ignore unknown msg_type/TLV, version exchanged at pairing and in heartbeats). Implement with psa_aead_* PSA_ALG_CCM.
- **Evidence:** RFC 3610 §2 (nonce uniqueness; tags 4-16); RFC 8613 §5.2, §3.2.1; AGENTS.md:45 no OTA; yapilandirma-ayarlari.md:9-10; grep finds no nonce/header/version in any doc.
- **Source findings:** CRY-4, DOC-3, PRO-11

### I-9

**No installation requirements for the building AP/LAN the design depends on**

- **Category / kind:** network / missing
- **Locations:** AGENTS.md:36-38, 47-48; yapilandirma-ayarlari.md:29-34; kullanim-kilavuzu.md:15-17
- **Problem:** The docs state the goal (same subnet, broadcast reaches every floor) but not the AP settings that achieve it. Missing: a DHCP server (LWIP_AUTOIP is off), client isolation off, no broadcast filtering or proxy-ARP, WPA2/WPA3 security (the default authmode threshold refuses WEP/TKIP), repeaters on the same SSID/PSK, fixed channel, visible SSID, DTIM, association slots and DHCP pool.
- **Scenario:** An installer uses a guest SSID with client isolation enabled. Devices show 'in coverage' but every RING and OPEN_DOOR between stations is dropped.
- **Recommendation:** Turn the sentence into an explicit installation checklist in AGENTS.md §2, kullanim-kilavuzu.md and yapilandirma-ayarlari.md §1.1. Verify it at commissioning with a broadcast test.
- **Evidence:** AGENTS.md:36-38; lwip/Kconfig:465-467; esp_wifi_types_generic.h:374-375; RFC 9119 §4.1/§4.6.2.
- **Source findings:** NET-1

### I-10

**'Wireless repeaters in bridge/AP mode every ~3 floors' is ambiguous, non-standard and degrades with each hop**

- **Category / kind:** network / weakness
- **Locations:** AGENTS.md:36-38, 72-74; yapilandirma-ayarlari.md:29-31; kullanim-kilavuzu.md:15-17
- **Problem:** The phrase mixes wired-uplink APs with single-radio wireless repeaters. Wireless repeaters halve throughput per hop, need non-standard WDS/4-address mode for L2 bridging, and add one unacknowledged broadcast hop each. The docs never require repeaters to reuse the main SSID/PSK, although modules know only one SSID.
- **Scenario:** A 24-floor building uses chained consumer extenders with default '_EXT' SSIDs. Upper-floor modules cannot connect, and once the SSIDs are fixed, top-floor broadcasts cross about 8 lossy hops.
- **Recommendation:** Make the canonical choice wired-backhaul APs on one SSID/PSK/security (one ESS, channels 1/6/11, one L2 segment). Accept a validated single-vendor mesh with dedicated backhaul as the alternative. Allow wireless repeaters only with a hop limit of ≤1 and a commissioning broadcast test, and fix the Turkish wording.
- **Evidence:** AGENTS.md:37; kernel iw docs (WDS 'non-standard extension'); Cisco tech note 66091 (half throughput per repeater).
- **Source findings:** NET-2

### I-11

**STA reconnect policy undefined; the canonical IDF example gives up after 5 retries**

- **Category / kind:** platform / missing
- **Locations:** AGENTS.md:47-48, 69-71, 160-161; kullanim-kilavuzu.md:131, 191, 218
- **Problem:** In IDF, reconnecting is the application's job, and the getting_started/station example stops after CONFIG_ESP_MAXIMUM_RETRY=5. Copied into a module, any AP outage longer than a few seconds strands the device until it is power-cycled. Backoff, reason-code handling and socket re-creation are also unspecified.
- **Scenario:** The AP reboots at night for 2-3 min. Every module exhausts its retries, and the next morning no apartment can ring or open until all 99 are power-cycled.
- **Recommendation:** Specify that on WIFI_EVENT_STA_DISCONNECTED the module calls esp_wifi_connect() forever with capped exponential backoff and jitter (1 s→60 s) and logs the reason code. Re-create the UDP socket on GOT_IP/LOST_IP, pause reconnects only under the single-radio rule (portal and pairing), and explicitly forbid the example's give-up pattern.
- **Evidence:** station-scenarios.rst:880-889; station_example_main.c:78-85; station Kconfig.projbuild:37-41 (default 5).
- **Source findings:** NET-5

### I-12

**Heartbeat transport and IP rediscovery undefined; after a panel IP change heartbeats vanish and the silence-restart loop cannot fix it**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:47-48, 97-101, 116-123, 133-145; yapilandirma-ayarlari.md:32-34, 45-46, 138-143
- **Problem:** ACK, retry and broadcast fallback are defined only for RING and OPEN_DOOR. Modules learn the panel IP only from authenticated panel traffic, so after a lease change unicast heartbeats go to a stale address. The panel then sees silence and restarts on backoff without repairing anything. How a module learns the panel IP for PAIR_COMPLETE, and whether the IP table persists, are also undefined.
- **Scenario:** After an AP/router replacement or DHCP pool reset the panel moves from .10 to .23. All modules keep sending to .10, and the panel logs 'no report' for everyone and restarts at 15 min, 30 min, 1 h and onward.
- **Recommendation:** Make heartbeats confirmable with unicast-then-broadcast fallback, with the authenticated ACK refreshing the panel address (SIP REGISTER pattern). Have the panel send an authenticated ANNOUNCE at boot and on IP change, and include the panel's LAN IP in the pairing payload. Enable CONFIG_LWIP_DHCP_RESTORE_LAST_IP and require a DHCP reservation for the panel.
- **Evidence:** AGENTS.md:117-122, 143-145; lwip/Kconfig:372-379 (LWIP_DHCP_RESTORE_LAST_IP default n); RFC 3261 §10.
- **Source findings:** NET-4, PRO-8

### I-13

**Copying the canonical nvs_flash_init() erase-on-error handler would wipe the building's pairing database**

- **Category / kind:** platform / missing
- **Locations:** absent from all docs; AGENTS.md:21-23, 45-46, 153-156; kullanim-kilavuzu.md:19
- **Problem:** Every IDF Wi-Fi example calls nvs_flash_erase() on ESP_ERR_NVS_NO_FREE_PAGES or NEW_VERSION_FOUND, and the 'canonical pattern' rule pushes implementers toward it. NO_FREE_PAGES can occur after unexpected power loss, and NEW_VERSION_FOUND after flashing an older build. On the panel either one erases every registration, key and code.
- **Scenario:** A power cut during GC leaves NVS with no free page. The copied handler erases NVS on boot and every module must be re-paired.
- **Recommendation:** Add a binding rule that the panel never auto-erases NVS: it enters a logged 'storage fault' state and keeps flash contents for recovery. Enable CONFIG_NVS_FLASH_VERIFY_ERASE and pin the IDF version (I-55).
- **Evidence:** station_example_main.c:169-171; nvs_rw_value example :65-68; nvs_flash.rst:168, 174; nvs_flash/Kconfig:55-57.
- **Source findings:** PLT-2, OPS-1

### I-14

**The documented 'NVS partition generator' Wi-Fi update replaces the whole NVS partition, and the three docs list different update methods**

- **Category / kind:** product-ops / incorrect-claim
- **Locations:** AGENTS.md:45-46, 57-59; kullanim-kilavuzu.md:104-106; yapilandirma-ayarlari.md:4-5, 22
- **Problem:** nvs_partition_gen builds a complete partition image from a CSV, and the single default nvs partition also holds registrations, keys, counters and codes. Flashing a Wi-Fi-only image therefore wipes pairings and reverts changed codes to factory values. AGENTS.md lists 3 methods, the manual lists portal/UART/'flaşlama aracı', and the config doc lists portal and console only.
- **Scenario:** After an AP password change, a technician flashes a generated NVS image. The panel boots with Wi-Fi but no apartments, no keys and the factory codes.
- **Recommendation:** Restrict the generator to factory provisioning of blank devices. Use only the portal and console for field updates. If an offline image path is needed, use a separate Wi-Fi NVS partition (nvs_open_from_partition) under an ADR, and make all three docs list the same methods.
- **Evidence:** nvs_partition_generator/README.rst:9-11; partitions_singleapp.csv (single nvs 0x6000); nvs_flash.rst:174; AGENTS.md:59; kullanim-kilavuzu.md:106.
- **Source findings:** DEV-10, DOC-2, PLT-1

### I-15

**The panel holds the only copy of pairing state; there is no backup, restore or panel-replacement procedure**

- **Category / kind:** product-ops / missing
- **Locations:** kullanim-kilavuzu.md:112, 155-160; AGENTS.md:102-108
- **Problem:** Keys and registrations exist only in the panel's NVS, and the panel is the most exposed device (street side, weather, vandalism). There is no export or import, no commissioning backup and no replacement procedure. Pairing happens at the panel, so losing panel state means dismounting and re-pairing every module.
- **Scenario:** A surge destroys the panel after 3 years. The replacement is empty, and every module in the building must be carried to the entrance and re-paired.
- **Recommendation:** At minimum, document a commissioning backup (esptool read_flash of NVS, stored securely) and note that it conflicts with flash encryption, which binds data to the chip. Preferably add an authenticated, encrypted export/import of the pairing database through the portal or console, and add a 'Panel replacement' section to the manual.
- **Evidence:** kullanim-kilavuzu.md:112, 160; no backup/export text in any doc; IDF nvs_partition_tool exists.
- **Source findings:** OPS-3

### I-16

**Platform hardening is incompatible with the decided workflows (no OTA, reflash to change settings, NVS-image updates, default partition table)**

- **Category / kind:** platform / inconsistency
- **Locations:** AGENTS.md:45-46, 57-60; yapilandirma-ayarlari.md:3-5, 111-112, 216; kullanim-kilavuzu.md:106
- **Problem:** Every setting change requires a serial rebuild and reflash, but Release-mode flash encryption allows new plaintext images only via OTA, which ADR 0013 forbids. Disabling download mode disables esptool entirely, and NVS encryption needs an nvs_keys partition that the 'default table' rule excludes. Without a threat model the project will either ship unhardened (I-2) or lock itself out of field updates.
- **Scenario:** A year in, the team enables Release-mode encryption and Secure Boot. Setting changes and NVS images can no longer be flashed, and retrofitting older panels needs site visits and full re-pairing.
- **Recommendation:** Write a security ADR with a physical threat model. Move field-tunable parameters from Kconfig into NVS settings changed over an authenticated channel. Reconsider ADR 0013 (local signed updates with partitions_two_ota_encr_nvs.csv and Secure Boot v2), or document that hardened devices cannot be updated.
- **Evidence:** flash-encryption.rst:719 ('ONLY ... OTA'); security.rst:200-204; bootloader/Kconfig.projbuild:862-866; nvs_encryption.rst:32,39; yapilandirma-ayarlari.md:3-5.
- **Source findings:** DEV-3

### I-17

**No firmware update path; 2 MB flash with a 1 MB factory slot leaves thin headroom and makes adding OTA impractical**

- **Category / kind:** product-ops / weakness
- **Locations:** AGENTS.md:45-46; yapilandirma-ayarlari.md:3-5, 216
- **Problem:** Fixes can only be applied by opening every device and reflashing over UART, a visit to each occupied flat. Several defaults are still unmeasured, and IDF releases reach end of life after 30 months. The 2 MB flash (the IDF default, not tied to the chosen module) has no image-size budget: a minimal PSA http_server build is already 826,909 B against the 1 MB slot, so a two-slot layout is barely possible.
- **Scenario:** A Mbed TLS CVE appears, or the 400 ms timeout fails behind repeaters. Fixing a 99-flat building needs 99 appointments, so most units are never updated.
- **Recommendation:** Revisit ADR 0013 before the hardware freeze. At minimum, specify 4 MB flash to match common modules (e.g. WROOM-32E-N4) so a standard two_ota layout remains possible, and add an idf.py size budget check. If OTA is adopted, use LAN-only esp_ota_ops with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and signed images.
- **Evidence:** esptool_py/Kconfig.projbuild:125 (2MB default); partition_table/Kconfig.projbuild:41-56; security.rst (http_server/simple 826,909 B); versions.rst:85,111.
- **Source findings:** OPS-2, PLT-7


## Medium

### I-18

**No freshness for OPEN_DOOR: a withheld, never-delivered request can be released later**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:116-128, 162-165; kullanim-kilavuzu.md:48-51
- **Problem:** Counter-only replay protection gives no freshness, so an OPEN_DOOR that never reached the panel stays valid until a later frame from that module is accepted. An attacker with the PSK and an evil-twin AP can capture it, suppress the module's later traffic and release it later. The preconditions limit how practical this is.
- **Scenario:** Module 7 joins an evil twin. The resident's OPEN_DOOR is captured and dropped, and hours later the attacker sends it to the panel and the door opens.
- **Recommendation:** Make OPEN_DOOR a challenge-response: REQ, then a fresh panel nonce from psa_generate_random valid for 1-2 s, then OPEN_DOOR echoing the nonce (RFC 9175 Echo). This also fixes I-7. Re-measure the ACK budget afterwards.
- **Evidence:** RFC 9175 §2.1-2.2; RFC 8613 §12.5; RollJam (DEF CON 23).
- **Source findings:** CRY-6

### I-19

**Any resident's correct code resets the shared lockout escalation, so an outsider's guessing never escalates**

- **Category / kind:** security / weakness
- **Locations:** kullanim-kilavuzu.md:60, 87-92; yapilandirma-ayarlari.md:114-124; AGENTS.md:168-169
- **Problem:** The lockout level is building-wide and any correct code resets it. Residents' normal use therefore keeps an attacker at the 60 s step, cutting the worst case from about 21 days to about 33 h (mean about 17 h). Key wear on a single shared code reduces the search to 24 orderings.
- **Scenario:** Over a few evenings an attacker guesses at the street keypad between residents' entries and finds the code.
- **Recommendation:** Let the escalation level decay only with time (e.g. reset after N hours without failures), keep separate admin and user counters, and record a brute-force budget as a requirement. Pair this with per-apartment codes (I-6).
- **Evidence:** kullanim-kilavuzu.md:88-90; yapilandirma-ayarlari.md:124; 2,000×60 s = 120,000 s.
- **Source findings:** GAP-3

### I-20

**Keypad grammar, verification order and wrong-attempt counting are undefined (possible lockout bypass, user/admin code overlap)**

- **Category / kind:** protocol / missing
- **Locations:** kullanim-kilavuzu.md:25-37, 56-61, 73-92; yapilandirma-ayarlari.md:107-112; AGENTS.md:60, 82, 107, 168
- **Problem:** Commands *2/*3/*4 exist only in the Turkish manual, and 'malformed' versus 'wrong code' is undefined. An entry like '* <guess> * 9 #' might be treated as malformed and never counted toward lockout. User code length (4-8) overlaps admin length (8-10), nothing forbids user == admin or trivial codes, and the meaning of '* <admin> #' and apartment-number parsing are unspecified.
- **Scenario:** An implementation that verifies the code before rejecting unknown command 9 gives unlimited, uncounted admin-code guesses. Or a manager sets user == admin, and every resident gains admin rights.
- **Recommendation:** Add an English EBNF grammar with the order syntax check → code check → execute. Any well-formed entry with a wrong code counts as a wrong attempt and shows the same error LED. Reject user == admin and trivial patterns, define '* <admin> #' as an error, and define apartment-number parsing.
- **Evidence:** kullanim-kilavuzu.md:77-80, 92; yapilandirma-ayarlari.md:108-109.
- **Source findings:** PRO-14, DOC-8, DEV-14

### I-21

**No hardware specification or lock-type scope; 'door stays locked on power loss' depends on an unspecified fail-secure lock**

- **Category / kind:** documentation / missing
- **Locations:** kullanim-kilavuzu.md:10-19, 167-168; yapilandirma-ayarlari.md:79-88, 101, 154, 191; AGENTS.md:151-152
- **Problem:** There is no schematic, relay or lock ratings, power or enclosure spec, and no supported lock type. The manual promises locking on power loss, which is false for a fail-safe maglock, and such a lock would also need REX and fire-release inputs. Relay polarity defaults differ per device (a wrong setting holds the lock open), LED polarity is unverified, and the 0.5 s release has not been validated against real strikes.
- **Scenario:** An installer fits a maglock. The door unlocks on every power cut and has no fire-alarm release.
- **Recommendation:** Add an English hardware spec (schematic, BOM, ratings, boot-time pin states) and an installation guide. State the supported lock (fail-secure strike with mechanical free egress) or add REX, fire and door-contact inputs. Qualify the manual's claim and validate the pulse on the bench.
- **Evidence:** kullanim-kilavuzu.md:19; yapilandirma-ayarlari.md:84 ('kilit beklemede açık durur'), :101, :191.
- **Source findings:** DOC-11, OPS-13

### I-22

**Door-relay safe state across boot, reset, brownout and hangs is unspecified, and on-time has no independent bound**

- **Category / kind:** platform / missing
- **Locations:** yapilandirma-ayarlari.md:83-84, 101; AGENTS.md:149-152, 170-173; kullanim-kilavuzu.md:19
- **Problem:** GPIO23 floats between reset and app init, the GPIO_OUT reset value is undefined, and configuring the pin as output before setting its level can glitch the active-low 'open' level. If the relay-off path blocks rather than spins, the TWDT idle check never fires and the door can stay unlocked without bound.
- **Scenario:** Solenoid inrush triggers brownout resets in a loop, and each boot glitches the relay. Or a blocked esp_timer callback leaves the strike energized.
- **Recommendation:** Call gpio_set_level(inactive) before gpio_config, and add an external pull to the inactive level. De-energize from a path independent of application code (a dedicated one-shot timer plus a TWDT-subscribed max-on check, or a hardware max-on circuit), and bench-test power-up, EN, brownout and panic.
- **Evidence:** soc/esp32/register/soc/gpio_reg.h:18-21 ('default: x'); esp_system/Kconfig:342-345.
- **Source findings:** GAP-4

### I-23

**Secret deletion undefined: NVS erase leaves data in flash, the Wi-Fi driver keeps its own plaintext copies, and there is no decommission runbook**

- **Category / kind:** security / missing
- **Locations:** AGENTS.md:56, 65-68, 73-74, 107-108, 129; kullanim-kilavuzu.md:143-160
- **Problem:** With NVS_READWRITE, erased or overwritten keys, codes and PSKs remain readable in flash. The driver's default WIFI_STORAGE_FLASH also stores sta.pswd and ap.passwd (the admin code) in nvs.net80211, which app-level resets do not touch. A module reset leaves its key valid on the panel, and no lost-module, move-out or full-wipe procedure exists.
- **Scenario:** A reset module is sold or thrown away. A flash dump yields the PSK and a key the panel still accepts.
- **Recommendation:** Call esp_wifi_set_storage(WIFI_STORAGE_RAM) (or set CONFIG_ESP_WIFI_NVS_ENABLED=n), and open secrets with NVS_READWRITE_PURGE or call nvs_purge_all. Implement module reset and a new panel full-wipe with nvs_flash_erase_partition() plus esp_wifi_restore(), and add runbooks for lost modules and move-outs. Combine with I-2.
- **Evidence:** nvs_flash.rst:66-70, 110-124; nvs.h:89; esp_wifi.h:460-473, 1108-1112; esp_wifi/Kconfig:221-225; overview.rst:1294; kullanim-kilavuzu.md:152.
- **Source findings:** DEV-12, CRY-15, PLT-9, OPS-10

### I-24

**Unauthenticated UART esp_console with an unspecified command set; the canonical example exposes generic nvs_get/nvs_set**

- **Category / kind:** security / missing
- **Locations:** AGENTS.md:57-58, 156; kullanim-kilavuzu.md:106, 205; yapilandirma-ayarlari.md:22, 215
- **Problem:** The console is a documented field path, but the docs give no command set, no authentication and no secret read-back rules. esp_console has no authentication, and the advanced example's cmd_nvs would read and write codes and keys through the running app, defeating flash encryption.
- **Scenario:** Someone with a USB-UART adapter in the box runs nvs_get to read the admin code, or nvs_set to install their own key.
- **Recommendation:** Define a minimal write-only command set (set Wi-Fi, non-secret status, list registrations) and exclude cmd_nvs. Gate state changes by the admin code plus tamper/button, rate-limit attempts, and consider a production build with the REPL disabled.
- **Evidence:** components/console (no auth); examples/system/console/advanced/components/cmd_nvs/cmd_nvs.c:548-593.
- **Source findings:** DEV-9

### I-25

**Canonical IDF Wi-Fi examples log the passphrase at INFO; copied code would print the admin code and building PSK**

- **Category / kind:** security / risk
- **Locations:** AGENTS.md:21-23, 88; kullanim-kilavuzu.md:205-208; yapilandirma-ayarlari.md:217
- **Problem:** The softAP, station and softap_sta examples log 'SSID:%s password:%s' with ESP_LOGI, and the default log level is Info. The serial monitor is the documented troubleshooting tool, so logs end up on installers' laptops and in tickets.
- **Scenario:** A contractor's monitor log containing 'password:<admin code>' and the STA PSK is attached to a support ticket.
- **Recommendation:** Add a rule to AGENTS.md: never log or echo codes, passphrases, keys or nonces at any level, and strip password arguments when adapting examples.
- **Evidence:** softap_example_main.c:101-102; station_example_main.c:155,158; softap_sta.c:138,248,252.
- **Source findings:** DEV-11

### I-26

**X25519 keypair may be generated while RF is off, when ESP32 RNG output is only pseudo-random**

- **Category / kind:** security / risk
- **Locations:** AGENTS.md:73, 93-96; kullanim-kilavuzu.md:117-121
- **Problem:** Pre-generating the key during the button hold carries no entropy-source requirement. In IDF 6.1 the PSA RNG calls esp_fill_random directly with no DRBG, and the output is documented as pseudo-random after app start unless RF or bootloader_random_enable() is active. No practical attack on the seeded state is demonstrated.
- **Scenario:** An unpaired module keeps Wi-Fi stopped and generates its key during the hold, so key quality rests only on the bootloader-seeded RNG state.
- **Recommendation:** Require key and nonce generation only after esp_wifi_start(), or inside bootloader_random_enable()/disable(). The simplest option is to start STA at boot on unpaired modules too.
- **Evidence:** mbedtls esp_config.h:38-39; esp_hardware.c:22-31; random.rst:41-51, 80, 98.
- **Source findings:** PLT-4, CRY-12

### I-27

**Pairing listener interface is unspecified; a LAN-side claimant would bypass the SoftAP-only collision check**

- **Category / kind:** security / missing
- **Locations:** AGENTS.md:84-101; yapilandirma-ayarlari.md:53-56
- **Problem:** The panel must listen on the LAN for PAIR_COMPLETE, but the docs do not say which interface the pairing service binds to. A wildcard bind, combined with lwIP's weak host model, would accept pairing requests from a PSK holder on the LAN, and the collision check counts only SoftAP stations. This widens I-1.
- **Scenario:** A resident's device on the building Wi-Fi sends the pairing request to the panel's LAN IP when the window opens and receives the apt key.
- **Recommendation:** Bind the pairing listener to the SoftAP netif (SO_BINDTODEVICE or 192.168.4.1) and accept pairing only from currently associated SoftAP stations. On the LAN socket accept only PAIR_COMPLETE authenticated under the new key.
- **Evidence:** lwip/Kconfig:259-261 (IP_FORWARD off); ip4.c:435,616 (weak host); httpd_main.c:369-380 (wildcard bind pattern).
- **Source findings:** GAP-2

### I-28

**Building PSK cannot be rotated without re-pairing every module at the panel**

- **Category / kind:** product-ops / weakness
- **Locations:** AGENTS.md:72-76; kullanim-kilavuzu.md:94-106, 112
- **Problem:** Replacing the AP has a documented workaround (reuse the SSID/PSK). Rotating a leaked PSK, after a lost module or a departing tenant, requires physically re-pairing every module. The portal changes only the panel and gives no warning at save time.
- **Scenario:** After a tenant leaks the PSK, the admin changes it in the portal. All 99 modules go offline until each one is re-paired.
- **Recommendation:** Show a warning in the portal. Optionally add an authenticated credential-update message: panel → each module under its key, ACKed, stored as pending with fallback to the old credentials, with the panel switching last. Record the rotation policy in an ADR.
- **Evidence:** AGENTS.md:75-76; kullanim-kilavuzu.md:101-105.
- **Source findings:** NET-15, OPS-4

### I-29

**Retries have no request identity, dedup rule or bound ACK, causing double rings and a false 'door not opened'**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:116-123, 162-165, 170; yapilandirma-ayarlari.md:42, 48-51, 137, 166, 183; kullanim-kilavuzu.md:48-51, 61
- **Problem:** Retries are defined neither as identical frames nor as re-encryptions. Identical frames are rejected as replays with no ACK, while new counters make retries fresh commands. The ACK is not specified as authenticated or bound to a request, and it has no result code, so 'ignored, relay active' cannot be expressed.
- **Scenario:** The door opens but its ACK is lost. The retry, about 400 ms later inside the 500 ms pulse, is ignored or rejected without an ACK, so the module blinks 'not opened' and the resident presses again. A lost RING ACK rings the bell twice.
- **Recommendation:** Use the CoAP confirmable pattern (RFC 7252 §4.2/§4.5): retries are the byte-identical frame, and the receiver caches the last ACK per peer and resends it for an exact duplicate without re-executing or counting it as rejected. Every ACK is a full CCM frame with its own counter, echoing (msg_type, counter) and a result code (OK/ALREADY_ACTIVE/REJECTED), sent on receipt before the pulse.
- **Evidence:** AGENTS.md:118-123, 170; RFC 9175 §4.1; RFC 7252 §4.5.
- **Source findings:** PRO-1, CRY-7, CRY-9

### I-30

**Pairing state machine undefined: commit point, rollback, PAIR_COMPLETE reply, module timeout, SSID and timing disagree across docs**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:82-106; yapilandirma-ayarlari.md:43, 53-56; kullanim-kilavuzu.md:112-137, 152-153
- **Problem:** The docs do not say when either side commits to NVS, how PAIR_COMPLETE is answered, what is rolled back, or when the old same-MAC record is removed. AGENTS.md and the config reference give different window-close triggers. There is no module search timeout or operation order, the 'default' SoftAP SSID ESP_xxxxxx cannot be told apart from other ESP devices, and the +10 s and 20 s budgets are unmeasured.
- **Scenario:** The panel commits but its reply is lost, so the module wipes its key while the apartment shows 'occupied'. Or a failed re-pair has already deleted the old same-MAC record, and both records are lost.
- **Recommendation:** Specify one state machine per role. The panel holds the pending record in RAM, commits atomically on a valid PAIR_COMPLETE, removes the old same-MAC record only after commit, and replies with an idempotent PAIR_ACK; the module shows success only after PAIR_ACK. Use a distinctive pairing SSID (project prefix plus MAC suffix), define the module timeout and both orders, include the panel's LAN IP, and make the bench-measured completion timeout a Kconfig value.
- **Evidence:** AGENTS.md:98-101, 105-106; yapilandirma-ayarlari.md:54-55; kullanim-kilavuzu.md:126-128; overview.rst:537; esp_wifi_types_generic.h:194.
- **Source findings:** PRO-2, PRO-3, DOC-7

### I-31

**Single-radio rule does not cover pairing, and pairing is allowed while the panel STA is disconnected**

- **Category / kind:** network / missing
- **Locations:** AGENTS.md:69-71, 86-90, 97-101; kullanim-kilavuzu.md:112-113, 214
- **Problem:** The pause of scans and reconnects applies only to the portal. 'Same channel as STA' is undefined when the STA is disconnected, and in APSTA the STA channel wins, so a reconnect moves the SoftAP via CSA in the middle of the exchange. The manual only asks the installer to 'make sure' the panel is connected.
- **Scenario:** Pairing starts while the panel is still reconnecting. The STA then joins on channel 11, and the module is dropped mid-X25519.
- **Recommendation:** Reject the pairing command with the error pattern unless the STA has an IPv4 lease, and apply the single-radio rule from the command until PAIR_COMPLETE or timeout. Abort if the STA drops, and pass the AP channel to the module as a wifi_sta_config_t.channel hint.
- **Evidence:** overview.rst:420, 1145; AGENTS.md:69-71, 86-87.
- **Source findings:** NET-9, PRO-5

### I-32

**SoftAP default 192.168.4.0/24 can collide with the building LAN, so lwIP sends LAN traffic out of the SoftAP**

- **Category / kind:** network / risk
- **Locations:** AGENTS.md:64, 86-90, 99-101; kullanim-kilavuzu.md:101; yapilandirma-ayarlari.md:25
- **Problem:** The pairing and portal SoftAP use 192.168.4.1/24 while the STA keeps its building lease. ip4_route returns the first up netif with a matching subnet, and the most recently added AP netif sits at the head of the list. If the building LAN is also 192.168.4.0/24, unicast to LAN peers leaves through the SoftAP.
- **Scenario:** In a building whose router hands out 192.168.4.x, PAIR_ACK and RINGs during the window are misrouted, so pairing fails every time.
- **Recommendation:** Before starting the SoftAP, compare it with the STA subnet. On overlap move the AP to another private /24 with esp_netif_dhcps_stop() → esp_netif_set_ip_info() → esp_netif_dhcps_start() (as in the captive_portal example) and show the new address. Alternatively list 'LAN must not be 192.168.4.0/24' as an installation requirement, and bring the SoftAP down as soon as discovery ends.
- **Evidence:** esp_netif_defaults.c:22-26; esp_netif_lwip.c:1236; netif.c:436-438; ip4.c:171-177; esp_netif_defaults.h:54,69.
- **Source findings:** NET-8, PLT-12, PRO-6

### I-33

**Retries switch to broadcast only, the least reliable 802.11 path, and that path is never tested**

- **Category / kind:** network / weakness
- **Locations:** AGENTS.md:116-119, 175-176; yapilandirma-ayarlari.md:32-34, 48-51
- **Problem:** After one unicast attempt, all retries go to 255.255.255.255. The AP's downlink copy is unacknowledged, sent at a basic rate, and DTIM-buffered if any station is in power save. A transient unicast loss therefore moves to a weaker channel, and broadcast filtering only shows up after an IP change.
- **Scenario:** The panel's stored IP for apt 12 is stale after a DHCP reshuffle. Both broadcast retries are lost or delayed past 400 ms, and the visitor sees 'not responding'.
- **Recommendation:** On each retry send both a unicast copy and a broadcast copy, with dedup per I-29. Add a commissioning and maintenance broadcast self-test, and recommend DTIM 1 and a dedicated SSID.
- **Evidence:** RFC 9119 §3.1.1, §3.1.2, §3.1.4; low-power-mode-wifi.rst:31.
- **Source findings:** NET-3

### I-34

**400 ms ACK timeout has no latency budget; ARP misses (10-entry cache, 1 s retry), DTIM and hops are unaccounted**

- **Category / kind:** network / risk
- **Locations:** AGENTS.md:117-119; yapilandirma-ayarlari.md:35, 42, 48-51, 53-56
- **Problem:** lwIP's fixed 10-entry, 300 s ARP cache means most panel-to-module unicasts need ARP first, and a lost ARP request is retried only at the 1 s timer. DTIM, repeater hops and off-channel scans can also exceed 400 ms. Bench tests with 2-3 modules will not exercise any of this, and the +10 s pairing completion relies on the short DHCP ARP check (ACD would add seconds).
- **Scenario:** A RING to an apartment not in the ARP cache loses its ARP broadcast. Attempt 1 times out, the later retries arrive late, and the panel shows 'not responding' while the bell actually rings.
- **Recommendation:** Define a per-attempt latency budget and an acceptance test: p99 RTT measured with more than 10 peers and idle periods over 300 s through the worst repeater chain, with the timeout set to ≥2×p99. Keep CONFIG_LWIP_DHCP_DOES_ARP_CHECK and do not select ACD, and absorb ARP misses with I-33 rather than patching lwIP.
- **Evidence:** lwip opt.h:628-637; etharp.h:64; etharp.c:227-229; acd_dhcp_check.c:16-20; lwip/Kconfig:330-343.
- **Source findings:** NET-12, PLT-11

### I-35

**Default WIFI_FAST_SCAN joins the first matching AP, not the strongest, and stationary modules never roam**

- **Category / kind:** network / weakness
- **Locations:** AGENTS.md:36-38, 47-48, 138-139; yapilandirma-ayarlari.md:29-31
- **Problem:** With several APs on one SSID, the fast scan stops at the first match (sort_method applies only to ALL_CHANNEL_SCAN, and the RSSI threshold is -127). A module that fell back to a weak AP stays on it indefinitely. RSSI is telemetry only, with no threshold, and a neighbouring building using the same SSID is not handled.
- **Scenario:** The floor-6 repeater reboots. Floor-7 modules move to the main AP at -88 dBm and stay there.
- **Recommendation:** Set scan_method=WIFI_ALL_CHANNEL_SCAN, sort_method=WIFI_CONNECT_AP_BY_SIGNAL, a documented threshold.rssi and failure_retry_cnt>0 on both targets. Add an RSSI commissioning criterion. Optionally enable the IDF roaming app (low-RSSI trigger only, bench-verified) or 802.11k/v, and require a building-unique SSID.
- **Evidence:** esp_wifi_types_generic.h:358, 584; overview.rst:502-520; station-scenarios.rst:346; esp_wifi/Kconfig:754-760.
- **Source findings:** NET-6

### I-36

**Default country '01' scans channels 12-13 only passively; hidden SSIDs there are never found**

- **Category / kind:** network / risk
- **Locations:** AGENTS.md:177; yapilandirma-ayarlari.md:26-27
- **Problem:** With the country option off, IDF uses '01' (ch 1-11 active, 12-14 passive, AUTO policy), so a hidden SSID on ch 12/13 is never found. The docs do not explain the default. They cite 'TR', which is missing from the esp_wifi.h list although the 2.4 GHz regulatory table includes it (ch 1-13, 20 dBm).
- **Scenario:** The AP auto-selects channel 13 with a hidden SSID, and the panel and modules never associate.
- **Recommendation:** Make a visible SSID on a fixed channel 1/6/11 an installation requirement. If ch 12/13 is used, enable the Kconfig option with 'TR' on both targets and bench-verify esp_wifi_set_country_code and APSTA on ch 13. Document the default in yapilandirma-ayarlari.md.
- **Evidence:** esp_wifi.h:861, 1546-1551; overview.rst:960-967, 997-1007, 1026-1028; esp_wifi_regulatory.c:1236, 1331-1336.
- **Source findings:** NET-7, PLT-14

### I-37

**No PMF/WPA3 guidance; deauthentication frames can silence ring and door-open**

- **Category / kind:** network / missing
- **Locations:** AGENTS.md:55, 63-66, 171-177; yapilandirma-ayarlari.md:23
- **Problem:** Ring and door-open depend entirely on association, but only WPA2 is specified. Without 802.11w PMF, spoofed deauth frames can keep every device disconnected. IDF defaults to PMF optional, so 'required' is an opt-in.
- **Scenario:** Someone runs a deauth tool from the street. Rings and door-opens fail, and only reconnect logs show anything.
- **Recommendation:** Require the AP to run WPA3-Personal or WPA2/WPA3 transition with PMF capable, preferably required. Set pmf_cfg.required=true once the AP is verified, count disconnect reasons in telemetry, and document jamming as a residual risk.
- **Evidence:** wifi-security.rst:20-37; esp_wifi_types_generic.h:498-502; esp_wifi/Kconfig:300-306.
- **Source findings:** NET-10

### I-38

**Network capacity limits undocumented: 254 apartments vs /24, AP association limits, DHCP pool, identical hostnames**

- **Category / kind:** network / missing
- **Locations:** yapilandirma-ayarlari.md:67-68, 107; AGENTS.md:36-38, 135-136
- **Problem:** Up to 254 apartments are allowed, but there is no station or DHCP budget: a /24 cannot hold 254 modules plus infrastructure, and APs and pools have vendor-specific limits. All devices report the hostname 'espressif'.
- **Scenario:** In a 120-flat building with a 100-address DHCP pool, modules 101-120 never get an IP, and the installer cannot tell which 'espressif' lease is which.
- **Recommendation:** Document a capacity budget (modules + panel + repeaters ≤ association limit and DHCP pool; /23 beyond about 250 hosts) and add it to the checklist. Set hostnames with esp_netif_set_hostname() ('doorbell-panel', 'doorbell-apt-012').
- **Evidence:** yapilandirma-ayarlari.md:107; lwip/Kconfig:15-16.
- **Source findings:** NET-11

### I-39

**Settings that must match on all devices are compile-time Kconfig on a generic, non-updatable module firmware, with no mismatch detection**

- **Category / kind:** product-ops / inconsistency
- **Locations:** yapilandirma-ayarlari.md:3-5, 9-10, 45-46, 69, 138; AGENTS.md:45, 56, 73-74, 114
- **Problem:** UDP port, heartbeat enable/interval, ACK timeout and block size must be identical everywhere but are compiled in. A per-site change makes stock spare modules incompatible, and nothing detects or logs a mismatch. The consequences of disabling heartbeat (no 'no report' warnings, no liveness, slower IP learning) are undocumented.
- **Scenario:** A replacement module built with heartbeat 1200 s joins a 300 s panel. 'No report' warnings fire forever, or a port mismatch makes every RING fail silently.
- **Recommendation:** Freeze the shared values as protocol constants, or have the panel provision them during pairing and store them in NVS. Carry the protocol version and heartbeat interval in heartbeats so the panel computes per-module thresholds and logs mismatches, and document what is lost when heartbeat is disabled.
- **Evidence:** yapilandirma-ayarlari.md:3-4, 9-10, 69, 138; AGENTS.md:73.
- **Source findings:** DOC-1, PLT-13, OPS-5, PRO-17

### I-40

**Setup portal applies untested credentials with no rollback and no user-visible result**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:61-71; kullanim-kilavuzu.md:98-106, 180
- **Problem:** On save the portal closes and the panel switches to the new SSID/PSK without testing them, keeping the old ones or reporting the outcome. The LED has no steady 'portal open' state, so the installer cannot tell whether the 120 s window is still running.
- **Scenario:** A mistyped passphrase is saved. The panel never reconnects, the LED shows idle, and the whole building fails until someone notices.
- **Recommendation:** Try the new credentials with esp_wifi_set_config while the SoftAP is still up, wait for IP_EVENT_STA_GOT_IP with a timeout, then persist and close the portal. Otherwise restore the old credentials and report the failure over HTTP and on the LED. Add a steady 'portal active' LED pattern and distinct outcome patterns.
- **Evidence:** kullanim-kilavuzu.md:102, 180; provisioning.rst:9 (network_provisioning).
- **Source findings:** PRO-13

### I-41

**No liveness indication: the panel LED cannot show STA/AP loss, and the module's 'dim solid' reflects only AP association**

- **Category / kind:** product-ops / missing
- **Locations:** AGENTS.md:141-142, 159-165; kullanim-kilavuzu.md:75-82, 112-113, 163-164, 172-208, 212-219
- **Problem:** The manual tells installers to make sure the panel is connected, but the panel LED has no 'not connected' state and health data exists only on the in-box UART monitor (no cable or baud instructions). The module's 'normal' state does not mean the panel is reachable, because heartbeats are not ACKed. There is no registration list command, and the manual does not say how to power a module at the panel.
- **Scenario:** The panel is offline. All modules show 'normal' and the panel shows 'ready', so failures surface only when someone rings or presses door-open.
- **Recommendation:** Add a panel LED state driven by WIFI_EVENT_STA_DISCONNECTED/IP_EVENT_STA_GOT_IP. Add a module state 'AP OK but panel unreachable for N heartbeats', using the confirmable heartbeat from I-12. Add a read-only portal status page (state, version, apartment list with last-seen and RSSI/BSSID) and a manual section on serial monitoring.
- **Evidence:** kullanim-kilavuzu.md:113, 177, 205, 214; AGENTS.md:160; yapilandirma-ayarlari.md:215.
- **Source findings:** PRO-7, OPS-7, DOC-9

### I-42

**Orphaned modules (deleted, building reset, panel replaced) are never told and keep looking healthy**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:107-108, 121-129, 160; kullanim-kilavuzu.md:80, 152-153, 159-160, 192
- **Problem:** Deletion or a building reset removes keys only on the panel. The module stays 'dim solid' and keeps sending traffic that the panel rejects, which pollutes rejected_messages, and an unauthenticated NACK cannot be trusted.
- **Scenario:** After a building reset, missed modules show 'normal' while 98 of them flood the monitor with rejections. Residents find out only when door-open fails.
- **Recommendation:** With the confirmable heartbeat, have a module that gets no authenticated ACK for N intervals switch to a 'panel unreachable / not registered' state. On the panel, count unknown key-ids separately from authentication failures and rate-limit their log lines.
- **Evidence:** AGENTS.md:122-123, 129; kullanim-kilavuzu.md:160, 192.
- **Source findings:** PRO-10

### I-43

**No panel mode state machine: concurrency of pairing, portal, resets, code changes and lockout is unspecified**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:61-71, 82-108, 168-170; kullanim-kilavuzu.md:73-92, 98-102, 144-145, 155-160
- **Problem:** Pairing (open SoftAP) and the portal (WPA2 SoftAP) share the single SoftAP with conflicting configurations, and no rule says which commands are accepted in which mode. Undefined: a code change or reset button during the portal, a building reset or deletion during pairing completion, and whether module door-open is served during lockout, pairing or the portal.
- **Scenario:** An installer starts pairing while the portal is open. The phone is dropped and both timers fight over the SoftAP.
- **Recommendation:** Define one esp_event-driven mode machine (IDLE, PAIRING_DISCOVERY, PAIRING_COMPLETION, PORTAL, RESETTING) with an allowed-input table. Admin commands return an error in non-IDLE modes, ringing and module door-open are always served, a reset button press closes the portal first, and a new passphrase applies at the next portal open.
- **Evidence:** AGENTS.md:67-71, 86-87; kullanim-kilavuzu.md:91.
- **Source findings:** PRO-4

### I-44

**Keypad lockout is a building-wide DoS lever that also blocks the admin**

- **Category / kind:** security / weakness
- **Locations:** kullanim-kilavuzu.md:38, 88-92, 215; yapilandirma-ayarlari.md:114-124; AGENTS.md:168-169
- **Problem:** Every '*' entry shares one counter, so five junk entries every 15 min keep the keypad locked for residents and also block every admin command. Nothing is logged or alerted.
- **Scenario:** A vandal, or an attacker blocking the admin's response after I-5, re-triggers the lockout continuously, and the manual only says to wait.
- **Recommendation:** Document the trade-off and give admin functions a way out (a separate admin counter, or an override through the console or a tamper-protected button). Log lockouts, and count per apartment-code slot if I-6 is adopted.
- **Evidence:** kullanim-kilavuzu.md:88, 215; yapilandirma-ayarlari.md:121.
- **Source findings:** DEV-7

### I-45

**Unauthenticated MAC address triggers automatic deletion of another apartment's registration**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:105-106; kullanim-kilavuzu.md:152-153
- **Problem:** The 'same physical module' is identified by its MAC, which is visible over the air and settable in software (esp_wifi_set_mac). An attacker who wins a pairing session (I-1) while spoofing a victim's MAC deletes that apartment's registration.
- **Scenario:** While apt 30 is being paired, the attacker presents apt 12's MAC, and apt 12 loses rings and door-open.
- **Recommendation:** Bind module identity to a cryptographic credential (the label code in the PAKE, or a static module key), and remove the old record only after a confirmed PAIR_COMPLETE. Alternatively drop the auto-removal and require an explicit '*2*'.
- **Evidence:** AGENTS.md:105-106; esp_wifi.h esp_wifi_set_mac.
- **Source findings:** CRY-14

### I-46

**TWDT plan targets X25519 (a non-issue) and misses blocking subscribed tasks and unwatched timer contexts**

- **Category / kind:** platform / weakness
- **Locations:** AGENTS.md:172-174; yapilandirma-ayarlari.md:214; kullanim-kilavuzu.md:163-164
- **Problem:** X25519 runs far below 5 s. The real pitfalls are unspecified: a subscribed task blocked in recvfrom without a timeout (as in udp_server.c) reboots the device after 5 s of silence. Button work in esp_timer and LED work in the FreeRTOS timer task are unwatched, so a deadlock there never restarts the device.
- **Scenario:** The UDP RX task, copied from the example, reboots the panel whenever no packets arrive for 5 s. Or a deadlocked button callback kills the keypad with no restart.
- **Recommendation:** Require subscribed tasks to block with a timeout of ≤ TWDT/2 and reset the TWDT on each loop. Add esp_task_wdt_add_user() users fed from an esp_timer callback and a FreeRTOS timer callback, and drop the X25519 note.
- **Evidence:** esp_system/Kconfig:324-356; esp_task_wdt.h:90,105,131; udp_server.c:105-111; iot_button.c:13,89; led_indicator.c:481.
- **Source findings:** PLT-10

### I-47

**Setup-portal web server is reachable from the building LAN, bypassing the admin-code gate**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:9-10, 62-71; kullanim-kilavuzu.md:94-106
- **Problem:** httpd binds to INADDR_ANY unless if_name is set, and the STA may stay associated during the portal. A PSK holder on the LAN can therefore POST new SSID/PSK values without the admin code, or exhaust the 7 sockets (lru_purge off). It is unspecified whether the form echoes the current PSK.
- **Scenario:** A LAN attacker sees the Doorbell-Setup beacon and posts bogus credentials, and the panel leaves the building AP.
- **Recommendation:** Set httpd_config_t.if_name to the SoftAP netif (optionally also check that the local address is 192.168.4.1 per request). Never echo the stored PSK, and state whether the STA stays connected during the portal.
- **Evidence:** httpd_main.c:369-380, 393-397; esp_http_server.h:65, 69, 82.
- **Source findings:** GAP-1

### I-48

**Installation acceptance test does not verify the apartment-number mapping**

- **Category / kind:** product-ops / weakness
- **Locations:** kullanim-kilavuzu.md:110-137
- **Problem:** Modules are paired at the entrance and then carried to their flats, and the only post-install check is a door-open press. Nothing confirms the module is in the right flat, and there is no labelling step or handover record.
- **Scenario:** The modules for flats 12 and 21 are swapped, so flat 21 opens the door for flat 12's visitors.
- **Recommendation:** Require a label at pairing time and a ring test from the panel confirmed inside each flat. Add a commissioning sheet (apartment, MAC, date) together with the backup from I-15.
- **Evidence:** kullanim-kilavuzu.md:126, 131-132.
- **Source findings:** OPS-12

### I-49

**ESP32-C3/S3 portability claim conflicts with the ESP32-only default GPIO map**

- **Category / kind:** platform / inconsistency
- **Locations:** AGENTS.md:40; yapilandirma-ayarlari.md:76-88, 151-160
- **Problem:** C3 has only GPIO0-21, so pins 23/25-27/32/33 are invalid. S3 lacks GPIO22-25, and its GPIO26-32 belong to flash/PSRAM. The GPIO notes (6-11 flash, 34-39 input-only) apply to ESP32 only.
- **Scenario:** An S3 build with the defaults fails gpio_config on relay 23, or disturbs the flash bus through keypad pins 26/27/32.
- **Recommendation:** Use per-target Kconfig defaults ('default N if IDF_TARGET_…'), validate pins with GPIO_IS_VALID_OUTPUT_GPIO(), and make the notes target-specific. Otherwise narrow the claim.
- **Evidence:** esp32c3 soc_caps.h:168; esp32s3 soc_caps.h:193; esp32s3.inc:253.
- **Source findings:** DOC-6, PLT-15

### I-50

**NVS write atomicity, failure handling and tx_counter write-ahead are unspecified**

- **Category / kind:** platform / missing
- **Locations:** AGENTS.md:107-108, 127-129; yapilandirma-ayarlari.md:44, 58-59; kullanim-kilavuzu.md:151, 159-160
- **Problem:** NVS is power-safe only per key. Multi-field records (panel registration; module SSID/PSK/key/flag), multi-key erases and failed writes are undefined. The tx_counter block reservation is not stated normatively as committed before use, so a naive implementation reuses counters, and therefore CCM nonces, after quick reboots.
- **Scenario:** Power drops during a module reset after the SSID is erased but before the key, leaving an undefined state. Or the device boots, uses counters S+100.. without committing the reservation, and reuses them on the next boot.
- **Recommendation:** Store each record as one blob (nvs_set_blob + nvs_commit) with a version byte. Write the valid flag last and erase it first. Before using counter c where c mod 100 == 0, commit c+100 (and stored+100 at boot), and stop transmitting if that write fails (RFC 8613 App. B.1.1). Check every nvs_* return value, and a failed pairing commit sends no PAIR_ACK.
- **Evidence:** nvs_flash.rst:105; RFC 8613 App. B.1.1; RFC 3610 §2.
- **Source findings:** PRO-16, CRY-8

### I-51

**Default 24 KB NVS cannot hold the documented 254 apartments plus IDF's own NVS users**

- **Category / kind:** platform / risk
- **Locations:** yapilandirma-ayarlari.md:107; AGENTS.md:45-46, 127-128
- **Problem:** 0x6000 gives about 630 usable entries, and PHY calibration takes about 62 of them with the Wi-Fi driver adding more. At roughly 3-4 entries per record, 254 apartments do not fit. 99 probably fit, but the GC margin under counter churn is unmeasured, and no budget exists.
- **Scenario:** Pairing #200 fails with ESP_ERR_NVS_NOT_ENOUGH_SPACE, or a later counter reservation fails.
- **Recommendation:** Write an NVS budget measured with nvs_get_stats() at the maximum count. Cap the range, or use a larger or dedicated pairing NVS partition via an ADR-recorded custom table.
- **Evidence:** nvs_constants.h:18-21; nvs_pagemanager.cpp:244-246; nvs_flash.rst:379, 442; esp_phy_init.h:48-50; partitions_singleapp.csv.
- **Source findings:** PLT-3, OPS-11

### I-52

**Persistence of runtime state across reboot is unspecified (silence backoff, lockout, IP table)**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:143-145, 168-169; yapilandirma-ayarlari.md:114-124, 138-143; kullanim-kilavuzu.md:87-92, 162-168
- **Problem:** The silence-restart backoff level must survive the panel's own restart, but its storage is undefined, its reset rule appears only in the config doc, and the timer start point is unspecified. Lockout count and level are not in the 'kept on restart' list, so a power cycle clears an attacker's escalation. The volatile IP table has no rule for sending to an unknown peer.
- **Scenario:** The backoff kept in RAM restarts the panel every 15 min throughout an AP outage. Or an attacker power-cycles the panel to reset the 900 s lockout.
- **Recommendation:** Keep the backoff level in RTC_NOINIT_ATTR (with magic and CRC, honoured only on software/WDT resets), reset it on the first authenticated report, and start the timer at boot. Persist the lockout level and remaining time in NVS when they change. State that the IP table is volatile and unknown peers get broadcast first, and move the backoff rules into AGENTS.md §6.
- **Evidence:** esp_attr.h:101-104; yapilandirma-ayarlari.md:142-143; kullanim-kilavuzu.md:168.
- **Source findings:** PRO-9, DOC-13, DEV-8

### I-53

**No cleartext destination/key-id: broadcast retries pollute rejected_messages on every module and force trial decryption on the panel**

- **Category / kind:** protocol / missing
- **Locations:** AGENTS.md:116-126, 137-139; yapilandirma-ayarlari.md:32-34, 61-66
- **Problem:** Every broadcast retry fails authentication on the N-1 non-target modules, and each failure increments rejected_messages. The attack indicator becomes noise proportional to building size. The panel has no key selector and may need up to 99 trial decryptions per frame under a flood (TWDT effect unmeasured).
- **Scenario:** One offline module turns every ring into +2 rejections on 98 modules, and every heartbeat line shows a warning that hides a real forgery.
- **Recommendation:** Add src/dst apartment (key-id) to the AAD-authenticated header (I-8). Drop frames not addressed to the receiver before any crypto and without counting them, select the single key by src, and count only addressed frames that fail authentication or replay checks.
- **Evidence:** AGENTS.md:119, 122-126; esp_system/Kconfig:342-345.
- **Source findings:** CRY-10, NET-13, PRO-12

### I-54

**No persistent security event log, time source or registration audit**

- **Category / kind:** product-ops / missing
- **Locations:** kullanim-kilavuzu.md:163-164, 205-208; AGENTS.md:141-145; yapilandirma-ayarlari.md:64-66
- **Problem:** Code resets, code changes, lockouts, portal sessions, pairings, deletions, building resets and door openings are recorded nowhere, and there is no time base. Rogue registrations show up only as heartbeat lines on a live monitor, and adding a log partition later requires a reflash.
- **Scenario:** After a burglary nobody can say who released the door or when a module was paired.
- **Recommendation:** Add a ring-buffer event log in NVS or a dedicated partition, readable via the authenticated console or status page. Use esp_netif_sntp if available, otherwise set time from the portal or use boot-relative sequence numbers. Add a registration list with MACs and define KVKK retention.
- **Evidence:** No log requirement in any doc; CRA Annex I(2)(l) (secondary).
- **Source findings:** DEV-15, OPS-9

### I-55

**No ESP-IDF or component versions pinned; crypto is named in legacy Mbed TLS terms although IDF 6.1 exposes it only via PSA**

- **Category / kind:** platform / missing
- **Locations:** AGENTS.md:17-23, 92, 125-126, 153-156; yapilandirma-ayarlari.md:3-5, 204-205
- **Problem:** The design depends on IDF defaults that changed in 6.x, yet no IDF, button or led_indicator version is pinned. In IDF 6.1 (Mbed TLS 4.1 / TF-PSA-Crypto) X25519 and CCM are available only through PSA: there is no ecdh.h, and ccm.h is private. Per-site rebuilds are not reproducible.
- **Scenario:** A developer writes mbedtls_ecdh_*/mbedtls_ccm_* and the build fails. Or two installers build with different IDF and component versions and get different defaults.
- **Recommendation:** Pin ESP-IDF v6.1 and exact component versions, and commit dependencies.lock and sdkconfig.defaults. Name the PSA calls: psa_generate_key (Montgomery 255), psa_raw_key_agreement(PSA_ALG_ECDH), PSA_ALG_HKDF, psa_aead_* with PSA_ALG_CCM. Forbid mbedtls/private includes, cite security1.c as reference, and add a short ADR on not using protocomm/network_provisioning.
- **Evidence:** migration-guides/release-6.x/6.0/security.rst:25-35; tf-psa-crypto private/ccm.h; esp_idf_version.h (6.1); button 4.2.3, led_indicator 2.1.2; security1.c:258-290.
- **Source findings:** PLT-6, DOC-12, CRY-11, PLT-5

### I-56

**Traceability gaps: missing ADR 0013/0014, SPEC 9 and flashing GUI; no VCS, test strategy or bench plan**

- **Category / kind:** documentation / missing
- **Locations:** AGENTS.md:1, 45, 67, 118, 172-174; yapilandirma-ayarlari.md:3, 8, 42, 112, 191, 197, 214, 216
- **Problem:** The docs cite ADR 0013, ADR 0014, SPEC/SPEC 9 and a flashing GUI, none of which exist, so the decisions and timing provenance cannot be audited. The folder is not under git and has no version headers. The 'to be measured' values have no test plan or pass criteria, and there is no unit, host or QEMU test strategy.
- **Scenario:** When OTA is reconsidered, nobody knows why it was rejected. The 400 ms timeout ships without ever being measured through a repeater.
- **Recommendation:** Add docs/adr/NNNN-*.md (Nygard/MADR), and add the SPEC document or remove its references. Put the folder under git with version headers. Add a bench test plan per 'ölçülecek' value, plus Unity, linux-target and fuzz tests for the parsers and QEMU/pytest-embedded runs in CI.
- **Evidence:** ls of project (3 files); environment 'Is a git repository: false'; docs/conf_common.py:520 QEMU_TARGETS.
- **Source findings:** DOC-5, OPS-17, DOC-14

### I-57

**LED patterns lack priority/preemption rules and are inconsistent or defined in only one document**

- **Category / kind:** documentation / inconsistency
- **Locations:** kullanim-kilavuzu.md:126, 144-149, 172-199; AGENTS.md:151-152, 157-166; yapilandirma-ayarlari.md:101, 167, 195-198
- **Problem:** About 10 overlapping patterns per LED have no priority or return rule, and the visual-bell duration is unspecified. The door-open LED is fixed at 0.5 s although the relay pulse is configurable, and the reset-armed and dim timings are missing. The panel code-reset button gives no feedback, the 12-15 s window is undefined, and the config doc calls timings 'suggestions' that AGENTS.md makes binding.
- **Scenario:** A ring during a door-open failure blink is suppressed, so a hearing-impaired resident misses it. A technician holding the reset button cannot tell when 10 s has passed.
- **Recommendation:** Write one English LED table per device (state, priority, pattern, timing, exit) and generate the Turkish tables from it. Tie the door-open LED to the relay pulse, add reset-button feedback, and implement priorities with led_indicator blink lists.
- **Evidence:** AGENTS.md:152, 163-165; yapilandirma-ayarlari.md:101, 167, 195-197; kullanim-kilavuzu.md:144-145, 193.
- **Source findings:** PRO-15, DOC-10

### I-58

**Heartbeat and pairing carry no firmware/protocol version or BSSID/channel data**

- **Category / kind:** product-ops / missing
- **Locations:** AGENTS.md:137-139; yapilandirma-ayarlari.md:61-66
- **Problem:** Without OTA, mixed-firmware fleets are certain, yet telemetry has no version, build hash or config fingerprint. BSSID, channel and disconnect count, which locate repeater problems, are also missing.
- **Scenario:** A replacement module with a newer format fails intermittently. Nobody can see its build, or that every failing module sits behind repeater #3.
- **Recommendation:** Add esp_app_desc_t.version, a short app_elf_sha256 and the protocol version to pairing and heartbeats, plus BSSID, channel, disconnect count and IP. Define a compatibility policy.
- **Evidence:** esp_app_desc.h:27-35, 55; esp_wifi_types_generic.h:323 (wifi_ap_record_t.bssid).
- **Source findings:** OPS-8

### I-59

**Visitor-side UX and accessibility: no audible cue, one LED for 9+ states, no ring rate limit or do-not-disturb**

- **Category / kind:** product-ops / weakness
- **Locations:** AGENTS.md:151; kullanim-kilavuzu.md:31-39, 91, 174-199; yapilandirma-ayarlari.md:166-167
- **Problem:** Visitors get feedback only from one LED with 9 similar patterns and no sound, so blind or low-vision visitors get nothing. Ringing has no rate limit or do-not-disturb option and works during lockout.
- **Scenario:** A visually impaired visitor cannot tell whether the bell rang or the door opened. A prankster rings flats 1-99 at 3 a.m.
- **Recommendation:** Revisit the no-buzzer decision (a piezo for key and release tones is standard on intercom keypads). Add a ring rate limit and an optional do-not-disturb window on modules.
- **Evidence:** AGENTS.md:151; kullanim-kilavuzu.md:91, 174-185.
- **Source findings:** OPS-14

### I-60

**Regulatory applicability not decided (CRA, RED DA/EN 18031, EN 303 645, radio/EMC/safety)**

- **Category / kind:** product-ops / risk
- **Locations:** absent from all docs
- **Problem:** If sold in the EU, the CRA (reporting from 11 Sep 2026, main obligations from 11 Dec 2027) requires update handling, an SBOM, logging and secure removal, all of which conflict with the no-OTA, no-log and no-wipe decisions. Applicability of RED DA and EN 18031 and the radio, EMC, safety and IP/IK requirements are not assessed.
- **Scenario:** After the hardware freeze at 2 MB with no OTA, the product cannot meet CRA update duties and needs a redesign.
- **Recommendation:** Make a market and compliance decision before the hardware freeze. If selling, plan I-17, I-54, I-23, I-5 and idf.py sbom-create in CI, and use a pre-certified module.
- **Evidence:** Regulation (EU) 2024/2847 Annex I Part I(2)(l),(m); idf-sbom.rst:31,55,75.
- **Source findings:** OPS-16


## Low

### I-61

**Default building PSK is a Kconfig string, so it lands in plaintext in sdkconfig and the app image**

- **Category / kind:** security / weakness
- **Locations:** AGENTS.md:56; yapilandirma-ayarlari.md:3-5, 22-23
- **Problem:** The PSK entered in the GUI is compiled in, so it ends up in sdkconfig, the build outputs and the factory image. It remains there after the portal changes the NVS copy.
- **Scenario:** An installer's archive of sdkconfig files exposes many buildings' AP passphrases.
- **Recommendation:** Leave the Kconfig default empty. Provision the PSK via the portal, the console or a per-device factory NVS image, and keep the GUI from saving it to project files.
- **Evidence:** yapilandirma-ayarlari.md:3-4, 22-23; station example Kconfig.projbuild:9.
- **Source findings:** DEV-13

### I-62

**Authenticated IP-table update can be redirected by relaying a not-yet-received frame from another IP**

- **Category / kind:** security / risk
- **Locations:** AGENTS.md:117-123; yapilandirma-ayarlari.md:32-34
- **Problem:** A LAN attacker can re-send a genuine but not-yet-accepted broadcast retry from its own IP, which maps the peer to the attacker. The impact is one extra 400 ms timeout before the fallback, or longer when combined with I-7.
- **Scenario:** The attacker relays apt 12's heartbeat retry, and the panel's next RING to apt 12 is delayed by 400 ms.
- **Recommendation:** Optionally include the sender's IPv4 in the authenticated payload and update the table only when it matches the source address. Otherwise document the accepted risk.
- **Evidence:** AGENTS.md:121-123.
- **Source findings:** CRY-16

### I-63

**Module button edge cases unspecified (pairing when already paired, both buttons, door button when unpaired or pending)**

- **Category / kind:** protocol / missing
- **Locations:** kullanim-kilavuzu.md:117-121, 136, 148-153, 190-199, 214; AGENTS.md:158, 166
- **Problem:** There is no defined feedback for releasing the pairing button on a paired module, pressing both hidden buttons, or pressing the door button when unpaired or while a request is outstanding.
- **Scenario:** An already-paired module shows 'release now' and then does nothing, and the panel reports 'not found'.
- **Recommendation:** Define per-state handling: error pattern when already paired, cancel when both buttons are pressed, error when unpaired, and coalesce door presses while a request is outstanding.
- **Evidence:** kullanim-kilavuzu.md:136, 196, 214.
- **Source findings:** PRO-18

### I-64

**Synchronized boot after a building power restore causes an association/DHCP storm and a heartbeat burst**

- **Category / kind:** network / missing
- **Locations:** AGENTS.md:135-136; yapilandirma-ayarlari.md:46, 67-68
- **Problem:** Jitter applies only to the interval, and first-heartbeat timing is undefined. Synchronized bursts can overflow the default UDP mailbox of 6, and with ACK+retry the retries re-collide.
- **Scenario:** About 99 heartbeats arrive within a few hundred ms, the mailbox drops some, and spurious 'no report' warnings follow.
- **Recommendation:** Send the first heartbeat after a uniform random delay in [0, interval] and jitter the retries. Raise CONFIG_LWIP_UDP_RECVMBOX_SIZE only if a full-size bench test shows drops.
- **Evidence:** lwip/Kconfig:860-864.
- **Source findings:** NET-14

### I-65

**Time base for long timers unspecified; 32-bit ticks at 100 Hz wrap after ~497 days**

- **Category / kind:** platform / risk
- **Locations:** AGENTS.md:139-148, 156, 168; yapilandirma-ayarlari.md:119-121, 140-143
- **Problem:** The lockout, no-report and 48 h backoff timers have no defined time source. Comparisons on absolute ticks that are not wrap-safe break after about 497 days of uptime.
- **Scenario:** A lockout started just before the wrap never expires.
- **Recommendation:** Specify esp_timer_get_time() (int64 µs) or wrap-safe xTaskCheckForTimeOut, and add a near-wrap test.
- **Evidence:** freertos/Kconfig:33-37; xtensa portmacro.h:85-91.
- **Source findings:** GAP-5

### I-66

**'Impossible settings fail the build' has no mechanism; Kconfig falls back to the default and sub-tick LED steps assert at runtime**

- **Category / kind:** platform / missing
- **Locations:** yapilandirma-ayarlari.md:11-12, 108, 177, 198
- **Problem:** esp-idf-kconfig 3.14 notes an out-of-range value and uses the default rather than failing, and it cannot express cross-symbol rules. A sub-tick led_indicator period hits configASSERT at runtime.
- **Scenario:** A 5 ms flash step builds cleanly, then panics in the timer task on boot.
- **Recommendation:** Add _Static_assert checks in a shared header and have the GUI validate before writing sdkconfig.
- **Evidence:** esp_kconfiglib core.py:5050-5062; led_indicator.c:445; timers.c:951.
- **Source findings:** PLT-8

### I-67

**'Dim 10%' is ambiguous under led_indicator's default gamma correction (~0.4% duty)**

- **Category / kind:** documentation / risk
- **Locations:** AGENTS.md:160; yapilandirma-ayarlari.md:192-193
- **Problem:** With USE_GAMMA_CORRECTION=y (gamma 2.3), an input of 25/255 maps to 1/255. The docs do not say whether 10% means perceived brightness or PWM duty.
- **Scenario:** One firmware revision's 'in coverage' LED is invisible in daylight.
- **Recommendation:** Define dim as an led_indicator brightness value (0-255, gamma-corrected) and validate visibility on the bench.
- **Evidence:** led_indicator Kconfig:8-10; led_gamma.c:16-19.
- **Source findings:** PLT-16

### I-68

**On ESP32 an EN-button restart is reported as power-on, and the 300 ms interrupt-WDT figure applies only without PSRAM**

- **Category / kind:** documentation / incorrect-claim
- **Locations:** kullanim-kilavuzu.md:163-168; yapilandirma-ayarlari.md:61-62, 213
- **Problem:** ESP_RST_EXT is not applicable on ESP32, so EN presses and power cuts both log as POWERON. ESP_INT_WDT_TIMEOUT_MS is 800 ms when SPIRAM is enabled.
- **Scenario:** A technician misdiagnoses tenant EN presses as mains cuts.
- **Recommendation:** Document that POWERON covers EN on ESP32, and qualify 300 ms as the default for non-PSRAM modules.
- **Evidence:** esp_system.h:24-28; esp32 reset_reason.c:19-20; esp_system/Kconfig:281-286.
- **Source findings:** PLT-17

### I-69

**255.255.255.255 is mislabelled as the 'subnet broadcast address'**

- **Category / kind:** documentation / incorrect-claim
- **Locations:** AGENTS.md:38, 119; yapilandirma-ayarlari.md:33
- **Problem:** It is the limited broadcast address (RFC 1122 §3.2.1.3); a subnet-directed broadcast is prefix plus all-ones. In APSTA mode it leaves via the default netif (STA).
- **Scenario:** An implementer computes ip|~mask instead, and behaviour diverges from the spec.
- **Recommendation:** Write 'limited broadcast address (255.255.255.255, RFC 1122 §3.2.1.3)' and note that it egresses via the STA netif.
- **Evidence:** RFC 1122 §3.2.1.3; lwip ip4.c:217-228; esp_netif_defaults.h:54,69.
- **Source findings:** NET-16, DOC-16

### I-70

**The 'Only These Apply' list of IDF deviations is incomplete and includes a non-deviation**

- **Category / kind:** documentation / inconsistency
- **Locations:** AGENTS.md:63-65, 86-88, 171-177
- **Problem:** The country code is off by default, so it is not a deviation, while explicit max_connection, APSTA and portal authmode settings are not listed.
- **Scenario:** A reviewer 'corrects' explicit Wi-Fi settings back to their defaults.
- **Recommendation:** Split the list into sdkconfig deviations (mirrored in sdkconfig.defaults) and explicit Wi-Fi API settings, and move the country code to 'optional'.
- **Evidence:** esp_system/Kconfig:324-337; wifi-performance-and-power-save.rst:1561.
- **Source findings:** DOC-15

### I-71

**Config reference conventions are inconsistent (Wi-Fi defaults in a 'panel ve modül' menu, missing defaults, no symbol table)**

- **Category / kind:** documentation / inconsistency
- **Locations:** yapilandirma-ayarlari.md:3-7, 16-23, 202-217
- **Problem:** Panel-only Wi-Fi defaults sit under a shared menu heading, the §5 rows lack the Varsayılan column, and settings are named only by prompt text, with no CONFIG_* catalogue.
- **Scenario:** A technician edits the Kconfig PSK default expecting it to change an installed panel.
- **Recommendation:** Move the Wi-Fi defaults to a panel-only menu, add defaults to §5, and keep an English CONFIG_* table in an engineering doc.
- **Evidence:** yapilandirma-ayarlari.md:7, 16, 22, 207-217.
- **Source findings:** DOC-17

### I-72

**Language rule leaves no room for Turkish installer-facing material, and an engineering requirement is traced to the manual**

- **Category / kind:** documentation / inconsistency
- **Locations:** AGENTS.md:24-30, 172
- **Problem:** Only two files may be Turkish, so portal UI strings, labels and an installation guide would have to be English. AGENTS.md:172 also justifies a binding deviation by 'Manual 6.4'.
- **Scenario:** The Turkish installer gets an English portal page, or developers break the rule inconsistently.
- **Recommendation:** Define the class instead ('end-user-facing text is Turkish; engineering artefacts are English'), and move the restart-on-hang requirement into an ADR.
- **Evidence:** AGENTS.md:25-30, 172.
- **Source findings:** DOC-18

