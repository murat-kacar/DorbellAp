#!/usr/bin/env python3
"""
Host Verification Suite for DorbellAp Python Tools & Protocol Vectors.

Verifies:
1. NVS Blob Binary Layouts & CRC32 (`panel_cfg` = 168 B, `panel_apt` = 48 B, `unit_state` = 152 B)
   + Write-Ahead Counter (`+100`).
2. 12-byte AAD Header Serialization, Transport Flag Masking (`is_retry`, `is_bcast`), and 13-byte AES-128-CCM
   Nonce Layout.
3. X25519 ECDH + HKDF-SHA256 (`"DorbellAp-Pair-v1"`) Pairing Key Agreement & Provisioning Blob AEAD.
4. Protocol Payload Sizes & Shared Wi-Fi Credential Validation (`validate_wifi_credentials`).
5. Burn Profile Helpers, eFuse Pre-Check & SBv2 Signature Verification.
"""

import copy
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils, x25519
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "flasher"))
from flasher_images import (  # noqa: E402
    PANEL_CFG_FORMAT_FULL,
    burn_efuse_problems,
    burn_layout_from_partition_table,
    make_nvs_keys_partition,
    pack_panel_cfg_blob,
    parse_partition_table,
    validate_wifi_credentials,
    verify_burn_image_signatures,
    verify_sbv2_signed_blob,
)

# ---------------------------------------------------------------------------
# 1. NVS Blob Layout & Write-Ahead Counter Tests
# ---------------------------------------------------------------------------


# Same vector as GOLDEN_PANEL_CFG_HEX in test/host_unit/main/test_main.c, where the firmware's doorbell_storage must
# load it: a field-order change on either side breaks one of the two suites
GOLDEN_PANEL_CFG_HEX = (
    "435042440100000042696e61204150000000000000000000000000000000000000000000000000000070617373776f7264313233"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000313233343536373800000000313233340000000000000000313233343536373800000000313233340000000000000000"
    "000000000000000030bc7bd7"
)


def test_nvs_blob_layouts_and_write_ahead() -> None:
    # 1. doorbell_panel_cfg_blob_t: 168 bytes
    assert struct.calcsize(PANEL_CFG_FORMAT_FULL) == 168
    assert pack_panel_cfg_blob("Bina AP", "password123", "12345678", "1234").hex() == GOLDEN_PANEL_CFG_HEX
    blob = pack_panel_cfg_blob("Building-AP", "SecretPass123", "12345678", "1234")
    assert len(blob) == 168
    expected_crc = zlib.crc32(blob[:-4]) & 0xFFFFFFFF
    (stored_crc,) = struct.unpack("<I", blob[-4:])
    assert stored_crc == expected_crc

    # 2. doorbell_panel_apt_blob_t: 48 bytes
    apt_fmt = "<IHH6s2s16sIIII"
    assert struct.calcsize(apt_fmt) == 48

    # 3. doorbell_unit_state_blob_t: 152 bytes
    unit_fmt = "<IHBB6sBB33s65s2sI16sHHIII"
    assert struct.calcsize(unit_fmt) == 152

    # 4. Write-ahead counter simulation (block_size = 100)
    ceiling = 0
    runtime_counter = 0
    nvs_commits = 0
    for i in range(1, 251):
        nxt = runtime_counter + 1
        if nxt > ceiling:
            ceiling = nxt + 100 - 1
            nvs_commits += 1
        runtime_counter = nxt
        assert runtime_counter <= ceiling
    # 250 packets require exactly 3 NVS commits (1..100, 101..200, 201..300)
    assert nvs_commits == 3
    assert ceiling == 300
    # Simulate power loss and reboot at counter 250 -> resumes from ceiling 300, next packet is 301
    runtime_after_reboot = ceiling
    next_after_reboot = runtime_after_reboot + 1
    assert next_after_reboot == 301


# ---------------------------------------------------------------------------
# 2. Wire Framing, Nonce Layout & Dual-Path Broadcast AAD Masking
# ---------------------------------------------------------------------------


