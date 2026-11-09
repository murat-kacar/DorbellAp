# Production Checklist (Release Software Level)

Scope: manufacturing and installing DorbellAp panels and apartment modules with the **Production** flasher profile.
This checklist does **not** burn eFuses. Secure Boot / flash encryption are tracked separately in
[ADR 0015](adr/0015-production-security-hardening.md) (Accepted, Option B; eFuse steps are added to this checklist after the bench prototype).

Normative references: [AGENTS.md](../AGENTS.md), [docs/protocol.md](protocol.md),
[kullanim-kilavuzu.md](../kullanim-kilavuzu.md), [yapilandirma-ayarlari.md](../yapilandirma-ayarlari.md).

## 1. Release build (once per release)

| # | Step | Pass criterion |
| :--- | :--- | :--- |
| 1.1 | Work on `master` only, clean tree (`git status` empty), tag pending | `git status` prints nothing |
| 1.2 | Flasher → **1. Profile = Production (release build: -Os)** | Build log shows `Production (-Os)` |
| 1.3 | Set the same interop values in both role tabs: UDP port, ACK timeout, pairing completion timeout, heartbeat on/off | Flasher does not show "Panel / Apartment Mismatch" |
| 1.4 | Build **panel** and **apartment** | `[Build] SUCCESS -> flasher/builds/<role>_<ts>_prod.bin`; app partition free space reported ≥ 10 % |
| 1.5 | Confirm firmware version in `doorbell_config_checks.h` matches the release tag (e.g. `1.1` ↔ `v1.1`) | Heartbeat log prints `fw=1.1` |
| 1.6 | Bench regression on one panel + one module (section 4) before field use | All rows pass |
| 1.7 | Tag the release commit (`git tag -a vX.Y`) and push the tag | Tag visible on GitHub |

## 2. Per panel

| # | Step | Pass criterion |
| :--- | :--- | :--- |
| 2.1 | Verify chip: Flasher card shows `ESP32-D0WD-V3 (revision v3.x)`; flash ID must be on ESP-IDF's XMC allow-list or a non-XMC part | Boot log has no `XMC flash startup fail` |
| 2.2 | **Flash** the `panel_*_prod.bin` image | Status `Flash OK` |
| 2.3 | Click **Random** for admin and user codes (unique per device, I-5; lengths follow the selected firmware) | Codes differ from each other and from any other panel |
| 2.4 | Optional: enter building SSID / passphrase if known at the factory | — |
| 2.5 | **Flash NVS** (writes codes + optional Wi-Fi; wipes registrations) | Status `Flash NVS OK`; log says `codes redacted` |
| 2.6 | Print the label with admin and user codes and attach it inside the enclosure | Label matches the card fields |
| 2.7 | **Monitor** → **Status**: boot without `Blank NVS` / `panel_cfg unreadable` warnings | `doorbell_status` prints, no repeating error LED |
| 2.8 | Clear the card code fields / close the Flasher session (codes are not stored anywhere else, I-15) | — |

Never ship a panel that booted with `Blank NVS: initializing panel_cfg with built-in default codes`: it runs on the
shared bench codes. Re-run step 2.5.

## 3. Per apartment module

| # | Step | Pass criterion |
| :--- | :--- | :--- |
| 3.1 | Verify chip and flash as in 2.1 | No XMC boot loop |
| 3.2 | **Flash** the `apartment_*_prod.bin` image | Status `Flash OK` |
| 3.3 | **Flash NVS** on an apartment card (= clear NVS, unpaired state) | LED: repeating double flash (unpaired) |
| 3.4 | Pair at the panel (`* <admin> * 1 * <apt> #`, hold pairing button 7–12 s) | Panel 2 s solid, module 2 s full + 1 bell ring |
| 3.5 | Label the module with its apartment number | — |

## 4. Bench / installation acceptance

| # | Check | Pass criterion |
| :--- | :--- | :--- |
| 4.1 | Building AP meets the AGENTS.md I-9 checklist, including **802.11b rates enabled** | Modules associate; boot log `Station restricted to 802.11b` |
| 4.2 | Ring `<apt> #` | Correct module rings |
| 4.3 | Door open from module button and with `* <user> #` | Relay pulses 0.5 s; module LED 2 s full |
| 4.4 | Panel reboot, then door open from module | Opens (STATUS_CHALLENGE path) |
| 4.5 | Wrong code ×5 → lockout; hidden reset button 10–60 s | Lockout clears, label codes work |
| 4.6 | Setup portal `* <admin> * 5 #` from a phone | Page loads on `Doorbell-Setup`; unreachable from building LAN |
| 4.7 | Error indication (invalid command) | 1 s fast blink |
| 4.8 | Heartbeat log on the panel monitor | `rssi` ≥ −75 dBm at the installed position, `disconn` stable |
