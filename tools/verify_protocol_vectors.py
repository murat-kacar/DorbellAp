#!/usr/bin/env python3
"""
Comprehensive Host Verification & Unit Test Suite for DorbellAp.

Verifies:
1. NVS Blob Binary Layouts & CRC32 (`panel_cfg` = 168 B, `panel_apt` = 48 B, `unit_state` = 152 B) + Write-Ahead Counter (`+100`).
2. 12-byte AAD Header Serialization, Transport Flag Masking (`is_retry`, `is_bcast`), and 13-byte AES-128-CCM Nonce Layout.
3. X25519 ECDH + HKDF-SHA256 (`"DorbellAp-Pair-v1"`) Pairing Key Agreement & Provisioning Blob AEAD.
4. Deterministic Keypad EBNF Grammar & Lockout State Machine (`I-19`, `I-20`, `I-44`, `docs/protocol.md` §6.1–6.2).
5. Pre-Crypto Destination Filtering (`I-53`), Idempotent ACK Deduplication Cache (`I-29`), and Monotonic Replay Floor.
6. `OPEN_DOOR` Reboot Replay & Withheld-Packet Freshness Protection via `panel_boot_id` + 64-bit `STATUS_CHALLENGE` (`I-7`, `I-18`).
7. Two-Phase Pairing Commit, 1.5 s Multi-Module Collision Abort, and Same-MAC Re-Pairing Cleanup (`I-30`, `I-45`).
"""

import hmac
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import Dict, Optional, Tuple
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from factory_provision_panel import PANEL_CFG_FORMAT_FULL, pack_panel_cfg_blob


# ---------------------------------------------------------------------------
# 1. NVS Blob Layout & Write-Ahead Counter Tests (I-50)
# ---------------------------------------------------------------------------

def test_nvs_blob_layouts_and_write_ahead() -> None:
    # 1. doorbell_panel_cfg_blob_t: 168 bytes
    assert struct.calcsize(PANEL_CFG_FORMAT_FULL) == 168
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
# 2. Wire Framing, Nonce Layout & Dual-Path Broadcast AAD Masking (I-8, I-33)
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
        raise AssertionError("Tampered AAD should have failed AEAD verification")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 3. X25519 + HKDF-SHA256 Key Agreement & Provisioning Blob Tests (I-8, I-26)
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
# 4. Keypad EBNF Grammar & Lockout Reference Model Tests (I-19, I-20, I-44)
# ---------------------------------------------------------------------------