def test_wire_framing_and_ccm_aead() -> None:
    k_apt = os.urandom(16)
    magic = 0xDB
    version = 0x01
    msg_type = 0x03  # OPEN_DOOR_REQ
    flags_initial = 0x01  # direction = Unit->Panel
    flags_bcast_retry = 0x01 | 0x02 | 0x04  # direction=1, is_retry=1, is_bcast=1
    src_id = 12
    dst_id = 0
    seq_id = 42
    tx_counter = 101

    aad = struct.pack(">BBBBBBHI", magic, version, msg_type, flags_initial & 0x01, src_id, dst_id, seq_id, tx_counter)
    assert len(aad) == 12

    nonce = struct.pack(">BBBHI4s", 1, src_id, dst_id, seq_id, tx_counter, b"\x00\x00\x00\x00")
    assert len(nonce) == 13

    payload = struct.pack(">IIQ", 0xC0A8012A, 0xCAFEBABE, 0x0102030405060708)
    assert len(payload) == 16

    aesccm = AESCCM(k_apt, tag_length=16)
    ciph_and_tag = aesccm.encrypt(nonce, payload, aad)
    assert len(ciph_and_tag) == 32

    # Mutating transport flags (is_retry, is_bcast) in wire header still authenticates after masking byte 3 to 0x01
    wire_hdr_retry = struct.pack(
        ">BBBBBBHI", magic, version, msg_type, flags_bcast_retry, src_id, dst_id, seq_id, tx_counter
    )
    rx_aad = bytearray(wire_hdr_retry)
    rx_aad[3] &= 0x01
    decrypted = aesccm.decrypt(nonce, ciph_and_tag, bytes(rx_aad))
    assert decrypted == payload

    # Tampering with src_id or tx_counter in header must fail AEAD authentication
    tampered_aad = bytearray(rx_aad)
    tampered_aad[4] = 13  # spoofed src_id
    try:
        aesccm.decrypt(nonce, ciph_and_tag, bytes(tampered_aad))
    except InvalidTag:
        pass
    else:
        raise AssertionError("Tampered AAD should have failed AEAD verification")


# ---------------------------------------------------------------------------
# 3. X25519 + HKDF-SHA256 Key Agreement & Provisioning Blob Tests
# ---------------------------------------------------------------------------


def test_x25519_hkdf_derivation() -> None:
    panel_priv = x25519.X25519PrivateKey.generate()
    unit_priv = x25519.X25519PrivateKey.generate()

    shared_p = panel_priv.exchange(unit_priv.public_key())
    shared_u = unit_priv.exchange(panel_priv.public_key())
    assert shared_p == shared_u

    panel_nonce = os.urandom(16)
    unit_nonce = os.urandom(16)
    panel_mac = b"\xaa\xbb\xcc\xdd\xee\x01"
    unit_mac = b"\xaa\xbb\xcc\xdd\xee\x02"
    apt_no = 7

    salt = panel_nonce + unit_nonce
    info = b"DorbellAp-Pair-v1" + panel_mac + unit_mac + bytes([apt_no])
    assert len(salt) == 32
    assert len(info) == 30

    k_pair_p = HKDF(algorithm=hashes.SHA256(), length=16, salt=salt, info=info).derive(shared_p)
    k_pair_u = HKDF(algorithm=hashes.SHA256(), length=16, salt=salt, info=info).derive(shared_u)
    assert k_pair_p == k_pair_u
    assert len(k_pair_p) == 16


# ---------------------------------------------------------------------------
# 4. Protocol Payload Sizes & Wi-Fi Credential Validation
# ---------------------------------------------------------------------------


def test_payload_sizes() -> None:
    assert struct.calcsize(">II") == 8  # RING_REQ
    assert struct.calcsize(">IHBI") == 11  # RING_ACK
    assert struct.calcsize(">IIQ") == 16  # OPEN_DOOR_REQ
    assert struct.calcsize(">IHBIIQ") == 23  # OPEN_DOOR_ACK
    assert struct.calcsize(">IIIIHHHHBbB6sBBH") == 37  # HEARTBEAT_REQ
    assert struct.calcsize(">IHBII") == 15  # HEARTBEAT_ACK / PAIR_COMPLETE_ACK
    assert struct.calcsize(">I6sBB") == 12  # PAIR_COMPLETE


