"""Firmware image, sdkconfig overlay, partition table and NVS helpers used by Flash and Burn."""

from __future__ import annotations

import csv
import hashlib
import json
import re
import secrets
import struct
import sys
import zlib
from collections.abc import Callable
from pathlib import Path
from typing import Any

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils

# flasher_env (stdlib only), not flasher_config: importing this module must not need esp_kconfiglib (CI Python checks)
from flasher_env import PROJECT_ROOT

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
    """Role ("panel", "apartment" or "unknown") of a complete flasher image (bootloader @0x1000, the partition table
    at the build's offset, the factory app where that table puts it), or None if the file is not such an image. The
    role comes from esp_app_desc_t."""
    try:
        data = bin_path.read_bytes()
    except OSError:
        return None
    info = read_build_info(bin_path) or {}
    if "partition_table_offset" not in info:
        return None  # not a build of this flasher (no sidecar with the layout)
    pt_off = int(info["partition_table_offset"])
    if len(data) <= pt_off + 0xC00 or data[0x1000] != 0xE9 or data[pt_off : pt_off + 2] != _PT_MAGIC:
        return None
    try:
        app_off = production_layout_from_partition_table(parse_partition_table(data[pt_off : pt_off + 0xC00]))[
            "app_offset"
        ]
    except ValueError:
        return None
    if len(data) <= app_off + 0x100 or data[app_off] != 0xE9:
        return None
    desc_off = app_off + 0x20  # esp_app_desc_t; magic 0xABCD5432 (little-endian)
    if len(data) >= desc_off + 0x50 and struct.unpack_from("<I", data, desc_off)[0] == 0xABCD5432:
        project = data[desc_off + 0x30 : desc_off + 0x50].split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
        if "panel" in project:
            return "panel"
        if "unit" in project:
            return "apartment"
    return "unknown"


# ---- Burn profile (ADR 0015, Option B): Secure Boot v2 + release flash encryption + NVS encryption ----

BURN_OVERLAY_PATH = PROJECT_ROOT / "firmware" / "sdkconfig.burn"
# sdkconfig defaults layer with the GUI settings, written into each profile's build directory
FLASHER_DEFAULTS_NAME = "sdkconfig.flasher"
BURN_CONFIRM_WORD = "BURN"
_SDKCONFIG_LINE_RE = re.compile(r"^(?:# )?(CONFIG_[A-Z0-9_]+)(?:=.*| is not set)$")

# Partition types / subtypes (esp_partition.h)
_PT_MAGIC = b"\xaa\x50"
_PT_MD5_MAGIC = b"\xeb\xeb"
_PT_TYPE_APP, _PT_TYPE_DATA = 0x00, 0x01
_PT_SUBTYPE_FACTORY, _PT_SUBTYPE_NVS, _PT_SUBTYPE_NVS_KEYS = 0x00, 0x02, 0x04
_PT_FLAG_ENCRYPTED = 0x1

# Secure Boot v2 RSA-3072 signature sector layout (espsecure.py / secure_boot_signature_priv.h)
_SBV2_SECTOR_SIZE = 0x1000
_SBV2_SIG_BLOCK_SIZE = 1216
_SBV2_SIG_MAGIC = 0xE7
_SBV2_SIG_VERSION_RSA = 0x02


def read_sdkconfig_overlay(path: Path) -> dict[str, str]:
    """CONFIG name -> authoritative line ("CONFIG_X=..." or "# CONFIG_X is not set")."""
    overlay: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _SDKCONFIG_LINE_RE.match(line.strip())
        if m:
            overlay[m.group(1)] = line.strip()
    return overlay


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


def production_layout_from_partition_table(entries: list[dict[str, Any]]) -> dict[str, int]:
    """nvs and factory app of a Production image's partition table; raises ValueError if either is missing."""
    nvs = [e for e in entries if e["type"] == _PT_TYPE_DATA and e["subtype"] == _PT_SUBTYPE_NVS]
    app = [e for e in entries if e["type"] == _PT_TYPE_APP and e["subtype"] == _PT_SUBTYPE_FACTORY]
    if len(nvs) != 1 or len(app) != 1:
        raise ValueError("partition table must contain exactly one nvs and one factory app partition")
    return {
        "nvs_offset": nvs[0]["offset"],
        "nvs_size": nvs[0]["size"],
        "app_offset": app[0]["offset"],
        "app_size": app[0]["size"],
    }


