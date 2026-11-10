# DorbellAp

Offline apartment doorbell and door-open system on ESP32: one entrance **panel** (keypad, door relay, status LED) and
one **apartment module** per flat (bell relay, door-open button, status LED). The devices talk over the building's
Wi-Fi with authenticated, encrypted UDP (AES-CCM, X25519 + HKDF pairing); there is no internet, cloud or OTA
([ADR 0013](docs/adr/0013-offline-operation-no-ota.md)).

## Repository

| Path | Content |
| :--- | :--- |
| `firmware/panel`, `firmware/unit` | ESP-IDF projects of the panel and the apartment module |
| `components/` | Shared components: protocol & crypto, NVS storage, Wi-Fi/UDP, LED, relay / watchdog helpers, Kconfig |
| `firmware/sdkconfig.burn`, `firmware/partitions_burn.csv` | Burn profile overlay and partition table ([ADR 0015](docs/adr/0015-production-security-hardening.md)) |
| `flasher/` | Tk GUI: configure, build, flash / burn, serial monitor (`run_flasher.bat` on Windows) |
| `tools/` | Host protocol test vectors, NVS record packer, building-AP broadcast check |
| `docs/` | Protocol specification, ADRs, production checklist, design review |

## Build and flash

Requirements: ESP-IDF **v6.1** (EIM install) and an ESP32-D0WD-V3 (rev v3.x) board with 2 MB flash or more.

1. Start the flasher: `flasher\run_flasher.bat` (or `python flasher/flasher_gui.py` inside the ESP-IDF Python
   environment).
2. Pick the profile — **Production** (release build, re-flashable) or **Burn** (Secure Boot v2 + flash/NVS
   encryption, irreversible per chip) — and the role, adjust the settings, click **Build**.
3. On the device card select the image and click **Flash** (or **Burn**). Flashing erases the chip and writes the
   firmware together with the panel's admin / user codes and optional building Wi-Fi from the card.

Plain ESP-IDF works too: `idf.py -C firmware/panel build` (the flasher additionally writes the codes and Wi-Fi).

## Documentation

- [AGENTS.md](AGENTS.md) — engineering rules and invariants (authoritative for contributors and coding assistants)
- [docs/protocol.md](docs/protocol.md) — wire protocol, state machines, LED patterns
- [docs/production-checklist.md](docs/production-checklist.md) — manufacturing, burn and acceptance steps
- [docs/adr/](docs/adr/) — architecture decisions
- [kullanim-kilavuzu.md](kullanim-kilavuzu.md) — user guide (Turkish)
- [yapilandirma-ayarlari.md](yapilandirma-ayarlari.md) — configuration reference (Turkish)

## Checks

- `python tools/verify_protocol_vectors.py` — host test vectors for the NVS records, AEAD framing, pairing KDF,
  keypad grammar and challenge flow (needs `cryptography`).
- `test/host_unit` — Unity tests of the protocol and captive-DNS code on the ESP-IDF `linux` target:
  `idf.py --preview -C test/host_unit set-target linux build`, then run `test/host_unit/build/doorbell_host_unit.elf`.
- CI (`.github/workflows/ci.yml`) builds both firmwares, runs the host tests and the Python checks on every push to
  `master` and on pull requests.
- `pre-commit run --all-files` — formatting and lint (astyle with the ESP-IDF style, ruff); see `.pre-commit-config.yaml`.
