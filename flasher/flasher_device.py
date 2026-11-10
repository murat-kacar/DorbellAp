"""Device operations of the flasher app: port detection, Flash, Burn, first-boot watch and Scan."""

from __future__ import annotations

import codecs
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
from collections.abc import Callable
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Any

import serial
import serial.tools.list_ports

from flasher_config import PROJECT_ROOT, strip_ansi
from flasher_env import IDF_VENV_PYTHON
from flasher_images import (
    PANEL_NVS_OFFSET,
    PANEL_NVS_SIZE,
    burn_efuse_problems,
    burn_layout_from_partition_table,
    make_nvs_keys_partition,
    parse_partition_table,
    write_panel_nvs_csv,
)
from flasher_widgets import DeviceCard

# First-boot log lines of a Burn device (bootloader_support / esp_app_format, ESP-IDF v6.1)
_BURN_FE_DONE_RE = re.compile(r"Flash encryption completed")

_BURN_APP_START_RE = re.compile(r"Project name:")

_BURN_ERROR_RE = re.compile(
    r"^E \(\d+\) (secure_boot\S*|flash_encrypt|boot|esp_image|doorbell_storage)\b|Guru Meditation|abort\(\) was called"
    r"|Brownout detector was triggered"
)

# Runs inside the ESP-IDF venv (esptool 5.x). Chip info is printed before the hard reset so a reset
# failure cannot discard it; every failure is reported as one PROBE_ERROR line instead of a traceback.
ESPTOOL_PROBE_SCRIPT = """
import sys
try:
    from esptool.cmds import detect_chip
    with detect_chip(port=sys.argv[1], connect_attempts=2) as esp:
        desc = esp.get_chip_description()
        mac = esp.read_mac("BASE_MAC")
        mac_s = ":".join(f"{b:02x}" for b in mac) if mac else "unknown"
        sb = "on" if esp.get_secure_boot_enabled() else "off"
        fe = "on" if esp.get_flash_encryption_enabled() else "off"
        print(f"PROBE_INFO|{desc}|{mac_s}|{sb}|{fe}", flush=True)
        try:
            esp.hard_reset()
        except Exception as exc:
            print(f"PROBE_WARN|hard reset failed: {exc}".replace(chr(10), " "), flush=True)
except Exception as exc:
    print(f"PROBE_ERROR|{type(exc).__name__}: {exc}".replace(chr(10), " "), flush=True)
    sys.exit(1)
"""

# esptool errors that mean "no ESP32 answered on this port" rather than a tooling problem
_PROBE_NO_CHIP_RE = re.compile(r"Failed to connect|No serial data received|Timed out", re.I)


