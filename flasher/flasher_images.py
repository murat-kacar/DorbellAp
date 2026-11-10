"""Firmware image, sdkconfig overlay, partition table and NVS helpers used by Flash and Burn."""

from __future__ import annotations

import csv
import json
import re
import secrets
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

from flasher_config import PROJECT_ROOT

# panel_cfg NVS record packer, shared with tools/verify_protocol_vectors.py
sys.path.insert(0, str(PROJECT_ROOT / "tools"))
from panel_cfg_blob import pack_panel_cfg_blob  # noqa: E402


def build_info_path(bin_path: Path) -> Path:
    return bin_path.with_suffix(".json")


def read_build_info(bin_path: Path) -> dict[str, Any] | None:
    """Read the sidecar written next to a flasher build (role and code lengths baked into that firmware)."""
    try:
        return json.loads(build_info_path(bin_path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def generate_random_digits(length: int) -> str:
    rng = secrets.SystemRandom()
    return "".join(str(rng.randrange(0, 10)) for _ in range(length))


def inspect_firmware_image(bin_path: Path) -> str | None:
    """Role ("panel", "apartment" or "unknown") of a complete flasher image (bootloader @0x1000, partition table
    @0x8000, app @0x10000), or None if the file is not such an image. The role comes from esp_app_desc_t."""
    try:
        data = bin_path.read_bytes()
    except OSError:
        return None
    if not (
        len(data) > 0x10100 and data[0x1000] == 0xE9 and data[0x8000:0x8002] == b"\xaa\x50" and data[0x10000] == 0xE9
    ):
        return None
    desc_off = 0x10000 + 0x20  # esp_app_desc_t; magic 0xABCD5432 (little-endian)
    if len(data) >= desc_off + 0x50 and struct.unpack_from("<I", data, desc_off)[0] == 0xABCD5432:
        project = data[desc_off + 0x30 : desc_off + 0x50].split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
        if "panel" in project:
            return "panel"
        if "unit" in project:
            return "apartment"
    return "unknown"


# ---- Burn profile (ADR 0015, Option B): Secure Boot v2 + release flash encryption + NVS encryption ----

BURN_OVERLAY_PATH = PROJECT_ROOT / "firmware" / "sdkconfig.burn"
BURN_CONFIRM_WORD = "BURN"
_SDKCONFIG_LINE_RE = re.compile(r"^(?:# )?(CONFIG_[A-Z0-9_]+)(?:=.*| is not set)$")

# Partition types / subtypes (esp_partition.h)
_PT_MAGIC = b"\xaa\x50"
_PT_MD5_MAGIC = b"\xeb\xeb"
_PT_TYPE_APP, _PT_TYPE_DATA = 0x00, 0x01
_PT_SUBTYPE_FACTORY, _PT_SUBTYPE_NVS, _PT_SUBTYPE_NVS_KEYS = 0x00, 0x02, 0x04
_PT_FLAG_ENCRYPTED = 0x1


def read_sdkconfig_overlay(path: Path) -> dict[str, str]:
    """CONFIG name -> authoritative line ("CONFIG_X=..." or "# CONFIG_X is not set")."""
    overlay: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _SDKCONFIG_LINE_RE.match(line.strip())
        if m:
            overlay[m.group(1)] = line.strip()
    return overlay


def apply_sdkconfig_overlay(sdkconfig_path: Path, overlay: dict[str, str]) -> None:
    out: list[str] = []
    seen = set()
    for line in sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = _SDKCONFIG_LINE_RE.match(line)
        if m and m.group(1) in overlay:
            line = overlay[m.group(1)]
            seen.add(m.group(1))
        out.append(line)
    out.extend(v for k, v in overlay.items() if k not in seen)
    sdkconfig_path.write_text("\n".join(out) + "\n", encoding="utf-8")


def check_sdkconfig_overlay(sdkconfig_path: Path, overlay: dict[str, str]) -> list[str]:
    """Overlay lines that kconfgen did not keep (a dependency silently dropped them)."""
    lines = set(sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines())
    problems = []
    for name, want in overlay.items():
        if want.endswith(" is not set"):
            if any(line.startswith(f"{name}=") for line in lines):
                problems.append(f"{name} should be unset")
        elif want not in lines:
            problems.append(f"expected '{want}'")
    return problems


def parse_partition_table(blob: bytes) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for off in range(0, len(blob) - 31, 32):
        raw = blob[off : off + 32]
        if raw[:2] == _PT_MD5_MAGIC:
            continue
        if raw[:2] != _PT_MAGIC:
            break
        _magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", raw, 0)
        (flags,) = struct.unpack_from("<I", raw, 28)
        label = raw[12:28].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        entries.append(
            {
                "label": label,
                "type": ptype,
                "subtype": subtype,
                "offset": offset,
                "size": size,
                "encrypted": bool(flags & _PT_FLAG_ENCRYPTED),
            }
        )
    return entries


def burn_layout_from_partition_table(entries: list[dict[str, Any]]) -> dict[str, int]:
    """Offsets of the Burn layout; raises ValueError if the table is not the expected nvs / nvs_keys / factory set."""

    def one(ptype: int, subtype: int, what: str) -> dict[str, Any]:
        found = [e for e in entries if e["type"] == ptype and e["subtype"] == subtype]
        if len(found) != 1:
            raise ValueError(f"partition table must contain exactly one {what} partition (found {len(found)})")
        return found[0]

    nvs = one(_PT_TYPE_DATA, _PT_SUBTYPE_NVS, "nvs")
    keys = one(_PT_TYPE_DATA, _PT_SUBTYPE_NVS_KEYS, "nvs_keys")
    app = one(_PT_TYPE_APP, _PT_SUBTYPE_FACTORY, "factory app")
    if nvs["encrypted"]:
        raise ValueError("the nvs partition must not carry the 'encrypted' flag (NVS encrypts its own entries)")
    if not keys["encrypted"]:
        raise ValueError("the nvs_keys partition must carry the 'encrypted' flag")
    if keys["size"] < 0x1000:
        raise ValueError("the nvs_keys partition must be at least 4 KB")
    return {
        "nvs_offset": nvs["offset"],
        "nvs_size": nvs["size"],
        "nvs_keys_offset": keys["offset"],
        "nvs_keys_size": keys["size"],
        "app_offset": app["offset"],
        "app_size": app["size"],
    }


PANEL_NVS_OFFSET = 0x9000  # Production partition table (ESP-IDF single factory app)
PANEL_NVS_SIZE = 0x6000


def write_panel_nvs_csv(csv_path: Path, fields: tuple[str, str, str, str]) -> None:
    """nvs_partition_gen input holding the panel_cfg blob (admin, user, ssid, password). Never logged (I-25)."""
    admin_code, user_code, ssid, pw = fields
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow(["doorbell", "namespace", "", ""])
        writer.writerow(["panel_cfg", "data", "hex2bin", pack_panel_cfg_blob(ssid, pw, admin_code, user_code).hex()])


def make_nvs_keys_partition() -> bytes:
    """nvs_keys partition image: XTS eky (32 B) + tky (32 B) + CRC32, rest 0xFF (nvs_flash_read_security_cfg).

    Generated with `secrets` rather than nvs_partition_gen --keygen, which draws the key from `random`.
    """
    buf = bytearray(b"\xff") * 0x1000
    buf[0:64] = secrets.token_bytes(64)
    struct.pack_into("<I", buf, 64, zlib.crc32(bytes(buf[0:64]), 0xFFFFFFFF) & 0xFFFFFFFF)
    return bytes(buf)


def burn_efuse_problems(summary: dict[str, Any]) -> list[str]:
    """Reasons an ESP32 must not be burned, from `espefuse summary --format json`. Empty list = fresh, eligible chip."""
    problems: list[str] = []

    def field(name: str) -> dict[str, Any]:
        entry = summary.get(name)
        if not isinstance(entry, dict) or "value" not in entry:
            raise KeyError(name)
        return entry

    try:
        if field("WAFER_VERSION_MAJOR")["value"] < 3:
            problems.append(
                f"chip revision v{field('WAFER_VERSION_MAJOR')['value']}.x: Secure Boot v2 needs v3.0 or newer"
            )
        if not str(field("CODING_SCHEME")["value"]).upper().startswith("NONE"):
            problems.append(f"eFuse coding scheme is '{field('CODING_SCHEME')['value']}': Secure Boot v2 needs 'NONE'")
        if field("FLASH_CRYPT_CNT")["value"] != 0:
            problems.append("flash encryption has already been enabled on this chip (FLASH_CRYPT_CNT != 0)")
        for name in ("ABS_DONE_0", "ABS_DONE_1"):
            if field(name)["value"]:
                problems.append(f"Secure Boot is already enabled ({name})")
        for name in ("BLOCK1", "BLOCK2"):
            entry = field(name)
            if not (entry.get("readable") and entry.get("writeable")):
                problems.append(f"{name} (key block) is read- or write-protected")
            elif any(b != "00" for b in str(entry["value"]).split()):
                problems.append(f"{name} (key block) already holds a key")
        for name in ("FLASH_CRYPT_CNT", "ABS_DONE_1", "UART_DOWNLOAD_DIS"):
            if not field(name).get("writeable"):
                problems.append(f"{name} is write-protected")
        if field("UART_DOWNLOAD_DIS")["value"]:
            problems.append("UART download mode is already disabled")
    except (KeyError, TypeError) as exc:
        problems.append(f"espefuse summary lacks or has an unexpected field {exc}")
    return problems