def test_wifi_credential_validation() -> None:
    # Valid credentials
    assert validate_wifi_credentials("Bina-AP", "SecretPass123", allow_empty_ssid=False) is None
    assert validate_wifi_credentials("Open-AP", "", allow_empty_ssid=False) is None
    assert validate_wifi_credentials("", "", allow_empty_ssid=True) is None
    assert validate_wifi_credentials("A" * 32, "B" * 63, allow_empty_ssid=False) is None

    # Empty SSID handling
    assert validate_wifi_credentials("", "SecretPass123", allow_empty_ssid=False) is not None
    assert validate_wifi_credentials("", "", allow_empty_ssid=False) is not None
    assert validate_wifi_credentials("", "SecretPass123", allow_empty_ssid=True) is not None

    # Control characters rejected in SSID and password
    for bad in ("\x00", "\r", "\n"):
        assert validate_wifi_credentials(f"AP{bad}1", "SecretPass123", allow_empty_ssid=False) is not None
        assert validate_wifi_credentials("Bina-AP", f"Pass{bad}1234", allow_empty_ssid=False) is not None

    # UTF-8 SSID byte limit (>32 bytes)
    assert validate_wifi_credentials("ğ" * 16, "SecretPass123", allow_empty_ssid=False) is None  # 32 UTF-8 bytes
    assert validate_wifi_credentials("ğ" * 16 + "a", "SecretPass123", allow_empty_ssid=False) is not None  # 33 bytes

    # WPA2 passphrase length bounds (0 allowed for open AP, otherwise 8..63 bytes)
    assert validate_wifi_credentials("Bina-AP", "1234567", allow_empty_ssid=False) is not None
    assert validate_wifi_credentials("Bina-AP", "12345678", allow_empty_ssid=False) is None
    assert validate_wifi_credentials("Bina-AP", "B" * 64, allow_empty_ssid=False) is not None


# ---------------------------------------------------------------------------
# 5. Burn Profile Helpers, eFuse Pre-Check & SBv2 Signature Verification
# ---------------------------------------------------------------------------


def _expect_value_error(fn, *args) -> None:
    try:
        fn(*args)
    except ValueError:
        return
    raise AssertionError(f"Expected ValueError from {fn.__name__}")


def _make_synthetic_sbv2_signed_image(priv_key: rsa.RSAPrivateKey, payload_fill: int) -> tuple[bytes, str]:
    payload = bytes([0xE9]) + bytes([payload_fill]) * 4095
    img_digest = hashlib.sha256(payload).digest()
    pub_nums = priv_key.public_key().public_numbers()
    n_le = pub_nums.n.to_bytes(384, "little")
    rinv_le = pow(2, 6144, pub_nums.n).to_bytes(384, "little")
    m = (-pow(pub_nums.n, -1, 2**32)) & 0xFFFFFFFF
    sig_le = priv_key.sign(
        img_digest,
        padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
        utils.Prehashed(hashes.SHA256()),
    )[::-1]
    pre_crc = struct.pack("<BBxx32s384sI384sI384s", 0xE7, 0x02, img_digest, n_le, pub_nums.e, rinv_le, m, sig_le)
    blk_crc = zlib.crc32(pre_crc) & 0xFFFFFFFF
    sig_blk = pre_crc + struct.pack("<I16s", blk_crc, b"\x00" * 16)
    sig_sector = sig_blk + b"\xff" * (4096 - len(sig_blk))
    key_digest_hex = hashlib.sha256(sig_blk[36:812]).hexdigest()
    return payload + sig_sector, key_digest_hex


def _make_partition_entry(ptype: int, subtype: int, offset: int, size: int, label: str, flags: int = 0) -> bytes:
    lbl = label.encode("ascii")[:16].ljust(16, b"\x00")
    return struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size, lbl, flags)


