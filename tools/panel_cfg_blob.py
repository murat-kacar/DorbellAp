"""Host-side packing of the panel's NVS `panel_cfg` record (`doorbell_panel_cfg_blob_t`, I-5, I-50).

Used by the flasher (Flash / Burn write the codes and Wi-Fi together with the firmware) and by
tools/verify_protocol_vectors.py. The layout must stay identical to components/doorbell_storage.
"""

import struct
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


def pack_panel_cfg_blob(wifi_ssid: str, wifi_pass: str, admin_code: str, user_code: str) -> bytes:
    """Fresh record: the given codes are both the active and the factory codes, no lockout."""
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