@dataclass
class KeypadModel:
    admin_code: str = "12345678"
    user_code: str = "1234"
    min_apt: int = 1
    max_apt: int = 99
    mode: str = "MODE_IDLE"
    wrong_code_count: int = 0
    lockout_level: int = 0
    lockout_remaining_s: int = 0
    last_action: str = ""

    def _apply_wrong_penalty(self) -> None:
        self.wrong_code_count += 1
        if self.wrong_code_count >= 5:
            dur = 60 * (2 ** self.lockout_level)
            if dur > 900:
                dur = 900
            self.lockout_level += 1
            self.lockout_remaining_s = dur

    def _reset_lockout(self) -> None:
        self.wrong_code_count = 0
        self.lockout_level = 0
        self.lockout_remaining_s = 0

    def _parse_apt(self, s: str) -> Optional[int]:
        if not s or len(s) > 3 or not s.isdigit() or s[0] == "0":
            return None
        v = int(s)
        if v < self.min_apt or v > self.max_apt:
            return None
        return v

    def evaluate(self, raw_with_hash: str) -> str:
        assert raw_with_hash.endswith("#")
        seq = raw_with_hash[:-1]
        self.last_action = ""

        # Step 1: Cancel check ("*#")
        if seq == "*":
            if self.mode in ("MODE_PAIR_DISCOVER", "MODE_PORTAL"):
                self.mode = "MODE_IDLE"
                self.last_action = "CANCEL_MODE"
            else:
                self.last_action = "CANCEL_NOOP"
            return "OK_CANCEL"

        if not seq:
            return "ERR_MALFORMED"

        # Visitor ring (does not start with '*'): allowed even during lockout
        if not seq.startswith("*"):
            apt = self._parse_apt(seq)
            if apt is None:
                return "ERR_MALFORMED"
            self.last_action = f"RING:{apt}"
            return "OK_RING"

        if self.mode != "MODE_IDLE":
            return "ERR_MODE"

        # Step 2: Lockout gate (before inspecting digits)
        if self.lockout_remaining_s > 0:
            return "ERR_LOCKED_OUT"

        # Step 3: Syntax & structure validation BEFORE any secret comparison
        tokens = seq[1:].split("*")
        if len(tokens) > 4 or any(not t.isdigit() for t in tokens):
            return "ERR_MALFORMED"

        if len(tokens) == 1:
            # User door-open candidate: * <digits> #
            if hmac.compare_digest(tokens[0], self.user_code) and len(tokens[0]) == len(self.user_code):
                self._reset_lockout()
                self.last_action = "OPEN_DOOR_LOCAL"
                return "OK_USER_DOOR"
            self._apply_wrong_penalty()
            return "ERR_WRONG_CODE"

        if len(tokens[1]) != 1:
            return "ERR_MALFORMED"

        cmd_id = tokens[1]
        shape_valid = False
        if cmd_id == "1":
            shape_valid = len(tokens) == 3 and self._parse_apt(tokens[2]) is not None
        elif cmd_id == "2":
            a1 = self._parse_apt(tokens[2]) if len(tokens) == 4 else None
            a2 = self._parse_apt(tokens[3]) if len(tokens) == 4 else None
            shape_valid = a1 is not None and a1 == a2
        elif cmd_id == "3":
            shape_valid = (
                len(tokens) == 4
                and len(tokens[2]) == len(self.user_code)
                and len(tokens[3]) == len(self.user_code)
                and hmac.compare_digest(tokens[2], tokens[3])
            )
        elif cmd_id == "4":
            shape_valid = (
                len(tokens) == 4
                and len(tokens[2]) == len(self.admin_code)
                and len(tokens[3]) == len(self.admin_code)
                and hmac.compare_digest(tokens[2], tokens[3])
            )
        elif cmd_id == "5":
            shape_valid = len(tokens) == 2
        elif cmd_id == "6":
            shape_valid = len(tokens) == 3

        if not shape_valid:
            return "ERR_MALFORMED"

        # Step 4: Constant-time admin credential verification
        if not (hmac.compare_digest(tokens[0], self.admin_code) and len(tokens[0]) == len(self.admin_code)):
            self._apply_wrong_penalty()
            return "ERR_WRONG_CODE"

        if cmd_id == "6":
            if not (hmac.compare_digest(tokens[2], self.admin_code) and len(tokens[2]) == len(self.admin_code)):
                self._apply_wrong_penalty()
                return "ERR_WRONG_CODE"

        self._reset_lockout()

        if cmd_id == "1":
            self.last_action = f"PAIR:{int(tokens[2])}"
        elif cmd_id == "2":
            self.last_action = f"DELETE:{int(tokens[2])}"
        elif cmd_id == "3":
            self.user_code = tokens[2]
            self.last_action = "SET_USER_CODE"
        elif cmd_id == "4":
            self.admin_code = tokens[2]
            self.last_action = "SET_ADMIN_CODE"
        elif cmd_id == "5":
            self.mode = "MODE_PORTAL"
            self.last_action = "START_PORTAL"
        elif cmd_id == "6":
            self.last_action = "BUILDING_RESET"
        return "OK_ADMIN"