def read_flasher_args(build_dir: Path) -> dict[str, Any]:
    """ESP-IDF's own record of a build's flash layout and settings (build/flasher_args.json): image offsets and files
    (bootloader, partition table, app) and flash mode / frequency / size, so nothing is hard-coded here."""
    args = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))

    def image(name: str) -> tuple[int, Path]:
        return int(args[name]["offset"], 0), build_dir / args[name]["file"]

    return {
        "flash_settings": dict(args["flash_settings"]),
        "bootloader": image("bootloader"),
        "partition_table": image("partition-table"),
        "app": image("app"),
    }


def merge_bin_cmd(
    python_exe: str, out_path: Path, flash_settings: dict[str, str], images: list[tuple[int, Path]]
) -> list[str]:
    """esptool merge-bin command for @p images; flash_settings "keep" leaves signed image headers untouched."""
    cmd = [python_exe, "-m", "esptool", "--chip", "esp32", "merge-bin", "-o", str(out_path)]
    for opt in ("flash_mode", "flash_freq", "flash_size"):
        cmd += [f"--{opt.replace('_', '-')}", flash_settings[opt]]
    for offset, path in images:
        cmd += [hex(offset), str(path)]
    return cmd


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
        rd_dis = field("RD_DIS")
        if not rd_dis.get("writeable"):
            problems.append("RD_DIS is write-protected (cannot read-protect BLOCK1)")
        if int(rd_dis["value"]) & 0x3:
            problems.append("RD_DIS already disables reading BLOCK1 or BLOCK2")
        fcc = field("FLASH_CRYPT_CONFIG")
        fcc_val = int(fcc["value"])
        if fcc_val not in (0, 0xF):
            problems.append(f"FLASH_CRYPT_CONFIG has unexpected value {fcc['value']} (expected 0 or 0xF)")
        elif fcc_val == 0 and not fcc.get("writeable"):
            problems.append("FLASH_CRYPT_CONFIG is write-protected")
        if field("DIS_CACHE")["value"]:
            problems.append("DIS_CACHE is set (flash cache is disabled)")
        for name in (
            "JTAG_DISABLE",
            "CONSOLE_DEBUG_DISABLE",
            "DISABLE_DL_ENCRYPT",
            "DISABLE_DL_DECRYPT",
            "DISABLE_DL_CACHE",
        ):
            entry = field(name)
            if not entry["value"] and not entry.get("writeable"):
                problems.append(f"{name} is write-protected")
        for name in ("FLASH_CRYPT_CNT", "ABS_DONE_1", "UART_DOWNLOAD_DIS"):
            if not field(name).get("writeable"):
                problems.append(f"{name} is write-protected")
        if field("UART_DOWNLOAD_DIS")["value"]:
            problems.append("UART download mode is already disabled")
    except (KeyError, TypeError, ValueError) as exc:
        problems.append(f"espefuse summary lacks or has an unexpected field {exc}")
    return problems