def test_burn_helpers_and_sbv2_verification() -> None:
    # 1. eFuse pre-check rules (burn_efuse_problems)
    zero_block = " ".join(["00"] * 32)
    fresh_summary = {
        "WAFER_VERSION_MAJOR": {"value": 3, "readable": True, "writeable": True},
        "CODING_SCHEME": {"value": "NONE (BLK1-3 len=256 bits)", "readable": True, "writeable": True},
        "FLASH_CRYPT_CNT": {"value": 0, "readable": True, "writeable": True},
        "FLASH_CRYPT_CONFIG": {"value": 0, "readable": True, "writeable": True},
        "RD_DIS": {"value": 0, "readable": True, "writeable": True},
        "ABS_DONE_0": {"value": False, "readable": True, "writeable": True},
        "ABS_DONE_1": {"value": False, "readable": True, "writeable": True},
        "BLOCK1": {"value": zero_block, "readable": True, "writeable": True},
        "BLOCK2": {"value": zero_block, "readable": True, "writeable": True},
        "UART_DOWNLOAD_DIS": {"value": False, "readable": True, "writeable": True},
        "DIS_CACHE": {"value": False, "readable": True, "writeable": True},
        "JTAG_DISABLE": {"value": False, "readable": True, "writeable": True},
        "CONSOLE_DEBUG_DISABLE": {"value": False, "readable": True, "writeable": True},
        "DISABLE_DL_ENCRYPT": {"value": False, "readable": True, "writeable": True},
        "DISABLE_DL_DECRYPT": {"value": False, "readable": True, "writeable": True},
        "DISABLE_DL_CACHE": {"value": False, "readable": True, "writeable": True},
    }
    assert burn_efuse_problems(fresh_summary) == []

    # Value 0xF for FLASH_CRYPT_CONFIG is allowed even if already write-protected
    cfg_f = copy.deepcopy(fresh_summary)
    cfg_f["FLASH_CRYPT_CONFIG"] = {"value": 0xF, "readable": True, "writeable": False}
    assert burn_efuse_problems(cfg_f) == []

    # Reject legacy rev, non-NONE coding scheme, active SB/FE, non-zero BLOCK1/2, protected WR/RD bits, DIS_CACHE
    reject_cases = [
        ("WAFER_VERSION_MAJOR", {"value": 1, "readable": True, "writeable": True}),
        ("CODING_SCHEME", {"value": "3/4 (BLK1-3 len=192 bits)", "readable": True, "writeable": True}),
        ("FLASH_CRYPT_CNT", {"value": 1, "readable": True, "writeable": True}),
        ("FLASH_CRYPT_CNT", {"value": 0, "readable": True, "writeable": False}),
        ("ABS_DONE_0", {"value": True, "readable": True, "writeable": True}),
        ("ABS_DONE_1", {"value": True, "readable": True, "writeable": True}),
        ("ABS_DONE_1", {"value": False, "readable": True, "writeable": False}),
        ("BLOCK1", {"value": "aa " + " ".join(["00"] * 31), "readable": True, "writeable": True}),
        ("BLOCK2", {"value": zero_block, "readable": False, "writeable": True}),
        ("RD_DIS", {"value": 0, "readable": True, "writeable": False}),
        ("RD_DIS", {"value": 0x1, "readable": True, "writeable": True}),
        ("RD_DIS", {"value": 0x2, "readable": True, "writeable": True}),
        ("FLASH_CRYPT_CONFIG", {"value": 0x5, "readable": True, "writeable": True}),
        ("FLASH_CRYPT_CONFIG", {"value": 0, "readable": True, "writeable": False}),
        ("DIS_CACHE", {"value": True, "readable": True, "writeable": True}),
        ("JTAG_DISABLE", {"value": False, "readable": True, "writeable": False}),
        ("CONSOLE_DEBUG_DISABLE", {"value": False, "readable": True, "writeable": False}),
        ("DISABLE_DL_ENCRYPT", {"value": False, "readable": True, "writeable": False}),
        ("DISABLE_DL_DECRYPT", {"value": False, "readable": True, "writeable": False}),
        ("DISABLE_DL_CACHE", {"value": False, "readable": True, "writeable": False}),
        ("UART_DOWNLOAD_DIS", {"value": True, "readable": True, "writeable": True}),
        ("UART_DOWNLOAD_DIS", {"value": False, "readable": True, "writeable": False}),
    ]
    for key, bad_val in reject_cases:
        mut = copy.deepcopy(fresh_summary)
        mut[key] = bad_val
        assert burn_efuse_problems(mut), f"burn_efuse_problems unexpectedly accepted {key}={bad_val}"

    # 2. make_nvs_keys_partition() format
    nvs_keys = make_nvs_keys_partition()
    assert len(nvs_keys) == 0x1000
    (stored_crc,) = struct.unpack_from("<I", nvs_keys, 64)
    assert stored_crc == (zlib.crc32(nvs_keys[:64], 0xFFFFFFFF) & 0xFFFFFFFF)
    assert nvs_keys[68:] == b"\xff" * (0x1000 - 68)

    # 3. parse_partition_table() and burn_layout_from_partition_table()
    valid_pt = (
        _make_partition_entry(0x01, 0x02, 0x11000, 0x6000, "nvs", flags=0)
        + _make_partition_entry(0x01, 0x04, 0x17000, 0x1000, "nvs_keys", flags=1)
        + _make_partition_entry(0x00, 0x00, 0x20000, 0x100000, "factory", flags=0)
        + b"\xff" * 32
    )
    layout = burn_layout_from_partition_table(parse_partition_table(valid_pt))
    assert layout == {
        "nvs_offset": 0x11000,
        "nvs_size": 0x6000,
        "nvs_keys_offset": 0x17000,
        "nvs_keys_size": 0x1000,
        "app_offset": 0x20000,
        "app_size": 0x100000,
    }
    bad_nvs_pt = (
        _make_partition_entry(0x01, 0x02, 0x11000, 0x6000, "nvs", flags=1)
        + _make_partition_entry(0x01, 0x04, 0x17000, 0x1000, "nvs_keys", flags=1)
        + _make_partition_entry(0x00, 0x00, 0x20000, 0x100000, "factory", flags=0)
    )
    _expect_value_error(burn_layout_from_partition_table, parse_partition_table(bad_nvs_pt))
    bad_keys_pt = (
        _make_partition_entry(0x01, 0x02, 0x11000, 0x6000, "nvs", flags=0)
        + _make_partition_entry(0x01, 0x04, 0x17000, 0x1000, "nvs_keys", flags=0)
        + _make_partition_entry(0x00, 0x00, 0x20000, 0x100000, "factory", flags=0)
    )
    _expect_value_error(burn_layout_from_partition_table, parse_partition_table(bad_keys_pt))

    # 4. verify_sbv2_signed_blob() and verify_burn_image_signatures()
    priv = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    signed_boot, digest_hex = _make_synthetic_sbv2_signed_image(priv, 0xAA)
    signed_app, digest_hex2 = _make_synthetic_sbv2_signed_image(priv, 0x55)
    assert digest_hex == digest_hex2
    verify_sbv2_signed_blob(signed_boot, digest_hex, "test boot")

    # Tamper payload -> SHA mismatch
    tampered_payload = bytearray(signed_boot)
    tampered_payload[100] ^= 0x01
    _expect_value_error(verify_sbv2_signed_blob, bytes(tampered_payload), digest_hex, "tampered payload")

    # Tamper signature block CRC
    tampered_crc = bytearray(signed_boot)
    tampered_crc[-4096 + 1196] ^= 0x01
    _expect_value_error(verify_sbv2_signed_blob, bytes(tampered_crc), digest_hex, "tampered crc")

    # Tamper signature (with updated CRC to reach PSS verification)
    tampered_sig = bytearray(signed_boot)
    sig_start = len(tampered_sig) - 4096
    tampered_sig[sig_start + 812] ^= 0x01
    struct.pack_into(
        "<I",
        tampered_sig,
        sig_start + 1196,
        zlib.crc32(bytes(tampered_sig[sig_start : sig_start + 1196])) & 0xFFFFFFFF,
    )
    _expect_value_error(verify_sbv2_signed_blob, bytes(tampered_sig), digest_hex, "tampered sig")

    # Wrong key digest
    wrong_digest = ("0" if digest_hex[0] != "0" else "1") + digest_hex[1:]
    _expect_value_error(verify_sbv2_signed_blob, signed_boot, wrong_digest, "wrong digest")

    # Full merged image verification (pt @ 0x10000, app @ 0x20000)
    pt_off, app_off = 0x10000, 0x20000
    merged = bytearray(b"\xff") * (app_off + len(signed_app))
    merged[0x1000 : 0x1000 + len(signed_boot)] = signed_boot
    merged[pt_off : pt_off + len(valid_pt)] = valid_pt
    merged[app_off : app_off + len(signed_app)] = signed_app
    info = {
        "partition_table_offset": pt_off,
        **layout,
        "signed_bootloader_size": len(signed_boot),
        "signed_app_size": len(signed_app),
        "secure_boot_key_digest": digest_hex,
    }
    verify_burn_image_signatures(bytes(merged), info)
    info_without_sizes = {k: v for k, v in info.items() if k not in ("signed_bootloader_size", "signed_app_size")}
    verify_burn_image_signatures(bytes(merged), info_without_sizes)


if __name__ == "__main__":
    test_nvs_blob_layouts_and_write_ahead()
    test_wire_framing_and_ccm_aead()
    test_x25519_hkdf_derivation()
    test_payload_sizes()
    test_wifi_credential_validation()
    test_burn_helpers_and_sbv2_verification()
    print("ALL 6 HOST VERIFICATION SUITES (NVS, AEAD, X25519/HKDF, PAYLOADS, WIFI CREDS, BURN) PASSED.")
    sys.exit(0)