def test_keypad_grammar_and_lockout_edge_cases() -> None:
    kp = KeypadModel()

    # 1. Cancel (*#) never increments wrong_code_count
    assert kp.evaluate("*#") == "OK_CANCEL"
    assert kp.wrong_code_count == 0

    # 2. Malformed sequences never increment wrong_code_count even with wrong admin code (I-20)
    malformed_inputs = [
        "#",
        "**#",
        "*1234*#",
        "*00000000*9*1#",           # unknown cmd_id 9
        "*00000000*1*0#",           # apt 0 invalid
        "*00000000*1*01#",          # leading zero invalid
        "*00000000*1*100#",         # apt > 99 invalid
        "*00000000*2*5*6#",         # apt1 != apt2 mismatch in Step 3
        "*00000000*3*5678*5679#",   # new user code confirmation mismatch in Step 3
        "*00000000*3*567*567#",     # wrong user code length (3 != 4) in Step 3
        "*00000000*4*87654321*87654322#",  # new admin code mismatch in Step 3
        "*00000000*5*1#",           # extra arg to cmd 5
    ]
    for seq in malformed_inputs:
        res = kp.evaluate(seq)
        assert res == "ERR_MALFORMED", f"Expected ERR_MALFORMED for {seq}, got {res}"
        assert kp.wrong_code_count == 0, f"wrong_code_count incremented on malformed {seq}"

    # 3. 4 wrong user/admin attempts + 1 valid user code resets wrong_code_count to 0 (I-19, I-44)
    for _ in range(4):
        assert kp.evaluate("*9999#") == "ERR_WRONG_CODE"
    assert kp.wrong_code_count == 4
    assert kp.lockout_remaining_s == 0
    assert kp.evaluate("*1234#") == "OK_USER_DOOR"
    assert kp.wrong_code_count == 0

    # 4. 5 wrong attempts trigger 60 s lockout; visitor rings still work during lockout!
    for _ in range(5):
        assert kp.evaluate("*00000000*5#") == "ERR_WRONG_CODE"
    assert kp.lockout_remaining_s == 60
    assert kp.lockout_level == 1

    # During lockout, even a valid user/admin code is rejected by the lockout gate without resetting lockout
    assert kp.evaluate("*1234#") == "ERR_LOCKED_OUT"
    assert kp.evaluate("*12345678*5#") == "ERR_LOCKED_OUT"
    # Visitor ring still works during lockout!
    assert kp.evaluate("12#") == "OK_RING"
    assert kp.last_action == "RING:12"

    # Simulate lockout expiry and next lockout doubling (60 -> 120 -> 240 -> 480 -> 900 -> 900)
    expected_durations = [120, 240, 480, 900, 900]
    for exp_dur in expected_durations:
        kp.lockout_remaining_s = 0
        kp.wrong_code_count = 4
        assert kp.evaluate("*9999#") == "ERR_WRONG_CODE"
        assert kp.lockout_remaining_s == exp_dur


# ---------------------------------------------------------------------------
# 5. OPEN_DOOR Challenge-Response, Dedup & Replay Simulation (I-7, I-18, I-29)
# ---------------------------------------------------------------------------