def verify_sbv2_signed_blob(blob: bytes, expected_key_digest_hex: str, label: str) -> None:
    """Verify an RSA-3072 Secure Boot v2 signed image's digest, key digest and PSS signature."""
    if len(blob) < 2 * _SBV2_SECTOR_SIZE or len(blob) % _SBV2_SECTOR_SIZE != 0 or blob[0] != 0xE9:
        raise ValueError(f"{label} is not a valid 4 KB-aligned ESP32 image")

    sig_sector = blob[-_SBV2_SECTOR_SIZE:]
    sig_blk = sig_sector[:_SBV2_SIG_BLOCK_SIZE]
    if sig_sector[_SBV2_SIG_BLOCK_SIZE:] != b"\xff" * (_SBV2_SECTOR_SIZE - _SBV2_SIG_BLOCK_SIZE):
        raise ValueError(f"{label} signature sector has unexpected trailing bytes")

    magic, version, image_digest, n_le, e, _rinv_le, _m, sig_le, blk_crc, pad = struct.unpack(
        "<BBxx32s384sI384sI384sI16s", sig_blk
    )
    if magic != _SBV2_SIG_MAGIC:
        raise ValueError(f"{label} signature block has invalid magic 0x{magic:02x}")
    if version != _SBV2_SIG_VERSION_RSA:
        raise ValueError(f"{label} signature block has unexpected version 0x{version:02x}")
    if pad != b"\x00" * 16:
        raise ValueError(f"{label} signature block has non-zero padding")
    if blk_crc != (zlib.crc32(sig_blk[:1196]) & 0xFFFFFFFF):
        raise ValueError(f"{label} signature block CRC32 mismatch")
    if image_digest != hashlib.sha256(blob[:-_SBV2_SECTOR_SIZE]).digest():
        raise ValueError(f"{label} SHA-256 digest does not match signature block")

    key_digest = hashlib.sha256(sig_blk[36:812]).hexdigest()
    if key_digest != expected_key_digest_hex.lower():
        raise ValueError(f"{label} public key digest does not match build metadata")

    try:
        pub_key = rsa.RSAPublicNumbers(e, int.from_bytes(n_le, "little")).public_key()
        pub_key.verify(
            sig_le[::-1],
            image_digest,
            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
            utils.Prehashed(hashes.SHA256()),
        )
    except (InvalidSignature, ValueError) as exc:
        raise ValueError(f"{label} RSA-3072 signature verification failed") from exc


def verify_burn_image_signatures(raw: bytes, info: dict[str, Any]) -> None:
    """Verify the signed bootloader and signed application embedded in a merged Burn binary."""
    pt_off = int(info["partition_table_offset"])
    app_off = int(info["app_offset"])
    expected_digest = str(info.get("secure_boot_key_digest", "")).strip().lower()
    if len(expected_digest) != 64 or any(c not in "0123456789abcdef" for c in expected_digest):
        raise ValueError("build info lacks a valid 64-hex secure_boot_key_digest")
    if len(raw) <= app_off or pt_off <= 0x1000:
        raise ValueError("merged Burn image offsets are invalid")

    boot_region = raw[0x1000:pt_off]
    if "signed_bootloader_size" in info:
        boot_size = int(info["signed_bootloader_size"])
        if boot_size <= 0 or boot_size > len(boot_region):
            raise ValueError("signed_bootloader_size in build info exceeds bootloader slot")
        if boot_region[boot_size:] != b"\xff" * (len(boot_region) - boot_size):
            raise ValueError("unexpected data between signed bootloader and partition table")
        signed_boot = boot_region[:boot_size]
    else:
        empty_sector = b"\xff" * _SBV2_SECTOR_SIZE
        end = len(boot_region)
        while end >= _SBV2_SECTOR_SIZE and boot_region[end - _SBV2_SECTOR_SIZE : end] == empty_sector:
            end -= _SBV2_SECTOR_SIZE
        signed_boot = boot_region[:end]

    signed_app = raw[app_off:]
    if "signed_app_size" in info and len(signed_app) != int(info["signed_app_size"]):
        raise ValueError("merged Burn image length does not match signed_app_size")

    verify_sbv2_signed_blob(signed_boot, expected_digest, "signed bootloader")
    verify_sbv2_signed_blob(signed_app, expected_digest, "signed application")


# ---- Burn build: one-time key, signing, merge and self-check (shared by the flasher and CI) ----

RunCmd = Callable[[list[str]], int]

BURN_BOOTLOADER_OFFSET = 0x1000


def generate_burn_signing_key(python_exe: str, run: RunCmd, key_path: Path) -> None:
    """Create the one-time RSA-3072 Secure Boot v2 key of a Burn build (ADR 0015); raises RuntimeError on failure."""
    rc = run(
        [python_exe, "-m", "espsecure", "generate-signing-key", "--version", "2", "--scheme", "rsa3072", str(key_path)]
    )
    if rc != 0 or not key_path.is_file():
        raise RuntimeError("could not create the one-time signing key")