class DeviceMixin:
    """Device methods of DoorbellFlasherApp (state and widgets live on the app)."""

    def detect_ports(self) -> None:
        if self._detecting:
            return
        if any(c._busy for c in self._device_cards):
            messagebox.showwarning(
                "Device Operation in Progress",
                "Cannot re-detect COM ports while a device is flashing or busy. Please wait for the operation to "
                "finish.",
            )
            return
        self._detecting = True
        self.update_detect_button_state()

        for card in self._device_cards:
            card._close_monitor_if_open()
        self._device_cards.clear()
        # Clear every child (old cards and any previous "no ports" label) before repopulating
        for child in self.cards_inner.winfo_children():
            child.destroy()

        ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
        if not ports:
            lbl = ttk.Label(
                self.cards_inner,
                text="No serial COM ports detected. Connect an ESP32 board via USB and click Detect.",
                foreground="#666666",
            )
            lbl.pack(anchor=tk.W, pady=8)
            self._detecting = False
            self.update_detect_button_state()
            self.log("[Detect] No COM ports found.\n")
            return

        for p in ports:
            desc = p.description or "Serial Port"
            # Strip trailing "(COMx)" if Windows driver already appended it
            desc = re.sub(r"\s*\(COM\d+\)\s*$", "", desc)
            card = DeviceCard(self.cards_inner, self, p.device, desc)
            card.pack(fill=tk.X, pady=(0, 10))
            # Lock the card until its own probe has released the COM port
            card.set_busy(True)
            self._device_cards.append(card)

        threading.Thread(target=self._worker_probe_ports, args=(list(self._device_cards),), daemon=True).start()

    def _worker_probe_ports(self, cards: list[DeviceCard]) -> None:
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

        for card in cards:
            port = card.port
            self.log(f"[{port}] Probing ESP32 via esptool...\n")
            try:
                proc = subprocess.run(
                    [python_exe, "-c", ESPTOOL_PROBE_SCRIPT, port],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=8,
                    creationflags=creationflags,
                )
                out = (proc.stdout or "") + (proc.stderr or "")
                probe_m = re.search(r"^PROBE_INFO\|([^|]+)\|([^|]+)\|([^|]+)\|([^\r\n]+)", out, re.M)
                err_m = re.search(r"^PROBE_ERROR\|([^\r\n]+)", out, re.M)
                warn_m = re.search(r"^PROBE_WARN\|([^\r\n]+)", out, re.M)

                if probe_m:
                    chip_str, mac_str, sb_str, fe_str = (g.strip() for g in probe_m.groups())
                    info_line = f"{chip_str} | MAC {mac_str.lower()} | secure boot {sb_str} | flash encryption {fe_str}"
                    self.log(f"[{port}] {info_line}\n")
                    if warn_m:
                        self.log(f"[{port}] Warning: {warn_m.group(1)}\n")
                    chip_text, status, color = info_line, "Detect done", "#1b7e2b"
                elif err_m and _PROBE_NO_CHIP_RE.search(err_m.group(1)):
                    self.log(f"[{port}] No ESP32 bootloader response: {err_m.group(1)}\n")
                    chip_text = "No ESP32 response (check USB cable or hold BOOT)"
                    status, color = "No response", "#b33a3a"
                else:
                    # Tooling failure (wrong venv, esptool API change, crash): show the real cause
                    reason = err_m.group(1) if err_m else f"probe exited with code {proc.returncode}"
                    tail = "\n".join(out.strip().splitlines()[-15:])
                    self.log(f"[{port}] Probe failed: {reason}\n" + (f"{tail}\n" if tail and not err_m else ""))
                    chip_text = f"Probe failed: {reason}"
                    status, color = "Probe error", "#b33a3a"
            except Exception as exc:
                self.log(f"[{port}] Probe error: {exc}\n")
                chip_text, status, color = f"Probe failed: {exc}", "Probe error", "#b33a3a"

            def _done(c: DeviceCard = card, t: str = chip_text, s: str = status, col: str = color) -> None:
                c.chip_info_var.set(t)
                c.set_status(s, col, 0.0)
                c.set_busy(False)

            self.post_ui(_done)

        def _finish() -> None:
            self._detecting = False
            self.update_detect_button_state()

        self.post_ui(_finish)

    def worker_flash_full(
        self, card: DeviceCard, fw_path: Path, panel_fields: tuple[str, str, str, str] | None
    ) -> None:
        """Production install: erase the whole chip, write bootloader + partition table + app and, for a
        panel, the NVS image with the card's codes and Wi-Fi (the factory values the hidden reset returns to)."""
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        self.log(
            f"\n[{port}] Flashing '{fw_path.name}' (full "
            f"erase{', firmware + codes/Wi-Fi' if panel_fields else ''})...\n"
        )
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp = Path(tmpdir)
                raw = fw_path.read_bytes()
                boot_file, app_file = tmp / "boot_and_pt.bin", tmp / "app.bin"
                boot_file.write_bytes(raw[0x1000:PANEL_NVS_OFFSET])  # bootloader + partition table @0x8000
                app_file.write_bytes(raw[0x10000:])
                images: list[tuple[int, Path]] = [(0x1000, boot_file), (0x10000, app_file)]

                if panel_fields is not None:
                    csv_path, nvs_file = tmp / "nvs.csv", tmp / "panel_nvs.bin"
                    write_panel_nvs_csv(csv_path, panel_fields)
                    rc = self._run_logged_cmd(
                        [
                            python_exe,
                            "-m",
                            "esp_idf_nvs_partition_gen",
                            "generate",
                            str(csv_path),
                            str(nvs_file),
                            hex(PANEL_NVS_SIZE),
                            "--outdir",
                            str(tmp),
                        ],
                        cwd=tmp,
                    )
                    if rc != 0 or not nvs_file.is_file():
                        raise RuntimeError("NVS image generation failed")
                    images.append((PANEL_NVS_OFFSET, nvs_file))

                cmd = [
                    python_exe,
                    "-m",
                    "esptool",
                    "--chip",
                    "esp32",
                    "-p",
                    port,
                    "-b",
                    "460800",
                    "--before",
                    "default-reset",
                    "--after",
                    "hard-reset",
                    "write-flash",
                    "--erase-all",
                    "--flash-mode",
                    "dio",
                    "--flash-size",
                    "2MB",
                    "--flash-freq",
                    "40m",
                ]
                for offset, path in images:
                    cmd += [hex(offset), str(path)]
                rc = self._run_logged_cmd(
                    cmd,
                    cwd=PROJECT_ROOT,
                    progress_cb=lambda pct: self.post_ui(
                        lambda p=pct: card.set_status(f"Flashing {int(p)}%...", "#b36b00", p)
                    ),
                    progress_weights=[path.stat().st_size for _o, path in images],
                )
            if rc == 0:
                if panel_fields is not None:
                    wifi = f"SSID='{panel_fields[2]}'" if panel_fields[2] else "SSID=(none)"
                    # Never log the admin / user code or the Wi-Fi passphrase (I-25)
                    self.log(f"[{port}] Flash complete: firmware + factory NVS ({wifi}, codes redacted per I-25).\n")
                else:
                    self.log(f"[{port}] Flash complete: firmware, blank NVS (module unpaired).\n")
                self.post_ui(lambda: card.set_status("Flash OK", "#1b7e2b", 100.0))
            else:
                self.log(f"[{port}] Flash FAILED (code {rc}).\n")
                self.post_ui(lambda: card.set_status("Flash failed", "#b33a3a", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Flash error: {exc}\n")
            self.post_ui(lambda: card.set_status("Flash error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))

    def worker_burn_device(
        self,
        card: DeviceCard,
        fw_path: Path,
        info: dict[str, Any],
        panel_fields: tuple[str, str, str, str] | None,
    ) -> None:
        """Irreversible Burn (ADR 0015): eFuse pre-check, one erase-all write of the signed image (+ encrypted panel
        NVS and its key partition), then a controlled first boot that burns the eFuses and encrypts the flash."""
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)

        def status(text: str, color: str = "#b36b00", pct: float | None = None) -> None:
            self.post_ui(lambda: card.set_status(text, color, pct))

        self.log(f"\n[{port}] ===== BURN '{fw_path.name}' (role {info.get('role')}) =====\n")
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp = Path(tmpdir)

                # 1. Layout from the image's own partition table; the sidecar only says where the table is
                raw = fw_path.read_bytes()
                pt_off = int(info["partition_table_offset"])
                layout = burn_layout_from_partition_table(parse_partition_table(raw[pt_off : pt_off + 0xC00]))
                app_off = layout["app_offset"]
                if raw[0x1000] != 0xE9 or len(raw) <= app_off or raw[app_off] != 0xE9:
                    raise ValueError("image does not contain a bootloader at 0x1000 and an app at the factory offset")
                for key in ("nvs_offset", "nvs_keys_offset", "app_offset"):
                    if int(info.get(key, -1)) != layout[key]:
                        raise ValueError(f"build info {key} does not match the image's partition table")
                boot_file, app_file = tmp / "boot_and_pt.bin", tmp / "app.bin"
                boot_file.write_bytes(raw[0x1000 : pt_off + 0x1000])
                app_file.write_bytes(raw[app_off:])
                images: list[tuple[int, Path]] = [(0x1000, boot_file), (app_off, app_file)]

                # 2. eFuse pre-check: refuse anything but a fresh v3.x chip with coding scheme NONE
                efuse_json = tmp / "efuse.json"
                rc = self._run_logged_cmd(
                    [
                        python_exe,
                        "-m",
                        "espefuse",
                        "--chip",
                        "esp32",
                        "-p",
                        port,
                        "--before",
                        "default-reset",
                        "summary",
                        "--format",
                        "json",
                        "--file",
                        str(efuse_json),
                    ],
                    cwd=PROJECT_ROOT,
                )
                if rc != 0 or not efuse_json.is_file():
                    raise RuntimeError("espefuse could not read the chip's eFuses")
                problems = burn_efuse_problems(json.loads(efuse_json.read_text(encoding="utf-8")))
                if problems:
                    for p in problems:
                        self.log(f"[{port}] Burn refused: {p}\n")
                    status("Burn refused (see log)", "#b33a3a", 0.0)
                    return
                self.log(f"[{port}] eFuse pre-check passed (fresh chip, rev >= v3.0, coding scheme NONE).\n")

                # 3. Panel: encrypted NVS with a host-generated key partition; verified by decrypting it again
                if panel_fields is not None:
                    status("Burn: preparing encrypted NVS...", "#b36b00", 5.0)
                    ssid = panel_fields[2]
                    nvs_gen = [python_exe, "-m", "esp_idf_nvs_partition_gen"]
                    keys_file, csv_path = tmp / "nvs_keys.bin", tmp / "nvs.csv"
                    enc_file, plain_file, dec_file = tmp / "nvs_enc.bin", tmp / "nvs_plain.bin", tmp / "nvs_dec.bin"
                    keys_file.write_bytes(make_nvs_keys_partition())
                    write_panel_nvs_csv(csv_path, panel_fields)
                    size = hex(layout["nvs_size"])
                    steps = [
                        nvs_gen
                        + [
                            "encrypt",
                            str(csv_path),
                            str(enc_file),
                            size,
                            "--inputkey",
                            str(keys_file),
                            "--outdir",
                            str(tmp),
                        ],
                        nvs_gen + ["generate", str(csv_path), str(plain_file), size, "--outdir", str(tmp)],
                        nvs_gen + ["decrypt", str(enc_file), str(keys_file), str(dec_file), "--outdir", str(tmp)],
                    ]
                    for cmd in steps:
                        if self._run_logged_cmd(cmd, cwd=tmp) != 0:
                            raise RuntimeError("NVS partition generation failed")
                    enc = enc_file.read_bytes()
                    if (
                        len(enc) != layout["nvs_size"]
                        or dec_file.read_bytes() != plain_file.read_bytes()
                        or enc == plain_file.read_bytes()
                    ):
                        raise RuntimeError(
                            "encrypted NVS self-check failed (decrypted image differs from the plaintext image)"
                        )
                    images += [(layout["nvs_offset"], enc_file), (layout["nvs_keys_offset"], keys_file)]
                    self.log(
                        f"[{port}] Encrypted NVS ready and verified ({'SSID=' + repr(ssid) if ssid else 'no Wi-Fi'}, "
                        "codes redacted per I-25).\n"
                    )

                # 4. One write: erase everything, write plaintext images; the chip encrypts them itself on first boot.
                # "keep" flash parameters: rewriting the signed bootloader header would break its signature.
                status("Burn: writing flash...", "#b36b00", 10.0)
                cmd = [
                    python_exe,
                    "-m",
                    "esptool",
                    "--chip",
                    "esp32",
                    "-p",
                    port,
                    "-b",
                    "460800",
                    "--before",
                    "default-reset",
                    "--after",
                    "no-reset",
                    "write-flash",
                    "--erase-all",
                    "--flash-mode",
                    "keep",
                    "--flash-freq",
                    "keep",
                    "--flash-size",
                    "keep",
                ]
                for offset, path in images:
                    cmd += [hex(offset), str(path)]
                rc = self._run_logged_cmd(
                    cmd,
                    cwd=PROJECT_ROOT,
                    progress_cb=lambda pct: status(f"Burn: writing {int(pct)}%...", "#b36b00", 10.0 + pct * 0.5),
                    progress_weights=[p.stat().st_size for _o, p in images],
                )
                if rc != 0:
                    self.log(
                        f"[{port}] Write FAILED (code {rc}). No eFuse has been burned yet; the chip is still open.\n"
                    )
                    status("Burn write failed", "#b33a3a", 0.0)
                    return
                self.log(f"[{port}] Write verified. Starting the first boot - DO NOT UNPLUG.\n")

            # 5. First boot: eFuses + in-place encryption, then the app starts on encrypted flash
            status("Burn: first boot - DO NOT UNPLUG", "#b33a3a", 65.0)
            result, errors = self._watch_burn_first_boot(port, status)
            if result == "ok":
                self.log(
                    f"[{port}] BURNED: flash encryption completed and the application started. The chip is locked.\n"
                )
                status("Burned and locked", "#1b7e2b", 100.0)
                self.post_ui(card.mark_burned)
            elif result == "errors":
                self.log(f"[{port}] Burn finished with errors: {' / '.join(errors)}\n")
                status("Burned - errors (see log)", "#b33a3a", 100.0)
            else:
                self.log(
                    f"[{port}] No completion seen within the time limit; keep the board powered and check it with "
                    "Monitor.\n"
                )
                status("Burn result unknown (see log)", "#b33a3a", 80.0)
        except Exception as exc:
            self.log(f"[{port}] Burn error: {exc}\n")
            status("Burn error (see log)", "#b33a3a", 0.0)
        finally:
            self.post_ui(lambda: card.set_busy(False))

    def _watch_burn_first_boot(
        self, port: str, status: Callable[..., None], timeout_s: float = 150.0
    ) -> tuple[str, list[str]]:
        """Reset into normal boot without toggling download mode and follow the first-boot log.

        Returns ("ok" | "errors" | "timeout", error lines). The port is opened with DTR/RTS released so opening it
        cannot reset the chip in the middle of the encryption pass.
        """
        errors: list[str] = []
        fe_done = app_started = False
        settle_until: float | None = None
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        try:
            ser.reset_input_buffer()
            ser.rts = True  # EN low (DTR released keeps GPIO0 high: normal boot)
            time.sleep(0.1)
            ser.rts = False
            deadline = time.monotonic() + timeout_s
            pending = ""
            decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
            while time.monotonic() < deadline:
                pending += decoder.decode(ser.read(512))
                *lines, pending = pending.split("\n")
                for line in lines:
                    line = strip_ansi(line).rstrip("\r")
                    if not line:
                        continue
                    self.log(f"[{port}] | {line}\n")
                    if "Encrypting partition" in line or "Generating new flash encryption key" in line:
                        status("Burn: encrypting - DO NOT UNPLUG", "#b33a3a", 75.0)
                    if _BURN_FE_DONE_RE.search(line):
                        fe_done = True
                        status("Burn: encryption done, rebooting...", "#b36b00", 90.0)
                    elif fe_done and _BURN_APP_START_RE.search(line):
                        if app_started:
                            # The app started again inside the settle window: a reboot loop, not a healthy boot
                            errors.append("application restarted during the first seconds (reboot loop)")
                            break
                        app_started = True
                        # Long enough to cover Wi-Fi / RF calibration start-up (brownout on a weak supply)
                        settle_until = time.monotonic() + 8.0
                    if _BURN_ERROR_RE.search(line):
                        errors.append(line.strip())
                if settle_until is not None and time.monotonic() >= settle_until:
                    break
        finally:
            ser.close()
        if app_started:
            return ("errors" if errors else "ok"), errors
        return ("errors" if errors else "timeout"), errors

    def worker_scan_device(self, card: DeviceCard) -> None:
        port = card.port
        self.log(f"\n[{port}] Querying live runtime status over UART (115200)...\n")
        try:
            with serial.Serial(port, baudrate=115200, timeout=0.15) as ser:
                ser.reset_input_buffer()
                ser.write(b"\r\ndoorbell_status\r\n")
                deadline = time.monotonic() + 1.5
                collected = bytearray()
                while time.monotonic() < deadline:
                    collected += ser.read(512)
                # Decode and strip ANSI once on the whole capture so split UTF-8 / escape sequences are handled
                full_out = strip_ansi(collected.decode("utf-8", errors="replace")).strip()
                if full_out:
                    self.log(f"[{port}] Status response:\n{full_out}\n")
                    self.post_ui(lambda: card.set_status("Scan OK (panel CLI)", "#1b7e2b", 100.0))
                else:
                    # Pulse RTS to read boot banner (useful for apartment module which has no UART REPL)
                    ser.dtr = False
                    ser.rts = True
                    time.sleep(0.1)
                    ser.rts = False
                    deadline = time.monotonic() + 2.2
                    while time.monotonic() < deadline:
                        collected += ser.read(512)
                    boot_out = strip_ansi(collected.decode("utf-8", errors="replace")).strip()
                    if boot_out:
                        self.log(f"[{port}] Boot log:\n{boot_out}\n")
                        self.post_ui(lambda: card.set_status("Scan OK (boot log)", "#1b7e2b", 100.0))
                    else:
                        self.log(f"[{port}] No serial output received on 115200 baud.\n")
                        self.post_ui(lambda: card.set_status("No UART output", "#b36b00", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Scan error: {exc}\n")
            self.post_ui(lambda: card.set_status("Scan error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))
