#!/usr/bin/env python3
"""
Factory NVS Provisioning Tool for Blank Entrance Panels (I-5, I-14, I-50).

Generates unique random per-device factory admin and user codes (CSPRNG),
packs the exact CRC32-protected `doorbell_panel_cfg_blob_t` binary structure,
and invokes ESP-IDF's `nvs_partition_gen.py` to produce a factory NVS image.

WARNING (I-14): Flashing an NVS image replaces the entire NVS partition and
wipes any paired apartments. Use ONLY during initial factory flashing of blank
devices. In the field, use the `*<admin>*5#` web portal or `doorbell_wifi`
UART console command.
"""

import argparse
import csv
import os
import secrets
import struct
import subprocess
import sys
import tempfile
import zlib

PANEL_CFG_MAGIC = 0x44425043  # 'DBPC'
BLOB_VERSION = 1

# C struct layout of doorbell_panel_cfg_blob_t (little-endian, 168 bytes total):
#   uint32_t magic;
#   uint16_t version;
#   uint16_t reserved;
#   char wifi_ssid[33];
#   char wifi_pass[65];
#   uint8_t pad[2];
#   char admin_code[12];
#   char user_code[12];
#   char factory_admin_code[12];
#   char factory_user_code[12];
#   uint16_t wrong_code_count;
#   uint16_t lockout_level;
#   uint32_t lockout_remaining_s;
#   uint32_t crc32;
PANEL_CFG_FORMAT_NO_CRC = "<IHH33s65s2s12s12s12s12sHHI"
PANEL_CFG_FORMAT_FULL = PANEL_CFG_FORMAT_NO_CRC + "I"


def generate_numeric_code(length: int) -> str:
    rng = secrets.SystemRandom()
    return "".join(str(rng.randrange(0, 10)) for _ in range(length))


def pack_panel_cfg_blob(
    wifi_ssid: str,
    wifi_pass: str,
    admin_code: str,
    user_code: str,
) -> bytes:
    body = struct.pack(
        PANEL_CFG_FORMAT_NO_CRC,
        PANEL_CFG_MAGIC,
        BLOB_VERSION,
        0,
        wifi_ssid.encode("utf-8")[:32],
        wifi_pass.encode("utf-8")[:64],
        b"\x00\x00",
        admin_code.encode("ascii")[:10],
        user_code.encode("ascii")[:8],
        admin_code.encode("ascii")[:10],
        user_code.encode("ascii")[:8],
        0,
        0,
        0,
    )
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a unique factory NVS binary image for a blank DorbellAp panel."
    )
    parser.add_argument("--ssid", default="", help="Initial building Wi-Fi SSID (optional)")
    parser.add_argument("--password", default="", help="Initial building Wi-Fi password (optional)")
    parser.add_argument("--admin-len", type=int, default=8, choices=range(8, 11), help="Admin code length (8-10)")
    parser.add_argument("--user-len", type=int, default=4, choices=range(4, 9), help="User code length (4-8)")
    parser.add_argument("--output", required=True, help="Output factory NVS binary path (e.g., panel_nvs.bin)")
    args = parser.parse_args()

    admin_code = generate_numeric_code(args.admin_len)
    user_code = generate_numeric_code(args.user_len)
    while user_code == admin_code:
        user_code = generate_numeric_code(args.user_len)

    blob = pack_panel_cfg_blob(args.ssid, args.password, admin_code, user_code)

    idf_path = os.environ.get("IDF_PATH", r"C:\esp\v6.1\esp-idf")
    nvs_gen_script = os.path.join(
        idf_path, "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py"
    )
    if not os.path.isfile(nvs_gen_script):
        print(f"Error: nvs_partition_gen.py not found at {nvs_gen_script}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmpdir:
        csv_path = os.path.join(tmpdir, "nvs.csv")
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["key", "type", "encoding", "value"])
            writer.writerow(["doorbell", "namespace", "", ""])
            writer.writerow(["panel_cfg", "data", "hex2bin", blob.hex()])

        cmd = [sys.executable, nvs_gen_script, "generate", csv_path, args.output, "0x6000"]
        subprocess.run(cmd, check=True)

    print("=== Factory Panel Provisioning Complete ===")
    print(f"Output NVS Image : {args.output} (24 KB, offset 0x9000)")
    print(f"Factory Admin Code ({args.admin_len} digits): {admin_code}")
    print(f"Factory User Code  ({args.user_len} digits): {user_code}")
    print("Print these codes on the panel's physical label before shipping.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