def sign_and_merge_burn_image(
    python_exe: str,
    run: RunCmd,
    build_dir: Path,
    overlay: dict[str, str],
    signing_key: Path,
    out_path: Path,
) -> dict[str, Any]:
    """Check a Burn build (resolved overlay, partition table, sizes), sign bootloader + app with @p signing_key, merge
    them into @p out_path and verify the merged image's signatures.

    @p run executes one command and returns its exit code. Returns the layout facts for the build-info sidecar;
    raises ValueError / RuntimeError with the reason (out_path is removed then).
    """
    sdkconfig = build_dir / "sdkconfig"
    problems = check_sdkconfig_overlay(sdkconfig, overlay)
    if "CONFIG_COMPILER_OPTIMIZATION_SIZE=y" not in sdkconfig.read_text(encoding="utf-8", errors="ignore").splitlines():
        problems.append("expected 'CONFIG_COMPILER_OPTIMIZATION_SIZE=y'")
    if problems:
        raise ValueError("the resolved Burn configuration differs from firmware/sdkconfig.burn: " + "; ".join(problems))

    flash = read_flasher_args(build_dir)
    pt_offset, part_bin = flash["partition_table"]
    boot_offset, bootloader_bin = flash["bootloader"]
    _app_offset, app_bin = flash["app"]
    if boot_offset != BURN_BOOTLOADER_OFFSET:
        raise ValueError(f"unexpected bootloader offset 0x{boot_offset:x}")
    if pt_offset != int(overlay["CONFIG_PARTITION_TABLE_OFFSET"].split("=", 1)[1], 0):
        raise ValueError("partition table offset differs from CONFIG_PARTITION_TABLE_OFFSET in sdkconfig.burn")
    part_data = part_bin.read_bytes()
    layout = burn_layout_from_partition_table(parse_partition_table(part_data))

    signed_dir = build_dir / "signed"
    signed_dir.mkdir(exist_ok=True)
    signed_boot, signed_app = signed_dir / "bootloader-signed.bin", signed_dir / "app-signed.bin"
    digest_file = signed_dir / "secure_boot_key_digest.bin"
    espsecure = [python_exe, "-m", "espsecure"]
    for src, dst in ((bootloader_bin, signed_boot), (app_bin, signed_app)):
        dst.unlink(missing_ok=True)
        if run(
            espsecure + ["sign-data", "--version", "2", "--keyfile", str(signing_key), "--output", str(dst), str(src)]
        ):
            raise RuntimeError(f"espsecure sign-data failed for {src.name}")
        if run(espsecure + ["verify-signature", "--version", "2", "--keyfile", str(signing_key), str(dst)]):
            raise RuntimeError(f"signature verification failed for {dst.name}")
    digest_file.unlink(missing_ok=True)
    if run(espsecure + ["digest-sbv2-public-key", "--keyfile", str(signing_key), "--output", str(digest_file)]):
        raise RuntimeError("cannot compute the public key digest")

    boot_size, app_size = signed_boot.stat().st_size, signed_app.stat().st_size
    if boot_offset + boot_size > pt_offset:
        raise ValueError(
            f"signed bootloader ends at 0x{boot_offset + boot_size:x}, past the partition table at 0x{pt_offset:x}; "
            "raise CONFIG_PARTITION_TABLE_OFFSET in firmware/sdkconfig.burn and move partitions_burn.csv"
        )
    if app_size > layout["app_size"]:
        raise ValueError(f"signed app ({app_size} B) does not fit the factory partition ({layout['app_size']} B)")

    # "keep": rewriting the flash parameters in a signed image header would invalidate its signature
    keep = {"flash_mode": "keep", "flash_freq": "keep", "flash_size": "keep"}
    images = [(boot_offset, signed_boot), (pt_offset, part_bin), (layout["app_offset"], signed_app)]
    if run(merge_bin_cmd(python_exe, out_path, keep, images)) != 0 or not out_path.is_file():
        raise RuntimeError("esptool merge-bin failed")

    info = {
        "partition_table_offset": pt_offset,
        **layout,
        "signed_bootloader_size": boot_size,
        "signed_app_size": app_size,
        "secure_boot_key_digest": digest_file.read_bytes().hex(),
        "chip_rev_min": "v3.0",
    }
    try:
        merged = out_path.read_bytes()
        for offset, path in images:
            part = path.read_bytes()
            if merged[offset : offset + len(part)] != part:
                raise ValueError(f"merged image differs from the signed input at 0x{offset:x}")
        verify_burn_image_signatures(merged, info)
    except ValueError:
        out_path.unlink(missing_ok=True)
        raise
    return info