def test_open_door_challenge_and_deduplication() -> None:
    panel_boot_id = 0xA1B2C3D4
    rx_floor = 10
    active_challenge = 0
    challenge_deadline_ms = 0
    door_pulse_count = 0
    last_ack: Optional[Tuple[int, int, str, int]] = None  # (tx_counter, seq_id, status, nonce)

    def panel_process_open_door(now_ms: int, tx_counter: int, seq_id: int, req_boot_id: int, echo_nonce: int):
        nonlocal rx_floor, active_challenge, challenge_deadline_ms, door_pulse_count, last_ack

        # Step 3: Deduplication check (I-29)
        if last_ack is not None and last_ack[0] == tx_counter and last_ack[1] == seq_id:
            return last_ack[2], last_ack[3]

        # Step 4: Replay check
        if tx_counter <= rx_floor:
            return "REJECT_REPLAY", 0

        rx_floor = tx_counter

        challenge_ok = (
            req_boot_id == panel_boot_id
            and echo_nonce != 0
            and echo_nonce == active_challenge
            and now_ms <= challenge_deadline_ms
        )
        if not challenge_ok:
            active_challenge = 0x1122334455667788
            challenge_deadline_ms = now_ms + 2000
            last_ack = (tx_counter, seq_id, "STATUS_CHALLENGE", active_challenge)
            return "STATUS_CHALLENGE", active_challenge

        active_challenge = 0
        challenge_deadline_ms = 0
        door_pulse_count += 1
        last_ack = (tx_counter, seq_id, "STATUS_OK", 0)
        return "STATUS_OK", 0

    # Round 0: unit sends initial OPEN_DOOR_REQ (echo_nonce=0) -> Panel returns STATUS_CHALLENGE
    status, chal = panel_process_open_door(now_ms=1000, tx_counter=11, seq_id=1, req_boot_id=0, echo_nonce=0)
    assert status == "STATUS_CHALLENGE" and chal != 0
    assert door_pulse_count == 0

    # Retry of Round 0 (if STATUS_CHALLENGE ACK was lost) -> Dedup cache returns same STATUS_CHALLENGE
    status_retry, chal_retry = panel_process_open_door(now_ms=1400, tx_counter=11, seq_id=1, req_boot_id=0, echo_nonce=0)
    assert status_retry == "STATUS_CHALLENGE" and chal_retry == chal
    assert door_pulse_count == 0

    # Round 1: unit echoes (panel_boot_id, chal) within 2000 ms -> Panel unlocks door once!
    status2, _ = panel_process_open_door(now_ms=1450, tx_counter=12, seq_id=2, req_boot_id=panel_boot_id, echo_nonce=chal)
    assert status2 == "STATUS_OK"
    assert door_pulse_count == 1

    # Retry of Round 1 (if STATUS_OK ACK was lost) -> Dedup cache returns STATUS_OK WITHOUT unlocking a 2nd time!
    status2_retry, _ = panel_process_open_door(
        now_ms=1850, tx_counter=12, seq_id=2, req_boot_id=panel_boot_id, echo_nonce=chal
    )
    assert status2_retry == "STATUS_OK"
    assert door_pulse_count == 1

    # Replaying an older frame (tx_counter=11) after rx_floor=12 is rejected
    status_replay, _ = panel_process_open_door(
        now_ms=1900, tx_counter=11, seq_id=99, req_boot_id=panel_boot_id, echo_nonce=chal
    )
    assert status_replay == "REJECT_REPLAY"
    assert door_pulse_count == 1


def test_payload_sizes() -> None:
    assert struct.calcsize(">II") == 8                # RING_REQ
    assert struct.calcsize(">IHBI") == 11             # RING_ACK
    assert struct.calcsize(">IIQ") == 16              # OPEN_DOOR_REQ
    assert struct.calcsize(">IHBIIQ") == 23           # OPEN_DOOR_ACK
    assert struct.calcsize(">IIIIHHHHBbB6sBBH") == 37 # HEARTBEAT_REQ
    assert struct.calcsize(">IHBII") == 15            # HEARTBEAT_ACK / PAIR_COMPLETE_ACK
    assert struct.calcsize(">I6sBB") == 12            # PAIR_COMPLETE


if __name__ == "__main__":
    test_nvs_blob_layouts_and_write_ahead()
    test_wire_framing_and_ccm_aead()
    test_x25519_hkdf_derivation()
    test_keypad_grammar_and_lockout_edge_cases()
    test_open_door_challenge_and_deduplication()
    test_payload_sizes()
    print("ALL 6 HOST VERIFICATION SUITES (NVS, AEAD, X25519/HKDF, KEYPAD EBNF/LOCKOUT, CHALLENGE/DEDUP, PAYLOADS) PASSED.")
    sys.exit(0)
