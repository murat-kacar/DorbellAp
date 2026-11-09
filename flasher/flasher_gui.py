#!/usr/bin/env python3
"""
DorbellAp — Customization, Build, Detection, and Flashing GUI.

Provides a two-column desktop interface:
- Left column ("Customization and build"):
  1. Profile selection (Development reversible vs Production disabled per ADR 0013)
  2. Role selection (Panel vs Apartment)
  3. Scrollable Kconfig parameter editor with hover tooltips, range checks, and GPIO conflict validation
  4. One-click incremental ESP-IDF build + esptool merge-bin outputting a single
     timestamped binary (`panel_YYYY-MM-DD_HH-MM-SS.bin` or `apartment_YYYY-MM-DD_HH-MM-SS.bin`)
     in `flasher/builds/`
  5. Live build and serial operation log
- Right column ("Detection and flashing"):
  - Auto-detection of connected USB-UART / ESP32 devices (chip revision, MAC, security eFuses)
  - Per-device cards with Role selector, Firmware selector, Browse button,
    Panel factory NVS provisioning fields (Admin code, User code, Random generators, optional Building Wi-Fi),
    and action buttons: Flash (NVS-preserving), Flash NVS, Monitor, Factory reset, Erase flash, Scan.
"""

from __future__ import annotations

import codecs
import csv
import datetime
import os
import queue
import re
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

# Automatically re-launch under ESP-IDF v6.1 Python venv if pyserial/esptool are missing in current interpreter
IDF_VENV_PYTHON = Path(r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe")
IDF_PATH_DEFAULT = Path(r"C:\esp\v6.1\esp-idf")
IDF_TOOLS_PATH_DEFAULT = Path(r"C:\Espressif\tools")

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    if IDF_VENV_PYTHON.is_file() and Path(sys.executable).resolve() != IDF_VENV_PYTHON.resolve():
        os.execv(str(IDF_VENV_PYTHON), [str(IDF_VENV_PYTHON), *sys.argv])
    raise

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# Project paths
FLASHER_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = FLASHER_DIR.parent
BUILDS_DIR = FLASHER_DIR / "builds"
PANEL_FW_DIR = PROJECT_ROOT / "firmware" / "panel"
UNIT_FW_DIR = PROJECT_ROOT / "firmware" / "unit"
KCONFIG_PROJBUILD_PATH = PROJECT_ROOT / "components" / "doorbell_common" / "Kconfig.projbuild"

# Import NVS packing helper from tools/factory_provision_panel.py
sys.path.insert(0, str(PROJECT_ROOT / "tools"))
from factory_provision_panel import pack_panel_cfg_blob  # noqa: E402

# Classic ESP32 GPIO rules (I-49)
ESP32_FLASH_GPIOS = {6, 7, 8, 9, 10, 11}
ESP32_INPUT_ONLY_GPIOS = {34, 35, 36, 37, 38, 39}
ESP32_VALID_GPIOS = set(range(0, 40)) - {20, 24, 28, 29, 30, 31} - ESP32_FLASH_GPIOS
ESP32_VALID_OUTPUT_GPIOS = ESP32_VALID_GPIOS - ESP32_INPUT_ONLY_GPIOS

# ANSI CSI escape sequences (colors, cursor moves, line erase) emitted by ESP_LOG and linenoise
_ANSI_CSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


# esptool 5.x write-flash progress line: "Writing at 0x00012345 [=====>    ]  45.2% ..."
_WRITE_PROGRESS_RE = re.compile(r"Writing at 0x[0-9a-fA-F]+\b.*?(\d+(?:\.\d+)?)\s*%")


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


def strip_ansi(text: str) -> str:
    return _ANSI_CSI_RE.sub("", text)


def escape_quoted(val: Any) -> str:
    """Escape backslashes and double quotes for a double-quoted Kconfig / esp_console argument."""
    return str(val).replace("\\", "\\\\").replace('"', '\\"')


def unescape_quoted(raw: str) -> str:
    """Inverse of escape_quoted for a value that may be wrapped in double quotes."""
    if len(raw) >= 2 and raw.startswith('"') and raw.endswith('"'):
        return re.sub(r"\\(.)", r"\1", raw[1:-1])
    return raw


@dataclass
class ParamSpec:
    """Metadata for a single Kconfig parameter exposed in the GUI.

    `default`, `min_val` and `max_val` are only fallbacks: at import time they are overwritten from
    Kconfig.projbuild (see _sync_params_from_kconfig), which is the single source of truth. Help texts
    therefore do not repeat ranges or defaults; the tooltip appends them from the synced values.
    """

    kconfig: str
    label: str
    section: str
    kind: str  # "int", "bool", "str"
    default: Any
    help_text: str
    min_val: Optional[int] = None
    max_val: Optional[int] = None
    max_len: Optional[int] = None
    is_gpio_output: bool = False
    is_gpio_input: bool = False


PANEL_PARAMS: List[ParamSpec] = [
    # GPIO
    ParamSpec(
        "DOORBELL_PANEL_GPIO_ROW0",
        "Keypad row 0",
        "GPIO",
        "int",
        13,
        "Output GPIO driving matrix keypad row 0.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_ROW1",
        "Keypad row 1",
        "GPIO",
        "int",
        14,
        "Output GPIO driving matrix keypad row 1.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_ROW2",
        "Keypad row 2",
        "GPIO",
        "int",
        27,
        "Output GPIO driving matrix keypad row 2.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_ROW3",
        "Keypad row 3",
        "GPIO",
        "int",
        26,
        "Output GPIO driving matrix keypad row 3.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_COL0",
        "Keypad column 0",
        "GPIO",
        "int",
        25,
        "Input GPIO reading matrix keypad column 0.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_COL1",
        "Keypad column 1",
        "GPIO",
        "int",
        33,
        "Input GPIO reading matrix keypad column 1.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_COL2",
        "Keypad column 2",
        "GPIO",
        "int",
        32,
        "Input GPIO reading matrix keypad column 2.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_DOOR_RELAY",
        "Door relay GPIO",
        "GPIO",
        "int",
        23,
        "Output GPIO driving the entrance door lock relay (boot-safe init per I-22).",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_DOOR_RELAY_ACTIVE_LOW",
        "Door relay active LOW",
        "GPIO",
        "bool",
        True,
        "Enable if the door relay module is energized by a LOW logic level.",
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_STATUS_LED",
        "Status LED GPIO",
        "GPIO",
        "int",
        2,
        "Output GPIO driving the panel status LED via LEDC PWM.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_LED_ACTIVE_HIGH",
        "Status LED active HIGH",
        "GPIO",
        "bool",
        True,
        "Enable if the status LED turns ON when the GPIO is driven HIGH.",
    ),
    ParamSpec(
        "DOORBELL_PANEL_GPIO_RESET_BTN",
        "Hidden reset button GPIO",
        "GPIO",
        "int",
        19,
        "Input GPIO for the hidden code-reset button inside the panel enclosure.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_PANEL_RESET_BTN_ACTIVE_LOW",
        "Reset button active LOW",
        "GPIO",
        "bool",
        True,
        "Enable if pressing the hidden reset button pulls the GPIO to GND.",
    ),
    ParamSpec(
        "DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL",
        "Reset button internal pull",
        "GPIO",
        "bool",
        True,
        "Enable internal pull-up/pull-down resistor on the reset button GPIO (not available on GPIO 34-39).",
    ),
    # Keypad & Door Relay
    ParamSpec(
        "DOORBELL_PANEL_KEY_MAP",
        "Keypad key map (12 chars)",
        "Keypad & Relay",
        "str",
        "123456789*0#",
        "12-character row-major key map for the 4x3 keypad.",
        max_len=12,
    ),
    ParamSpec(
        "DOORBELL_PANEL_INPUT_TIMEOUT_S",
        "Keypad input timeout (s)",
        "Keypad & Relay",
        "int",
        10,
        "Seconds of inactivity before an incomplete keypad entry is cleared.",
        3,
        60,
    ),
    ParamSpec(
        "DOORBELL_PANEL_DOOR_PULSE_MS",
        "Door relay pulse (ms)",
        "Keypad & Relay",
        "int",
        500,
        "Milliseconds the door relay remains energized on door open.",
        100,
        10000,
    ),
    # Apartments & Codes
    ParamSpec(
        "DOORBELL_PANEL_MIN_APT_NO",
        "Lowest apartment number",
        "Apartments & Codes",
        "int",
        1,
        "Lowest valid apartment number accepted on the keypad.",
        1,
        254,
    ),
    ParamSpec(
        "DOORBELL_PANEL_MAX_APT_NO",
        "Highest apartment number",
        "Apartments & Codes",
        "int",
        99,
        "Highest valid apartment number accepted on the keypad.",
        1,
        99,
    ),
    ParamSpec(
        "DOORBELL_PANEL_ADMIN_CODE_LEN",
        "Admin code length",
        "Apartments & Codes",
        "int",
        8,
        "Number of digits in the panel admin code (min 8 required for WPA3-SAE setup AP).",
        8,
        10,
    ),
    ParamSpec(
        "DOORBELL_PANEL_USER_CODE_LEN",
        "User code length",
        "Apartments & Codes",
        "int",
        4,
        "Number of digits in the shared building door-open user code.",
        4,
        8,
    ),
    # Lockout & Reset Button
    ParamSpec(
        "DOORBELL_PANEL_LOCKOUT_WRONG_CODES",
        "Wrong codes before lockout",
        "Lockout & Reset",
        "int",
        5,
        "Consecutive wrong user/admin code entries before keypad lockout activates.",
        1,
        20,
    ),
    ParamSpec(
        "DOORBELL_PANEL_LOCKOUT_FIRST_S",
        "First lockout duration (s)",
        "Lockout & Reset",
        "int",
        60,
        "Duration of the first keypad lockout in seconds.",
        10,
        3600,
    ),
    ParamSpec(
        "DOORBELL_PANEL_LOCKOUT_MULTIPLIER",
        "Lockout multiplier",
        "Lockout & Reset",
        "int",
        2,
        "Multiplier applied to lockout duration on each subsequent lockout level.",
        1,
        10,
    ),
    ParamSpec(
        "DOORBELL_PANEL_LOCKOUT_MAX_S",
        "Maximum lockout duration (s)",
        "Lockout & Reset",
        "int",
        900,
        "Maximum keypad lockout duration cap in seconds.",
        10,
        86400,
    ),
    ParamSpec(
        "DOORBELL_PANEL_RESET_MIN_HOLD_S",
        "Reset button min hold (s)",
        "Lockout & Reset",
        "int",
        10,
        "Minimum hold time in seconds for the hidden code-reset button.",
        3,
        30,
    ),
    ParamSpec(
        "DOORBELL_PANEL_RESET_MAX_HOLD_S",
        "Reset button max hold (s)",
        "Lockout & Reset",
        "int",
        60,
        "Maximum hold time in seconds for the hidden code-reset button.",
        15,
        65,
    ),
    # Network, Pairing & Portal
    ParamSpec(
        "DOORBELL_UDP_PORT",
        "UDP port",
        "Network & Pairing",
        "int",
        19500,
        "UDP port used for all operational and pairing messages.",
        1024,
        49151,
    ),
    ParamSpec(
        "DOORBELL_ACK_TIMEOUT_MS",
        "ACK timeout (ms)",
        "Network & Pairing",
        "int",
        400,
        "Milliseconds to wait for an application-layer UDP ACK before retrying.",
        100,
        5000,
    ),
    ParamSpec(
        "DOORBELL_PANEL_RING_RETRIES",
        "RING retries without ACK",
        "Network & Pairing",
        "int",
        2,
        "Number of dual-path (unicast + broadcast) retries when RING_ACK is not received.",
        0,
        5,
    ),
    ParamSpec(
        "DOORBELL_PAIRING_WINDOW_S",
        "Pairing discovery window (s)",
        "Network & Pairing",
        "int",
        20,
        "Seconds the panel keeps the temporary Doorbell-Pair-<MAC4> SoftAP open.",
        10,
        120,
    ),
    ParamSpec(
        "DOORBELL_PAIRING_COMPLETE_TIMEOUT_S",
        "Pairing LAN timeout (s)",
        "Network & Pairing",
        "int",
        10,
        "Seconds to wait for PAIR_COMPLETE on the building LAN after SoftAP handoff.",
        5,
        60,
    ),
    ParamSpec(
        "DOORBELL_PANEL_PORTAL_TIMEOUT_S",
        "Setup portal timeout (s)",
        "Network & Pairing",
        "int",
        120,
        "Seconds the Doorbell-Setup WPA3-SAE web portal stays open (*<admin>*5#).",
        30,
        600,
    ),
    ParamSpec(
        "DOORBELL_WIFI_SET_COUNTRY",
        "Set Wi-Fi country code",
        "Network & Pairing",
        "bool",
        False,
        "Apply explicit regulatory country code so channels 12-13 can be scanned actively.",
    ),
    ParamSpec(
        "DOORBELL_WIFI_COUNTRY_CODE",
        "Wi-Fi country code",
        "Network & Pairing",
        "str",
        "TR",
        "ISO 3166-1 alpha-2 country code (e.g., TR, DE, US).",
        max_len=2,
    ),
    ParamSpec(
        "DOORBELL_NVS_COUNTER_BLOCK",
        "NVS counter block size",
        "Network & Pairing",
        "int",
        100,
        "Write-ahead reservation block size for monotonic replay counters in NVS.",
        10,
        10000,
    ),
    # Heartbeat & Silence Restart
    ParamSpec(
        "DOORBELL_HEARTBEAT_ENABLED",
        "Periodic module heartbeat",
        "Heartbeat",
        "bool",
        True,
        "Enable periodic confirmable HEARTBEAT_REQ/ACK health monitoring.",
    ),
    ParamSpec(
        "DOORBELL_HEARTBEAT_INTERVAL_S",
        "Heartbeat interval (s)",
        "Heartbeat",
        "int",
        300,
        "Seconds between module heartbeats provisioned during pairing.",
        30,
        3600,
    ),
    ParamSpec(
        "DOORBELL_HEARTBEAT_RETRIES",
        "HEARTBEAT retries",
        "Heartbeat",
        "int",
        1,
        "Number of retries when HEARTBEAT_ACK is not received.",
        0,
        5,
    ),
    ParamSpec(
        "DOORBELL_PANEL_SILENCE_RESTART",
        "Silence auto-restart",
        "Heartbeat",
        "bool",
        True,
        "Restart panel with exponential RTC backoff (15m -> 48h) if no registered module reports for 3 intervals.",
    ),
    # Status LED
    ParamSpec(
        "DOORBELL_LED_FULL_BRIGHTNESS_PCT",
        "Full brightness (%)",
        "Status LED",
        "int",
        100,
        "Full brightness level.",
        10,
        100,
    ),
    ParamSpec(
        "DOORBELL_LED_DIM_BRIGHTNESS_PCT",
        "Dim brightness (%)",
        "Status LED",
        "int",
        10,
        "Perceived dim brightness percentage (must be < Full brightness).",
        1,
        50,
    ),
    ParamSpec(
        "DOORBELL_LED_SHORT_FLASH_MS",
        "Short flash (ms)",
        "Status LED",
        "int",
        50,
        "Key-press short flash duration in ms.",
        10,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_FAST_PERIOD_MS",
        "Fast blink period (ms)",
        "Status LED",
        "int",
        200,
        "Full period of fast blink (error / visual bell).",
        40,
        1000,
    ),
    ParamSpec(
        "DOORBELL_LED_MEDIUM_PERIOD_MS",
        "Medium blink period (ms)",
        "Status LED",
        "int",
        500,
        "Full period of medium blink (no-response / button hold).",
        100,
        2000,
    ),
    ParamSpec(
        "DOORBELL_LED_SLOW_PERIOD_MS",
        "Slow blink period (ms)",
        "Status LED",
        "int",
        1000,
        "Full period of 1 Hz slow blink (typing / portal / pairing search).",
        200,
        4000,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_PULSE_MS",
        "Multi-flash pulse (ms)",
        "Status LED",
        "int",
        100,
        "ON duration of each pulse in double/triple/sparse flashes.",
        20,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_GAP_MS",
        "Multi-flash gap (ms)",
        "Status LED",
        "int",
        100,
        "OFF gap between pulses in double/triple flash patterns.",
        20,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_PERIOD_MS",
        "Multi-flash period (ms)",
        "Status LED",
        "int",
        1000,
        "Total repeat period for double/triple flash patterns.",
        500,
        5000,
    ),
    ParamSpec(
        "DOORBELL_LED_SPARSE_PERIOD_MS",
        "Sparse flash period (ms)",
        "Status LED",
        "int",
        3000,
        "Total repeat period for sparse single flash (lockout / network lost).",
        1000,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_OK_DURATION_MS",
        "OK solid duration (ms)",
        "Status LED",
        "int",
        2000,
        "Duration of solid ON confirmation after a successful operation.",
        200,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_ERR_DURATION_MS",
        "Error duration (ms)",
        "Status LED",
        "int",
        1000,
        "Duration of fast-blink error indication.",
        200,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_NO_RESP_DURATION_MS",
        "No-response duration (ms)",
        "Status LED",
        "int",
        3000,
        "Duration of medium-blink no-response indication.",
        500,
        10000,
    ),
]

APARTMENT_PARAMS: List[ParamSpec] = [
    # GPIO
    ParamSpec(
        "DOORBELL_UNIT_GPIO_BELL_RELAY",
        "Bell relay GPIO",
        "GPIO",
        "int",
        4,
        "Output GPIO driving the apartment bell relay (boot-safe init per I-22).",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_UNIT_BELL_RELAY_ACTIVE_HIGH",
        "Bell relay active HIGH",
        "GPIO",
        "bool",
        True,
        "Enable if the bell relay is energized when the GPIO is driven HIGH.",
    ),
    ParamSpec(
        "DOORBELL_UNIT_GPIO_DOOR_BTN",
        "Door-open button GPIO",
        "GPIO",
        "int",
        23,
        "Input GPIO for the indoor door-open push button.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_UNIT_GPIO_PAIR_BTN",
        "Hidden pairing button GPIO",
        "GPIO",
        "int",
        18,
        "Input GPIO for the hidden pairing button.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_UNIT_GPIO_RESET_BTN",
        "Hidden reset button GPIO",
        "GPIO",
        "int",
        19,
        "Input GPIO for the hidden reset button.",
        0,
        39,
        is_gpio_input=True,
    ),
    ParamSpec(
        "DOORBELL_UNIT_BTNS_ACTIVE_LOW",
        "Buttons active LOW",
        "GPIO",
        "bool",
        True,
        "Enable if pressing a button connects the GPIO pin to GND.",
    ),
    ParamSpec(
        "DOORBELL_UNIT_BTNS_INTERNAL_PULL",
        "Buttons internal pull",
        "GPIO",
        "bool",
        True,
        "Enable internal pull-up/pull-down resistors on button GPIOs (not available on GPIO 34-39).",
    ),
    ParamSpec(
        "DOORBELL_UNIT_GPIO_STATUS_LED",
        "Status LED GPIO",
        "GPIO",
        "int",
        2,
        "Output GPIO driving the module status LED via LEDC PWM.",
        0,
        33,
        is_gpio_output=True,
    ),
    ParamSpec(
        "DOORBELL_LED_ACTIVE_HIGH",
        "Status LED active HIGH",
        "GPIO",
        "bool",
        True,
        "Enable if the status LED turns ON when the GPIO is driven HIGH.",
    ),
    # Bell & Buttons
    ParamSpec(
        "DOORBELL_UNIT_BELL_PULSE_MS",
        "Bell relay pulse (ms)",
        "Bell & Buttons",
        "int",
        500,
        "Milliseconds the bell relay remains energized when rung.",
        100,
        10000,
    ),
    ParamSpec(
        "DOORBELL_UNIT_VISUAL_BELL",
        "Visual bell LED blink",
        "Bell & Buttons",
        "bool",
        True,
        "Blink the status LED rapidly at highest priority while the bell relay is active.",
    ),
    ParamSpec(
        "DOORBELL_UNIT_DOOR_BTN_MIN_MS",
        "Door button min press (ms)",
        "Bell & Buttons",
        "int",
        100,
        "Minimum valid press duration for the door-open button.",
        20,
        2000,
    ),
    ParamSpec(
        "DOORBELL_UNIT_DOOR_BTN_MAX_S",
        "Door button max press (s)",
        "Bell & Buttons",
        "int",
        30,
        "Maximum valid press duration for the door-open button.",
        5,
        120,
    ),
    ParamSpec(
        "DOORBELL_UNIT_HOLD_BTN_MIN_S",
        "Pair/reset min hold (s)",
        "Bell & Buttons",
        "int",
        7,
        "Start of the valid hold window for hidden pairing and reset buttons.",
        3,
        20,
    ),
    ParamSpec(
        "DOORBELL_UNIT_HOLD_BTN_MAX_S",
        "Pair/reset max hold (s)",
        "Bell & Buttons",
        "int",
        12,
        "End of the valid hold window for hidden pairing and reset buttons.",
        5,
        30,
    ),
    ParamSpec(
        "DOORBELL_UNIT_HOLD_BTN_CANCEL_S",
        "Pair/reset cancel after (s)",
        "Bell & Buttons",
        "int",
        15,
        "Hold duration at which the action is cancelled with a triple flash.",
        8,
        65,
    ),
    # Network & Heartbeat
    ParamSpec(
        "DOORBELL_UDP_PORT",
        "UDP port",
        "Network & Heartbeat",
        "int",
        19500,
        "UDP port used for all operational and pairing messages.",
        1024,
        49151,
    ),
    ParamSpec(
        "DOORBELL_ACK_TIMEOUT_MS",
        "ACK timeout (ms)",
        "Network & Heartbeat",
        "int",
        400,
        "Milliseconds to wait for an application-layer UDP ACK before retrying.",
        100,
        5000,
    ),
    ParamSpec(
        "DOORBELL_UNIT_OPEN_DOOR_RETRIES",
        "OPEN_DOOR retries",
        "Network & Heartbeat",
        "int",
        1,
        "Number of dual-path retries when OPEN_DOOR_ACK is not received.",
        0,
        5,
    ),
    ParamSpec(
        "DOORBELL_PAIRING_COMPLETE_TIMEOUT_S",
        "Pairing LAN timeout (s)",
        "Network & Heartbeat",
        "int",
        10,
        "Seconds allowed to associate to the building AP and complete PAIR_COMPLETE.",
        5,
        60,
    ),
    ParamSpec(
        "DOORBELL_WIFI_SET_COUNTRY",
        "Set Wi-Fi country code",
        "Network & Heartbeat",
        "bool",
        False,
        "Apply explicit regulatory country code so channels 12-13 can be scanned actively.",
    ),
    ParamSpec(
        "DOORBELL_WIFI_COUNTRY_CODE",
        "Wi-Fi country code",
        "Network & Heartbeat",
        "str",
        "TR",
        "ISO 3166-1 alpha-2 country code (e.g., TR, DE, US).",
        max_len=2,
    ),
    ParamSpec(
        "DOORBELL_NVS_COUNTER_BLOCK",
        "NVS counter block size",
        "Network & Heartbeat",
        "int",
        100,
        "Write-ahead reservation block size for monotonic replay counters in NVS.",
        10,
        10000,
    ),
    ParamSpec(
        "DOORBELL_HEARTBEAT_ENABLED",
        "Periodic module heartbeat",
        "Network & Heartbeat",
        "bool",
        True,
        "Enable periodic confirmable HEARTBEAT_REQ/ACK health monitoring.",
    ),
    ParamSpec(
        "DOORBELL_HEARTBEAT_INTERVAL_S",
        "Default heartbeat interval (s)",
        "Network & Heartbeat",
        "int",
        300,
        "Fallback heartbeat interval in seconds if not overridden during pairing.",
        30,
        3600,
    ),
    ParamSpec(
        "DOORBELL_HEARTBEAT_RETRIES",
        "HEARTBEAT retries",
        "Network & Heartbeat",
        "int",
        1,
        "Number of retries when HEARTBEAT_ACK is not received.",
        0,
        5,
    ),
    # Status LED
    ParamSpec(
        "DOORBELL_LED_FULL_BRIGHTNESS_PCT",
        "Full brightness (%)",
        "Status LED",
        "int",
        100,
        "Full brightness level.",
        10,
        100,
    ),
    ParamSpec(
        "DOORBELL_LED_DIM_BRIGHTNESS_PCT",
        "Dim brightness (%)",
        "Status LED",
        "int",
        10,
        "Perceived dim brightness percentage in coverage (must be < Full brightness).",
        1,
        50,
    ),
    ParamSpec(
        "DOORBELL_LED_SHORT_FLASH_MS",
        "Short flash (ms)",
        "Status LED",
        "int",
        50,
        "Short flash duration in ms.",
        10,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_FAST_PERIOD_MS",
        "Fast blink period (ms)",
        "Status LED",
        "int",
        200,
        "Full period of fast blink (visual bell).",
        40,
        1000,
    ),
    ParamSpec(
        "DOORBELL_LED_MEDIUM_PERIOD_MS",
        "Medium blink period (ms)",
        "Status LED",
        "int",
        500,
        "Full period of medium blink (no-response / pairing button hold).",
        100,
        2000,
    ),
    ParamSpec(
        "DOORBELL_LED_SLOW_PERIOD_MS",
        "Slow blink period (ms)",
        "Status LED",
        "int",
        1000,
        "Full period of 1 Hz slow blink (pairing search).",
        200,
        4000,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_PULSE_MS",
        "Multi-flash pulse (ms)",
        "Status LED",
        "int",
        100,
        "ON duration of each pulse in double/triple/sparse flashes.",
        20,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_GAP_MS",
        "Multi-flash gap (ms)",
        "Status LED",
        "int",
        100,
        "OFF gap between pulses in double/triple flash patterns.",
        20,
        500,
    ),
    ParamSpec(
        "DOORBELL_LED_MULTI_PERIOD_MS",
        "Multi-flash period (ms)",
        "Status LED",
        "int",
        1000,
        "Total repeat period for double/triple flash patterns.",
        500,
        5000,
    ),
    ParamSpec(
        "DOORBELL_LED_SPARSE_PERIOD_MS",
        "Sparse flash period (ms)",
        "Status LED",
        "int",
        3000,
        "Total repeat period for sparse single flash (out of coverage).",
        1000,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_OK_DURATION_MS",
        "OK solid duration (ms)",
        "Status LED",
        "int",
        2000,
        "Duration of solid ON confirmation after door open or pairing success.",
        200,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_ERR_DURATION_MS",
        "Error duration (ms)",
        "Status LED",
        "int",
        1000,
        "Duration of fast-blink error indication.",
        200,
        10000,
    ),
    ParamSpec(
        "DOORBELL_LED_NO_RESP_DURATION_MS",
        "No-response duration (ms)",
        "Status LED",
        "int",
        3000,
        "Duration of medium-blink no-response indication.",
        500,
        10000,
    ),
]


def _kconfig_cond(cond: Optional[str], bool_values: Dict[str, bool]) -> Optional[bool]:
    """Evaluate a Kconfig `if` condition limited to `SYM` / `!SYM`; None when it cannot be evaluated."""
    if cond is None:
        return True
    cond = cond.strip()
    negate = cond.startswith("!")
    sym = cond[1:].strip() if negate else cond
    if not re.fullmatch(r"[A-Z0-9_]+", sym) or sym not in bool_values:
        return None
    return bool_values[sym] != negate


def _kconfig_pick(items: List[Tuple[Any, Optional[str]]], bool_values: Dict[str, bool]) -> Tuple[bool, Any]:
    """Return (resolved, value): Kconfig uses the first entry whose condition holds."""
    for value, cond in items:
        result = _kconfig_cond(cond, bool_values)
        if result is None:
            return False, None
        if result:
            return True, value
    return True, None


def _parse_kconfig_default(val_s: str, ktype: Optional[str]) -> Any:
    if ktype == "bool":
        return {"y": True, "n": False}.get(val_s)
    if ktype == "int":
        try:
            return int(val_s, 0)
        except ValueError:
            return None
    if ktype == "string":
        return unescape_quoted(val_s)
    return None


def _sync_params_from_kconfig(kconfig_path: Path, param_lists: List[List[ParamSpec]]) -> List[str]:
    """Overwrite default/min_val/max_val of every ParamSpec from Kconfig.projbuild.

    Returns warnings for specs that could not be synced, so the hardcoded fallbacks never drift silently.
    """
    if not kconfig_path.is_file():
        return [f"{kconfig_path} not found; using built-in fallback defaults and ranges."]
    try:
        text = kconfig_path.read_text(encoding="utf-8", errors="ignore")
    except OSError as exc:
        return [f"Cannot read {kconfig_path}: {exc}; using built-in fallback defaults and ranges."]

    parsed: Dict[str, Dict[str, Any]] = {}
    cur_entry: Optional[Dict[str, Any]] = None
    in_help = False
    help_indent = 0

    for raw_line in text.splitlines():
        expanded = raw_line.expandtabs(4)
        stripped = expanded.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(expanded) - len(expanded.lstrip(" "))
        if in_help:
            if indent >= help_indent:
                continue
            in_help = False

        m_cfg = re.match(r"^(?:menu)?config\s+([A-Z0-9_]+)\b", stripped)
        if m_cfg:
            cur_entry = {"type": None, "defaults": [], "ranges": []}
            parsed[m_cfg.group(1)] = cur_entry
            continue

        if stripped.startswith(("menu ", "endmenu", "choice", "endchoice", "if ", "endif", "source ", "comment ")):
            cur_entry = None
            continue

        if cur_entry is None:
            continue

        m_type = re.match(r"^(int|bool|string|hex)\b", stripped)
        if m_type:
            cur_entry["type"] = m_type.group(1)
            continue

        m_range = re.match(r"^range\s+(-?\w+)\s+(-?\w+)(?:\s+if\s+(.+))?$", stripped)
        if m_range:
            try:
                bounds = (int(m_range.group(1), 0), int(m_range.group(2), 0))
            except ValueError:
                bounds = None  # symbolic bound: unresolvable, recorded so the pick fails safely
            cur_entry["ranges"].append((bounds, m_range.group(3)))
            continue

        m_def = re.match(r"^default\s+(\".*\"|\S+)(?:\s+if\s+(.+))?$", stripped)
        if m_def:
            cur_entry["defaults"].append((m_def.group(1), m_def.group(2)))
            continue

        if stripped == "help" or stripped == "---help---":
            in_help = True
            help_indent = indent + 1

    # Bool symbols with an unconditional first default, used to evaluate `if SYM` conditions.
    bool_values: Dict[str, bool] = {}
    for name, meta in parsed.items():
        if meta["type"] == "bool" and meta["defaults"] and meta["defaults"][0][1] is None:
            val = _parse_kconfig_default(meta["defaults"][0][0], "bool")
            if val is not None:
                bool_values[name] = val

    warnings: List[str] = []
    for plist in param_lists:
        for spec in plist:
            meta = parsed.get(spec.kconfig)
            if not meta:
                warnings.append(f"{spec.kconfig} is not defined in Kconfig.projbuild; using built-in fallback.")
                continue
            ok, raw_default = _kconfig_pick(meta["defaults"], bool_values)
            value = _parse_kconfig_default(raw_default, meta["type"]) if ok and raw_default is not None else None
            if ok and raw_default is None and meta["type"] == "bool":
                value = False  # Kconfig: a bool without an applicable default is n
            if value is not None:
                spec.default = value
            else:
                warnings.append(f"{spec.kconfig}: Kconfig default not resolvable; using built-in fallback.")
            if spec.kind == "int":
                ok, bounds = _kconfig_pick(meta["ranges"], bool_values)
                if ok and bounds is not None:
                    spec.min_val, spec.max_val = bounds
                elif meta["ranges"]:
                    warnings.append(f"{spec.kconfig}: Kconfig range not resolvable; using built-in fallback.")
    return warnings


KCONFIG_SYNC_WARNINGS = _sync_params_from_kconfig(KCONFIG_PROJBUILD_PATH, [PANEL_PARAMS, APARTMENT_PARAMS])


class ToolTip:
    """Lightweight hover tooltip for parameter labels."""

    def __init__(self, widget: tk.Widget, text: str) -> None:
        self.widget = widget
        self.text = text
        self.tip_win: Optional[tk.Toplevel] = None
        widget.bind("<Enter>", self._show)
        widget.bind("<Leave>", self._hide)

    def _show(self, _event: Optional[tk.Event] = None) -> None:
        if self.tip_win or not self.text:
            return
        x = self.widget.winfo_rootx() + 18
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.tip_win = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        lbl = tk.Label(
            tw,
            text=self.text,
            justify=tk.LEFT,
            background="#ffffe0",
            foreground="#111111",
            relief=tk.SOLID,
            borderwidth=1,
            font=("Segoe UI", 9),
            wraplength=340,
            padx=6,
            pady=4,
        )
        lbl.pack()

    def _hide(self, _event: Optional[tk.Event] = None) -> None:
        if self.tip_win:
            self.tip_win.destroy()
            self.tip_win = None


def generate_random_digits(length: int) -> str:
    rng = secrets.SystemRandom()
    return "".join(str(rng.randrange(0, 10)) for _ in range(length))


def inspect_firmware_binary(bin_path: Path) -> Tuple[str, str]:
    """
    Inspect a firmware `.bin` file to determine:
    1) Image layout: "merged" (0x0-based merge-bin with bootloader@0x1000, part@0x8000, app@0x10000)
       or "app_only" (raw app image starting at 0x0).
    2) Project role: "panel", "apartment", or "unknown" from esp_app_desc_t.
    """
    try:
        data = bin_path.read_bytes()
    except OSError:
        return "unknown", "unknown"

    app_offset = 0
    layout = "app_only"
    if (
        len(data) > 0x10100
        and data[0x1000] == 0xE9
        and data[0x8000:0x8002] == b"\xaa\x50"
        and data[0x10000] == 0xE9
    ):
        layout = "merged"
        app_offset = 0x10000
    elif len(data) > 0x100 and data[0] == 0xE9:
        layout = "app_only"
        app_offset = 0
    else:
        return "unknown", "unknown"

    # esp_app_desc_t starts at app_offset + 0x20; magic is 0xABCD5432 (little-endian)
    desc_off = app_offset + 0x20
    if len(data) >= desc_off + 0x50:
        (magic,) = struct.unpack_from("<I", data, desc_off)
        if magic == 0xABCD5432:
            proj_raw = data[desc_off + 0x30 : desc_off + 0x50].split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
            if "panel" in proj_raw:
                return layout, "panel"
            if "unit" in proj_raw or "apartment" in proj_raw:
                return layout, "apartment"

    name_lower = bin_path.name.lower()
    if name_lower.startswith("panel"):
        return layout, "panel"
    if name_lower.startswith("apartment") or name_lower.startswith("unit"):
        return layout, "apartment"
    return layout, "unknown"


class SerialMonitorWindow(tk.Toplevel):
    """Live UART Serial Monitor & Authenticated Console Window for a COM port."""

    def __init__(self, parent: tk.Widget, port: str, log_cb: Callable[[str], None]) -> None:
        super().__init__(parent)
        self.port = port
        self.log_cb = log_cb
        self.title(f"Serial Monitor — {port} (115200 8N1)")
        self.geometry("760x500")
        self.minsize(620, 380)

        self._stop_evt = threading.Event()
        self._ser: Optional[serial.Serial] = None
        self._rx_queue: queue.Queue[str] = queue.Queue()
        self._redact_lock = threading.Lock()
        self._redact_tokens: List[str] = []
        # While a doorbell_wifi command is in flight, the REPL echo (possibly redrawn char-by-char and
        # horizontally scrolled by linenoise) can carry fragments of secrets, so only known-safe lines pass.
        self._echo_mute_until = 0.0

        top_bar = ttk.Frame(self, padding=(8, 6))
        top_bar.pack(fill=tk.X)

        ttk.Button(top_bar, text="Status (doorbell_status)", command=self._send_status).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(top_bar, text="Reboot (RTS Reset)", command=self._reset_board).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(top_bar, text="Clear", command=self._clear).pack(side=tk.LEFT, padx=(0, 6))

        wifi_bar = ttk.LabelFrame(self, text="Authenticated Panel Wi-Fi Update (doorbell_wifi — redacted from logs)", padding=(8, 4))
        wifi_bar.pack(fill=tk.X, padx=8, pady=(0, 6))

        ttk.Label(wifi_bar, text="Admin code:").pack(side=tk.LEFT)
        self.admin_var = tk.StringVar()
        ttk.Entry(wifi_bar, textvariable=self.admin_var, width=11, show="*").pack(side=tk.LEFT, padx=(4, 10))

        ttk.Label(wifi_bar, text="SSID:").pack(side=tk.LEFT)
        self.ssid_var = tk.StringVar()
        ttk.Entry(wifi_bar, textvariable=self.ssid_var, width=18).pack(side=tk.LEFT, padx=(4, 10))

        ttk.Label(wifi_bar, text="Password:").pack(side=tk.LEFT)
        self.pass_var = tk.StringVar()
        ttk.Entry(wifi_bar, textvariable=self.pass_var, width=16, show="*").pack(side=tk.LEFT, padx=(4, 10))

        ttk.Button(wifi_bar, text="Send Wi-Fi Update", command=self._send_wifi).pack(side=tk.LEFT)

        txt_frame = ttk.Frame(self, padding=(8, 0, 8, 8))
        txt_frame.pack(fill=tk.BOTH, expand=True)

        self.text = tk.Text(txt_frame, wrap=tk.WORD, font=("Consolas", 9), bg="#1e1e1e", fg="#d4d4d4")
        sb = ttk.Scrollbar(txt_frame, orient=tk.VERTICAL, command=self.text.yview)
        self.text.configure(yscrollcommand=sb.set)
        self.text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

        self.protocol("WM_DELETE_WINDOW", self.close)
        threading.Thread(target=self._reader_thread, daemon=True).start()
        self.after(50, self._poll_rx)

    def _append(self, msg: str) -> None:
        self.text.insert(tk.END, msg)
        self.text.see(tk.END)

    def _clear(self) -> None:
        self.text.delete("1.0", tk.END)

    def _send_status(self) -> None:
        if self._ser and self._ser.is_open:
            try:
                self._ser.write(b"\r\ndoorbell_status\r\n")
            except serial.SerialException as exc:
                self._append(f"\n[Error sending command: {exc}]\n")

    # Panel console output that can never contain doorbell_wifi arguments (panel_console.c printf/ESP_LOG)
    _ECHO_SAFE_LINE_RE = re.compile(r"^\s*(?:panel> )?(?:Error:|Usage:|Wi-Fi credentials saved|[IWE] \(\d+\) )")
    _ECHO_MUTE_S = 3.0

    def _is_echo_muted(self) -> bool:
        with self._redact_lock:
            return time.monotonic() < self._echo_mute_until

    def _scrub_rx_text(self, text: str, echo_muted: bool) -> str:
        """Redact secrets from received console text (I-25). `text` must already be ANSI-stripped."""
        if echo_muted:
            kept: List[str] = []
            suppressed = False
            for line in text.splitlines(keepends=True):
                if self._ECHO_SAFE_LINE_RE.match(line):
                    kept.append(line)
                elif line.strip():
                    suppressed = True
            if suppressed:
                kept.insert(0, "[console echo suppressed]\n")
            text = "".join(kept)
        text = re.sub(
            r"doorbell_wifi\s+[^\r\n]*",
            "doorbell_wifi [credentials redacted]",
            text,
        )
        with self._redact_lock:
            tokens = list(self._redact_tokens)
        for tok in tokens:
            if tok and tok in text:
                text = text.replace(tok, "[REDACTED]")
        return text

    def _send_wifi(self) -> None:
        admin = self.admin_var.get().strip()
        ssid = self.ssid_var.get().strip()
        pw = self.pass_var.get()
        if not admin.isdigit() or not (8 <= len(admin) <= 10):
            messagebox.showerror("Invalid Admin Code", "Admin code must be 8-10 digits.", parent=self)
            return
        if any(ch in ssid or ch in pw for ch in ("\x00", "\r", "\n")):
            messagebox.showerror("Invalid Wi-Fi", "SSID and password cannot contain control characters.", parent=self)
            return
        ssid_bytes = len(ssid.encode("utf-8"))
        pw_bytes = len(pw.encode("utf-8"))
        if ssid_bytes == 0 or ssid_bytes > 32 or (pw and (pw_bytes < 8 or pw_bytes > 63)):
            messagebox.showerror(
                "Invalid Wi-Fi",
                "SSID must be 1-32 UTF-8 bytes; password must be empty or 8-63 UTF-8 bytes.",
                parent=self,
            )
            return
        if self._ser and self._ser.is_open:
            try:
                esc_ssid = escape_quoted(ssid)
                esc_pw = escape_quoted(pw)
                with self._redact_lock:
                    for tok in (admin, pw, esc_pw):
                        if tok and tok not in self._redact_tokens:
                            self._redact_tokens.append(tok)
                    self._echo_mute_until = time.monotonic() + self._ECHO_MUTE_S
                # Quote arguments and escape embedded quotes/backslashes; never log admin code or password (I-25)
                cmd = f'\r\ndoorbell_wifi "{admin}" "{esc_ssid}" "{esc_pw}"\r\n'
                self._ser.write(cmd.encode("utf-8"))
                self._append(f"\n[Sent authenticated doorbell_wifi for SSID '{ssid}' (credentials redacted)]\n")
                self.admin_var.set("")
                self.pass_var.set("")
            except serial.SerialException as exc:
                self._append(f"\n[Error sending command: {exc}]\n")

    def _reset_board(self) -> None:
        if self._ser and self._ser.is_open:
            try:
                self._ser.dtr = False
                self._ser.rts = True
                time.sleep(0.1)
                self._ser.rts = False
                self._append("\n[Hard resetting via RTS pin...]\n")
            except serial.SerialException as exc:
                self._append(f"\n[Reset error: {exc}]\n")

    def _reader_thread(self) -> None:
        try:
            self._ser = serial.Serial(self.port, baudrate=115200, timeout=0.08)
            self._rx_queue.put(f"[Connected to {self.port} at 115200 baud]\n")
            decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
            pending = ""
            pending_muted = False
            last_rx = time.monotonic()
            while not self._stop_evt.is_set():
                data = self._ser.read(512)
                if data:
                    pending += decoder.decode(data)
                    # Text received while a doorbell_wifi echo is expected stays filtered until it is emitted
                    pending_muted = pending_muted or self._is_echo_muted()
                    last_rx = time.monotonic()
                    if "\n" in pending:
                        lines = pending.splitlines(keepends=True)
                        if not lines[-1].endswith("\n"):
                            pending = lines.pop()
                        else:
                            pending = ""
                        # Strip ANSI on whole lines so escape sequences split across reads are removed too
                        out = self._scrub_rx_text(strip_ansi("".join(lines)), pending_muted)
                        if not pending:
                            pending_muted = False
                        if out:
                            self._rx_queue.put(out)
                elif pending and (time.monotonic() - last_rx) >= 0.15:
                    out = self._scrub_rx_text(strip_ansi(pending), pending_muted)
                    pending = ""
                    pending_muted = False
                    if out:
                        self._rx_queue.put(out)
        except Exception as exc:
            self._rx_queue.put(f"\n[Serial port error on {self.port}: {exc}]\n")
        finally:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.close()
                except Exception:
                    pass

    def _poll_rx(self) -> None:
        if self._stop_evt.is_set():
            return
        try:
            while True:
                chunk = self._rx_queue.get_nowait()
                self._append(chunk)
        except queue.Empty:
            pass
        self.after(50, self._poll_rx)

    def close(self) -> None:
        self._stop_evt.set()
        if self._ser and self._ser.is_open:
            try:
                self._ser.close()
            except Exception:
                pass
        self.destroy()


class DeviceCard(ttk.LabelFrame):
    """Right-column card representing one detected COM port / ESP32 device."""

    def __init__(
        self,
        parent: tk.Widget,
        app: "DoorbellFlasherApp",
        port: str,
        description: str,
    ) -> None:
        title = f"{description} ({port})" if description else port
        super().__init__(parent, text=title, padding=(10, 6))
        self.app = app
        self.port = port
        self.description = description
        self._busy = False
        self._monitor_win: Optional[SerialMonitorWindow] = None

        # 1. Chip info line
        self.chip_info_var = tk.StringVar(value="Probing ESP32 chip info...")
        chip_lbl = ttk.Label(self, textvariable=self.chip_info_var, font=("Segoe UI", 9))
        chip_lbl.pack(anchor=tk.W, pady=(0, 6))

        # 2. Role & Firmware selector row
        row_fw = ttk.Frame(self)
        row_fw.pack(fill=tk.X, pady=(0, 6))

        ttk.Label(row_fw, text="Role").pack(side=tk.LEFT, padx=(0, 4))
        self.role_var = tk.StringVar(value="panel")
        self.role_cb = ttk.Combobox(
            row_fw,
            textvariable=self.role_var,
            values=["panel", "apartment"],
            state="readonly",
            width=10,
        )
        self.role_cb.pack(side=tk.LEFT, padx=(0, 12))
        self.role_cb.bind("<<ComboboxSelected>>", self._on_role_changed)

        ttk.Label(row_fw, text="Firmware").pack(side=tk.LEFT, padx=(0, 4))
        self.fw_var = tk.StringVar()
        self.fw_cb = ttk.Combobox(row_fw, textvariable=self.fw_var, width=32)
        self.fw_cb.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))

        self.browse_btn = ttk.Button(row_fw, text="Browse...", width=9, command=self._on_browse)
        self.browse_btn.pack(side=tk.LEFT)

        # 3. Panel provisioning rows (Admin code, User code, optional Building Wi-Fi)
        self.panel_prov_frame = ttk.Frame(self)
        self.panel_prov_frame.pack(fill=tk.X, pady=(0, 6))

        row_codes = ttk.Frame(self.panel_prov_frame)
        row_codes.pack(fill=tk.X, pady=(0, 4))

        ttk.Label(row_codes, text="Admin code").pack(side=tk.LEFT, padx=(0, 4))
        self.admin_code_var = tk.StringVar(value=generate_random_digits(self.app.get_configured_admin_len()))
        self.admin_entry = ttk.Entry(row_codes, textvariable=self.admin_code_var, width=12)
        self.admin_entry.pack(side=tk.LEFT, padx=(0, 4))
        self.rand_admin_btn = ttk.Button(
            row_codes,
            text="Random",
            width=8,
            command=lambda: self.admin_code_var.set(generate_random_digits(self.app.get_configured_admin_len())),
        )
        self.rand_admin_btn.pack(side=tk.LEFT, padx=(0, 14))

        ttk.Label(row_codes, text="User code").pack(side=tk.LEFT, padx=(0, 4))
        self.user_code_var = tk.StringVar(value=generate_random_digits(self.app.get_configured_user_len()))
        self.user_entry = ttk.Entry(row_codes, textvariable=self.user_code_var, width=10)
        self.user_entry.pack(side=tk.LEFT, padx=(0, 4))
        self.rand_user_btn = ttk.Button(
            row_codes,
            text="Random",
            width=8,
            command=lambda: self.user_code_var.set(generate_random_digits(self.app.get_configured_user_len())),
        )
        self.rand_user_btn.pack(side=tk.LEFT)

        row_wifi = ttk.Frame(self.panel_prov_frame)
        row_wifi.pack(fill=tk.X)

        ttk.Label(row_wifi, text="Wi-Fi SSID (opt)").pack(side=tk.LEFT, padx=(0, 4))
        self.wifi_ssid_var = tk.StringVar(value="")
        self.wifi_ssid_entry = ttk.Entry(row_wifi, textvariable=self.wifi_ssid_var, width=16)
        self.wifi_ssid_entry.pack(side=tk.LEFT, padx=(0, 10))

        ttk.Label(row_wifi, text="Wi-Fi Pass (opt)").pack(side=tk.LEFT, padx=(0, 4))
        self.wifi_pass_var = tk.StringVar(value="")
        self.wifi_pass_entry = ttk.Entry(row_wifi, textvariable=self.wifi_pass_var, width=16, show="*")
        self.wifi_pass_entry.pack(side=tk.LEFT, padx=(0, 6))

        self.show_pass_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            row_wifi,
            text="Show",
            variable=self.show_pass_var,
            command=lambda: self.wifi_pass_entry.configure(show="" if self.show_pass_var.get() else "*"),
        ).pack(side=tk.LEFT)

        # 4. Action buttons row
        row_btns = ttk.Frame(self)
        row_btns.pack(fill=tk.X, pady=(2, 6))

        self.btn_flash = ttk.Button(row_btns, text="Flash", width=9, command=self._on_flash)
        self.btn_flash.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_flash_nvs = ttk.Button(row_btns, text="Flash NVS", width=10, command=self._on_flash_nvs)
        self.btn_flash_nvs.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_monitor = ttk.Button(row_btns, text="Monitor", width=9, command=self._on_monitor)
        self.btn_monitor.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_factory = ttk.Button(row_btns, text="Factory reset", width=12, command=self._on_factory_reset)
        self.btn_factory.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_erase = ttk.Button(row_btns, text="Erase flash", width=10, command=self._on_erase_flash)
        self.btn_erase.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_scan = ttk.Button(row_btns, text="Scan", width=8, command=self._on_scan)
        self.btn_scan.pack(side=tk.LEFT)

        # 5. Progress bar & status row
        row_prog = ttk.Frame(self)
        row_prog.pack(fill=tk.X)

        self.progress_var = tk.DoubleVar(value=0.0)
        self.progressbar = ttk.Progressbar(row_prog, variable=self.progress_var, maximum=100.0, length=240)
        self.progressbar.pack(side=tk.LEFT, padx=(0, 10))

        self.status_lbl = tk.Label(row_prog, text="Detecting...", fg="#555555", font=("Segoe UI", 9))
        self.status_lbl.pack(side=tk.LEFT)

        self.refresh_firmware_list()

    def set_status(self, text: str, color: str = "#1b7e2b", progress: Optional[float] = None) -> None:
        if not self.winfo_exists():
            return
        self.status_lbl.configure(text=text, fg=color)
        if progress is not None:
            self.progress_var.set(progress)

    def set_busy(self, busy: bool) -> None:
        self._busy = busy
        self.app.update_detect_button_state()
        if not self.winfo_exists():
            return
        state = tk.DISABLED if busy else tk.NORMAL
        for btn in (
            self.btn_flash,
            self.btn_flash_nvs,
            self.btn_monitor,
            self.btn_factory,
            self.btn_erase,
            self.btn_scan,
            self.browse_btn,
        ):
            btn.configure(state=state)
        self.role_cb.configure(state=tk.DISABLED if busy else "readonly")

    def _on_role_changed(self, _event: Optional[tk.Event] = None) -> None:
        role = self.role_var.get()
        if role == "panel":
            self.panel_prov_frame.pack(fill=tk.X, pady=(0, 6), before=self.btn_flash.master)
        else:
            self.panel_prov_frame.pack_forget()
        self.refresh_firmware_list()

    def refresh_firmware_list(self) -> None:
        BUILDS_DIR.mkdir(parents=True, exist_ok=True)
        bins = sorted(BUILDS_DIR.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
        names = [b.name for b in bins]
        self.fw_cb["values"] = names

        role = self.role_var.get()
        prefix = "panel_" if role == "panel" else "apartment_"
        for name in names:
            if name.startswith(prefix):
                self.fw_var.set(name)
                return
        if names and not self.fw_var.get():
            self.fw_var.set(names[0])

    def _on_browse(self) -> None:
        initial = str(BUILDS_DIR if BUILDS_DIR.is_dir() else PROJECT_ROOT)
        path = filedialog.askopenfilename(
            parent=self,
            title="Select Firmware Binary (.bin)",
            initialdir=initial,
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")],
        )
        if path:
            self.fw_var.set(path)

    def _resolve_fw_path(self) -> Optional[Path]:
        raw = self.fw_var.get().strip()
        if not raw:
            return None
        p = Path(raw)
        if not p.is_absolute():
            p = BUILDS_DIR / raw
        return p if p.is_file() else None

    def _close_monitor_if_open(self) -> None:
        if self._monitor_win and self._monitor_win.winfo_exists():
            self._monitor_win.close()
            self._monitor_win = None

    def _on_flash(self) -> None:
        if self._busy:
            return
        fw_path = self._resolve_fw_path()
        if not fw_path:
            messagebox.showerror("Firmware Not Found", "Please build or select a valid .bin firmware file first.")
            return

        layout, detected_role = inspect_firmware_binary(fw_path)
        selected_role = self.role_var.get()
        if detected_role in ("panel", "apartment") and detected_role != selected_role:
            ok = messagebox.askyesno(
                "Role Mismatch Warning",
                f"The selected binary '{fw_path.name}' appears to be a '{detected_role}' image, "
                f"but the card role is set to '{selected_role}'.\n\nContinue flashing anyway?",
            )
            if not ok:
                return

        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Flashing firmware...", "#b36b00", 5.0)
        threading.Thread(
            target=self.app.worker_flash_firmware,
            args=(self, fw_path, layout),
            daemon=True,
        ).start()

    def _on_flash_nvs(self) -> None:
        if self._busy:
            return
        role = self.role_var.get()
        if role == "panel":
            admin_code = self.admin_code_var.get().strip()
            user_code = self.user_code_var.get().strip()
            ssid = self.wifi_ssid_var.get().strip()
            pw = self.wifi_pass_var.get()

            req_admin_len = self.app.get_configured_admin_len()
            req_user_len = self.app.get_configured_user_len()

            if not admin_code.isdigit() or len(admin_code) != req_admin_len:
                messagebox.showerror(
                    "Invalid Admin Code",
                    f"Admin code must be exactly {req_admin_len} digits (0-9).",
                )
                return
            if not user_code.isdigit() or len(user_code) != req_user_len:
                messagebox.showerror(
                    "Invalid User Code",
                    f"User code must be exactly {req_user_len} digits (0-9).",
                )
                return
            if user_code == admin_code:
                messagebox.showerror("Invalid Codes", "User code and Admin code cannot be identical.")
                return
            if "\x00" in ssid or "\x00" in pw:
                messagebox.showerror("Invalid Wi-Fi Credentials", "SSID and password cannot contain null bytes.")
                return
            ssid_bytes = len(ssid.encode("utf-8"))
            pw_bytes = len(pw.encode("utf-8"))
            if ssid_bytes > 32 or (pw and (pw_bytes < 8 or pw_bytes > 63)):
                messagebox.showerror(
                    "Invalid Wi-Fi Credentials",
                    "Wi-Fi SSID must be 0-32 UTF-8 bytes, and password must be empty or 8-63 UTF-8 bytes.",
                )
                return

            ok = messagebox.askyesno(
                "Confirm Panel NVS Provisioning (I-14)",
                f"Flashing the NVS partition on {self.port} replaces the entire 24 KB NVS region (0x9000) "
                "and erases any existing apartment registrations on this panel.\n\n"
                f"Provision Admin code ({len(admin_code)} digits) and User code ({len(user_code)} digits) now?",
            )
            if not ok:
                return

            self._close_monitor_if_open()
            self.set_busy(True)
            self.set_status("Flashing panel NVS...", "#b36b00", 10.0)
            threading.Thread(
                target=self.app.worker_flash_panel_nvs,
                args=(self, admin_code, user_code, ssid, pw),
                daemon=True,
            ).start()
        else:
            ok = messagebox.askyesno(
                "Confirm Apartment NVS Clear",
                f"Erase the 24 KB NVS partition (0x9000) on apartment module {self.port}?\n"
                "This clears its pairing state and building Wi-Fi credentials.",
            )
            if not ok:
                return
            self._close_monitor_if_open()
            self.set_busy(True)
            self.set_status("Clearing unit NVS...", "#b36b00", 10.0)
            threading.Thread(
                target=self.app.worker_erase_nvs_region,
                args=(self, "NVS cleared (unpaired)"),
                daemon=True,
            ).start()

    def _on_monitor(self) -> None:
        if self._busy:
            return
        if self._monitor_win and self._monitor_win.winfo_exists():
            self._monitor_win.lift()
            return
        self._monitor_win = SerialMonitorWindow(self, self.port, self.app.log)

    def _on_factory_reset(self) -> None:
        if self._busy:
            return
        ok = messagebox.askyesno(
            "Confirm Factory Reset",
            f"Reset NVS partition (0x9000..0xF000) on {self.port} to factory state?\n\n"
            "All apartment registrations and stored credentials in NVS will be erased.",
        )
        if not ok:
            return
        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Factory resetting NVS...", "#b36b00", 10.0)
        threading.Thread(
            target=self.app.worker_erase_nvs_region,
            args=(self, "Factory reset complete"),
            daemon=True,
        ).start()

    def _on_erase_flash(self) -> None:
        if self._busy:
            return
        ok = messagebox.askyesno(
            "Confirm Full Flash Erase",
            f"Erase the ENTIRE 2 MB flash memory on {self.port}?\n\n"
            "This removes bootloader, firmware, and all NVS data.",
        )
        if not ok:
            return
        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Erasing flash...", "#b36b00", 10.0)
        threading.Thread(
            target=self.app.worker_erase_full_flash,
            args=(self,),
            daemon=True,
        ).start()

    def _on_scan(self) -> None:
        if self._busy:
            return
        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Scanning device...", "#b36b00", 20.0)
        threading.Thread(
            target=self.app.worker_scan_device,
            args=(self,),
            daemon=True,
        ).start()


class DoorbellFlasherApp:
    """Main application controller for the two-column DorbellAp Flasher GUI."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("DorbellAp — Wi-Fi Doorbell Flasher")
        self.root.geometry("1260x820")
        self.root.minsize(1060, 700)

        BUILDS_DIR.mkdir(parents=True, exist_ok=True)

        self._ui_queue: queue.Queue[Callable[[], None]] = queue.Queue()
        self._building = False
        self._detecting = False
        self._device_cards: List[DeviceCard] = []

        # Store Tkinter variables for Panel and Apartment Kconfig parameters
        self.panel_vars: Dict[str, tk.Variable] = {}
        self.unit_vars: Dict[str, tk.Variable] = {}
        self._init_param_vars()

        self._build_layout()
        self._populate_config_panel()
        self._update_last_build_label()
        for warning in KCONFIG_SYNC_WARNINGS:
            self.log(f"[Kconfig] WARNING: {warning}\n")

        self.root.after(50, self._process_ui_queue)
        # Trigger initial port detection shortly after window appears
        self.root.after(250, self.detect_ports)

    def _init_param_vars(self) -> None:
        for spec in PANEL_PARAMS:
            if spec.kind == "bool":
                self.panel_vars[spec.kconfig] = tk.BooleanVar(value=bool(spec.default))
            else:
                self.panel_vars[spec.kconfig] = tk.StringVar(value=str(spec.default))

        for spec in APARTMENT_PARAMS:
            if spec.kind == "bool":
                self.unit_vars[spec.kconfig] = tk.BooleanVar(value=bool(spec.default))
            else:
                self.unit_vars[spec.kconfig] = tk.StringVar(value=str(spec.default))

        # Load current values from firmware/panel/sdkconfig and firmware/unit/sdkconfig if present
        self._load_existing_sdkconfig(PANEL_FW_DIR / "sdkconfig", PANEL_PARAMS, self.panel_vars)
        self._load_existing_sdkconfig(UNIT_FW_DIR / "sdkconfig", APARTMENT_PARAMS, self.unit_vars)

    @staticmethod
    def _load_existing_sdkconfig(
        sdkconfig_path: Path,
        specs: List[ParamSpec],
        vars_map: Dict[str, tk.Variable],
    ) -> None:
        if not sdkconfig_path.is_file():
            return
        try:
            lines = sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            return
        kv: Dict[str, str] = {}
        for line in lines:
            if line.startswith("CONFIG_"):
                parts = line.split("=", 1)
                if len(parts) == 2:
                    kv[parts[0][7:]] = parts[1]
            elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
                key = line[len("# CONFIG_") : -len(" is not set")]
                kv[key] = "n"

        for spec in specs:
            if spec.kconfig in kv:
                raw = kv[spec.kconfig]
                if spec.kind == "bool":
                    vars_map[spec.kconfig].set(raw == "y")
                elif spec.kind == "str":
                    vars_map[spec.kconfig].set(unescape_quoted(raw))
                else:
                    vars_map[spec.kconfig].set(raw)

    def get_configured_admin_len(self) -> int:
        try:
            v = int(str(self.panel_vars["DOORBELL_PANEL_ADMIN_CODE_LEN"].get()).strip())
            return v if 8 <= v <= 10 else 8
        except ValueError:
            return 8

    def get_configured_user_len(self) -> int:
        try:
            v = int(str(self.panel_vars["DOORBELL_PANEL_USER_CODE_LEN"].get()).strip())
            return v if 4 <= v <= 8 else 4
        except ValueError:
            return 4

    def _build_layout(self) -> None:
        style = ttk.Style(self.root)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        # Main two-column paned/grid container
        main_pane = ttk.Frame(self.root, padding=(12, 10))
        main_pane.pack(fill=tk.BOTH, expand=True)
        main_pane.columnconfigure(0, weight=4, minsize=430)
        main_pane.columnconfigure(1, weight=7, minsize=620)
        main_pane.rowconfigure(0, weight=1)

        # ==================== LEFT COLUMN: Customization and build ====================
        left_col = ttk.Frame(main_pane)
        left_col.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        left_col.columnconfigure(0, weight=1)
        left_col.rowconfigure(3, weight=3)  # Configuration scrollable area
        left_col.rowconfigure(5, weight=2)  # Log area

        lbl_left_hdr = ttk.Label(left_col, text="Customization and build", font=("Segoe UI", 13, "bold"))
        lbl_left_hdr.grid(row=0, column=0, sticky="w", pady=(0, 8))

        # 1. Profile
        prof_box = ttk.LabelFrame(left_col, text="1. Profile", padding=(10, 6))
        prof_box.grid(row=1, column=0, sticky="ew", pady=(0, 8))

        self.profile_var = tk.StringVar(value="dev")
        ttk.Radiobutton(
            prof_box,
            text="Development (reversible)",
            variable=self.profile_var,
            value="dev",
        ).pack(anchor=tk.W)
        rb_prod = ttk.Radiobutton(
            prof_box,
            text="Production (irreversible, burns eFuses)",
            variable=self.profile_var,
            value="prod",
            state=tk.DISABLED,
        )
        rb_prod.pack(anchor=tk.W, pady=(2, 2))
        tk.Label(
            prof_box,
            text="Production is not used in this scope (ADR 0013 / I-16).",
            fg="#666666",
            font=("Segoe UI", 8),
        ).pack(anchor=tk.W)

        # 2. Role to configure and build
        role_box = ttk.LabelFrame(left_col, text="2. Role to configure and build", padding=(10, 6))
        role_box.grid(row=2, column=0, sticky="ew", pady=(0, 8))

        self.build_role_var = tk.StringVar(value="panel")
        ttk.Radiobutton(
            role_box,
            text="Panel",
            variable=self.build_role_var,
            value="panel",
            command=self._populate_config_panel,
        ).pack(side=tk.LEFT, padx=(0, 18))
        ttk.Radiobutton(
            role_box,
            text="Apartment",
            variable=self.build_role_var,
            value="apartment",
            command=self._populate_config_panel,
        ).pack(side=tk.LEFT)

        ttk.Button(
            role_box,
            text="Reset defaults",
            width=13,
            command=self._reset_current_role_defaults,
        ).pack(side=tk.RIGHT)

        # 3. Configuration (hover a name for help)
        cfg_box = ttk.LabelFrame(left_col, text="3. Configuration (hover a name for help)", padding=(6, 6))
        cfg_box.grid(row=3, column=0, sticky="nsew", pady=(0, 8))
        cfg_box.columnconfigure(0, weight=1)
        cfg_box.rowconfigure(0, weight=1)

        self.cfg_canvas = tk.Canvas(cfg_box, highlightthickness=0)
        cfg_vscroll = ttk.Scrollbar(cfg_box, orient=tk.VERTICAL, command=self.cfg_canvas.yview)
        self.cfg_inner = ttk.Frame(self.cfg_canvas, padding=(6, 4))

        self.cfg_inner.bind(
            "<Configure>",
            lambda _e: self.cfg_canvas.configure(scrollregion=self.cfg_canvas.bbox("all")),
        )
        self._cfg_win_id = self.cfg_canvas.create_window((0, 0), window=self.cfg_inner, anchor="nw")
        self.cfg_canvas.bind(
            "<Configure>",
            lambda e: self.cfg_canvas.itemconfigure(self._cfg_win_id, width=e.width),
        )
        self.cfg_canvas.configure(yscrollcommand=cfg_vscroll.set)

        self.cfg_canvas.grid(row=0, column=0, sticky="nsew")
        cfg_vscroll.grid(row=0, column=1, sticky="ns")

        # Mousewheel support over configuration canvas
        self.cfg_canvas.bind("<Enter>", lambda _e: self._bind_mousewheel(self.cfg_canvas))
        self.cfg_canvas.bind("<Leave>", lambda _e: self._unbind_mousewheel(self.cfg_canvas))

        # 4. Build
        build_box = ttk.LabelFrame(left_col, text="4. Build", padding=(10, 8))
        build_box.grid(row=4, column=0, sticky="ew", pady=(0, 8))

        self.btn_build = ttk.Button(build_box, text="Build firmware", width=15, command=self._on_build_clicked)
        self.btn_build.pack(side=tk.LEFT, padx=(0, 10))

        self.last_build_var = tk.StringVar(value="Last build: (none)")
        ttk.Label(build_box, textvariable=self.last_build_var, font=("Segoe UI", 9)).pack(side=tk.LEFT)

        # 5. Log
        log_box = ttk.LabelFrame(left_col, text="Log", padding=(6, 6))
        log_box.grid(row=5, column=0, sticky="nsew")
        log_box.columnconfigure(0, weight=1)
        log_box.rowconfigure(0, weight=1)

        self.log_text = tk.Text(log_box, height=8, wrap=tk.WORD, font=("Consolas", 8))
        log_sb = ttk.Scrollbar(log_box, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=log_sb.set)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        log_sb.grid(row=0, column=1, sticky="ns")

        # ==================== RIGHT COLUMN: Detection and flashing ====================
        right_col = ttk.Frame(main_pane)
        right_col.grid(row=0, column=1, sticky="nsew")
        right_col.columnconfigure(0, weight=1)
        right_col.rowconfigure(1, weight=1)

        right_hdr = ttk.Frame(right_col)
        right_hdr.grid(row=0, column=0, sticky="ew", pady=(0, 8))

        ttk.Label(right_hdr, text="Detection and flashing", font=("Segoe UI", 13, "bold")).pack(side=tk.LEFT)
        self.btn_detect = ttk.Button(right_hdr, text="Detect", width=12, command=self.detect_ports)
        self.btn_detect.pack(side=tk.RIGHT)

        # Scrollable container for detected COM port cards
        cards_outer = ttk.Frame(right_col, relief=tk.SOLID, borderwidth=1)
        cards_outer.grid(row=1, column=0, sticky="nsew")
        cards_outer.columnconfigure(0, weight=1)
        cards_outer.rowconfigure(0, weight=1)

        self.cards_canvas = tk.Canvas(cards_outer, highlightthickness=0)
        cards_vscroll = ttk.Scrollbar(cards_outer, orient=tk.VERTICAL, command=self.cards_canvas.yview)
        self.cards_inner = ttk.Frame(self.cards_canvas, padding=(10, 10))

        self.cards_inner.bind(
            "<Configure>",
            lambda _e: self.cards_canvas.configure(scrollregion=self.cards_canvas.bbox("all")),
        )
        self._cards_win_id = self.cards_canvas.create_window((0, 0), window=self.cards_inner, anchor="nw")
        self.cards_canvas.bind(
            "<Configure>",
            lambda e: self.cards_canvas.itemconfigure(self._cards_win_id, width=e.width),
        )
        self.cards_canvas.configure(yscrollcommand=cards_vscroll.set)

        self.cards_canvas.grid(row=0, column=0, sticky="nsew")
        cards_vscroll.grid(row=0, column=1, sticky="ns")

    def _bind_mousewheel(self, canvas: tk.Canvas) -> None:
        canvas.bind_all("<MouseWheel>", lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), "units"))

    def _unbind_mousewheel(self, canvas: tk.Canvas) -> None:
        canvas.unbind_all("<MouseWheel>")

    def _populate_config_panel(self) -> None:
        for child in self.cfg_inner.winfo_children():
            child.destroy()

        role = self.build_role_var.get()
        is_panel = role == "panel"
        specs = PANEL_PARAMS if is_panel else APARTMENT_PARAMS
        vars_map = self.panel_vars if is_panel else self.unit_vars

        role_title = "Doorbell panel" if is_panel else "Doorbell apartment module"
        ttk.Label(self.cfg_inner, text=role_title, font=("Segoe UI", 10, "bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 6)
        )

        self.cfg_inner.columnconfigure(0, weight=1)
        self.cfg_inner.columnconfigure(1, weight=0)

        row_idx = 1
        current_section = ""

        for spec in specs:
            if spec.section != current_section:
                current_section = spec.section
                ttk.Label(
                    self.cfg_inner,
                    text=current_section,
                    font=("Segoe UI", 9, "bold"),
                ).grid(row=row_idx, column=0, columnspan=2, sticky="w", padx=(6, 0), pady=(8, 2))
                row_idx += 1

                if current_section == "GPIO":
                    note = tk.Label(
                        self.cfg_inner,
                        text="ESP32: GPIO 6-11 are used by flash; GPIO 34-39 are input-only without pull-ups.",
                        fg="#666666",
                        justify=tk.LEFT,
                        anchor="w",
                        wraplength=330,
                        font=("Segoe UI", 8),
                    )
                    note.grid(row=row_idx, column=0, columnspan=2, sticky="ew", padx=(6, 0), pady=(0, 4))
                    row_idx += 1

            range_hint = ""
            if spec.kind == "int" and spec.min_val is not None and spec.max_val is not None:
                range_hint = f"\nRange: {spec.min_val} .. {spec.max_val} (Default: {spec.default})"
            else:
                range_hint = f"\nDefault: {spec.default}"

            lbl = ttk.Label(self.cfg_inner, text=spec.label)
            lbl.grid(row=row_idx, column=0, sticky="w", padx=(14, 8), pady=2)
            ToolTip(lbl, f"{spec.kconfig}\n{spec.help_text}{range_hint}")

            var = vars_map[spec.kconfig]
            if spec.kind == "bool":
                cb = ttk.Checkbutton(self.cfg_inner, variable=var)
                cb.grid(row=row_idx, column=1, sticky="w", padx=(0, 8), pady=2)
            else:
                ent = ttk.Entry(self.cfg_inner, textvariable=var, width=14)
                ent.grid(row=row_idx, column=1, sticky="ew", padx=(0, 8), pady=2)

            row_idx += 1

        self.cfg_canvas.yview_moveto(0.0)

    def _reset_current_role_defaults(self) -> None:
        role = self.build_role_var.get()
        specs = PANEL_PARAMS if role == "panel" else APARTMENT_PARAMS
        vars_map = self.panel_vars if role == "panel" else self.unit_vars
        for spec in specs:
            vars_map[spec.kconfig].set(spec.default if spec.kind == "bool" else str(spec.default))
        self.log(f"[Config] Restored factory Kconfig defaults for {role}.\n")

    def _validate_config(self, role: str) -> Optional[Dict[str, Any]]:
        """Validate all parameters and cross-parameter rules before invoking idf.py build."""
        is_panel = role == "panel"
        specs = PANEL_PARAMS if is_panel else APARTMENT_PARAMS
        vars_map = self.panel_vars if is_panel else self.unit_vars

        parsed: Dict[str, Any] = {}
        used_gpios: Dict[int, str] = {}

        for spec in specs:
            raw_val = vars_map[spec.kconfig].get()
            if spec.kind == "bool":
                parsed[spec.kconfig] = bool(raw_val)
            elif spec.kind == "int":
                try:
                    iv = int(str(raw_val).strip())
                except ValueError:
                    messagebox.showerror("Validation Error", f"'{spec.label}' must be an integer.")
                    return None
                if spec.min_val is not None and iv < spec.min_val:
                    messagebox.showerror(
                        "Validation Error",
                        f"'{spec.label}' ({iv}) is below minimum {spec.min_val}.",
                    )
                    return None
                if spec.max_val is not None and iv > spec.max_val:
                    messagebox.showerror(
                        "Validation Error",
                        f"'{spec.label}' ({iv}) exceeds maximum {spec.max_val}.",
                    )
                    return None
                if spec.is_gpio_output and iv not in ESP32_VALID_OUTPUT_GPIOS:
                    messagebox.showerror(
                        "Invalid ESP32 Output GPIO",
                        f"'{spec.label}' (GPIO {iv}) is not a valid output GPIO on classic ESP32.\n"
                        "Avoid flash pins 6-11 and input-only pins 34-39.",
                    )
                    return None
                if spec.is_gpio_input and iv not in ESP32_VALID_GPIOS:
                    messagebox.showerror(
                        "Invalid ESP32 Input GPIO",
                        f"'{spec.label}' (GPIO {iv}) is not a valid GPIO on classic ESP32.\n"
                        "Avoid flash pins 6-11.",
                    )
                    return None
                if spec.is_gpio_output or spec.is_gpio_input:
                    if iv in used_gpios:
                        messagebox.showerror(
                            "Duplicate GPIO Assignment",
                            f"GPIO {iv} is assigned to both '{used_gpios[iv]}' and '{spec.label}'.",
                        )
                        return None
                    used_gpios[iv] = spec.label
                parsed[spec.kconfig] = iv
            else:
                sv = str(raw_val).strip()
                if spec.max_len is not None and len(sv) > spec.max_len:
                    messagebox.showerror(
                        "Validation Error",
                        f"'{spec.label}' exceeds maximum length {spec.max_len}.",
                    )
                    return None
                parsed[spec.kconfig] = sv

        # Cross-parameter checks (matching doorbell_config_checks.h, I-66)
        if parsed["DOORBELL_LED_DIM_BRIGHTNESS_PCT"] >= parsed["DOORBELL_LED_FULL_BRIGHTNESS_PCT"]:
            messagebox.showerror(
                "Validation Error",
                "Dim brightness (%) must be strictly less than Full brightness (%).",
            )
            return None

        min_multi = 3 * parsed["DOORBELL_LED_MULTI_PULSE_MS"] + 2 * parsed["DOORBELL_LED_MULTI_GAP_MS"]
        if parsed["DOORBELL_LED_MULTI_PERIOD_MS"] <= min_multi:
            messagebox.showerror(
                "Validation Error",
                f"Multi-flash period ({parsed['DOORBELL_LED_MULTI_PERIOD_MS']} ms) must exceed "
                f"3 pulses + 2 gaps ({min_multi} ms).",
            )
            return None

        if parsed["DOORBELL_LED_SPARSE_PERIOD_MS"] <= parsed["DOORBELL_LED_MULTI_PULSE_MS"]:
            messagebox.showerror(
                "Validation Error",
                "Sparse flash period must exceed Multi-flash pulse duration.",
            )
            return None

        if len(parsed["DOORBELL_WIFI_COUNTRY_CODE"]) != 2:
            messagebox.showerror("Validation Error", "Wi-Fi country code must be exactly 2 characters (e.g., TR).")
            return None

        if is_panel:
            if len(parsed["DOORBELL_PANEL_KEY_MAP"]) != 12:
                messagebox.showerror("Validation Error", "Keypad key map must be exactly 12 characters.")
                return None
            if parsed["DOORBELL_PANEL_MIN_APT_NO"] > parsed["DOORBELL_PANEL_MAX_APT_NO"]:
                messagebox.showerror(
                    "Validation Error",
                    "Lowest apartment number must be <= Highest apartment number.",
                )
                return None
            if parsed["DOORBELL_PANEL_LOCKOUT_FIRST_S"] > parsed["DOORBELL_PANEL_LOCKOUT_MAX_S"]:
                messagebox.showerror(
                    "Validation Error",
                    "First lockout duration must be <= Maximum lockout duration.",
                )
                return None
            if parsed["DOORBELL_PANEL_RESET_MIN_HOLD_S"] >= parsed["DOORBELL_PANEL_RESET_MAX_HOLD_S"]:
                messagebox.showerror(
                    "Validation Error",
                    "Reset button min hold must be strictly less than max hold.",
                )
                return None
            if (
                parsed["DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL"]
                and parsed["DOORBELL_PANEL_GPIO_RESET_BTN"] in ESP32_INPUT_ONLY_GPIOS
            ):
                messagebox.showerror(
                    "Validation Error",
                    f"GPIO {parsed['DOORBELL_PANEL_GPIO_RESET_BTN']} is input-only (GPIO 34-39) and has no internal "
                    "pull resistor. Either disable 'Reset button internal pull' (with an external resistor) or choose another GPIO.",
                )
                return None
            for col_key in ("DOORBELL_PANEL_GPIO_COL0", "DOORBELL_PANEL_GPIO_COL1", "DOORBELL_PANEL_GPIO_COL2"):
                if parsed[col_key] in ESP32_INPUT_ONLY_GPIOS:
                    messagebox.showerror(
                        "Validation Error",
                        f"Keypad column GPIO {parsed[col_key]} is in GPIO 34-39 which lacks internal pull resistors "
                        "required by the matrix keypad scanner.",
                    )
                    return None
        else:
            if parsed["DOORBELL_UNIT_DOOR_BTN_MIN_MS"] >= parsed["DOORBELL_UNIT_DOOR_BTN_MAX_S"] * 1000:
                messagebox.showerror(
                    "Validation Error",
                    "Door button min press (ms) must be less than max press (s).",
                )
                return None
            if not (
                parsed["DOORBELL_UNIT_HOLD_BTN_MIN_S"]
                < parsed["DOORBELL_UNIT_HOLD_BTN_MAX_S"]
                < parsed["DOORBELL_UNIT_HOLD_BTN_CANCEL_S"]
            ):
                messagebox.showerror(
                    "Validation Error",
                    "Pair/reset button hold times must satisfy: min hold < max hold < cancel after.",
                )
                return None
            if parsed["DOORBELL_UNIT_BTNS_INTERNAL_PULL"]:
                for bkey in (
                    "DOORBELL_UNIT_GPIO_DOOR_BTN",
                    "DOORBELL_UNIT_GPIO_PAIR_BTN",
                    "DOORBELL_UNIT_GPIO_RESET_BTN",
                ):
                    if parsed[bkey] in ESP32_INPUT_ONLY_GPIOS:
                        messagebox.showerror(
                            "Validation Error",
                            f"GPIO {parsed[bkey]} is in GPIO 34-39 and has no internal pull resistor.",
                        )
                        return None

        return parsed

    # ---- Thread-safe UI Queue & Logging ----

    def post_ui(self, fn: Callable[[], None]) -> None:
        self._ui_queue.put(fn)

    def _process_ui_queue(self) -> None:
        try:
            while True:
                fn = self._ui_queue.get_nowait()
                try:
                    fn()
                except tk.TclError:
                    # Widget may have been destroyed before the queued UI callback ran
                    pass
                except Exception as exc:
                    sys.stderr.write(f"[UI Queue Error] {exc}\n")
        except queue.Empty:
            pass
        finally:
            if self.root.winfo_exists():
                self.root.after(40, self._process_ui_queue)

    def update_detect_button_state(self) -> None:
        if not hasattr(self, "btn_detect") or not self.btn_detect.winfo_exists():
            return
        any_busy = self._detecting or any(c._busy for c in self._device_cards)
        self.btn_detect.configure(state=tk.DISABLED if any_busy else tk.NORMAL)

    def log(self, text: str) -> None:
        def _append() -> None:
            self.log_text.insert(tk.END, text)
            self.log_text.see(tk.END)

        self.post_ui(_append)

    def _update_last_build_label(self, filename: Optional[str] = None) -> None:
        if filename:
            self.last_build_var.set(f"Last build: {filename}")
            return
        bins = sorted(BUILDS_DIR.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
        if bins:
            self.last_build_var.set(f"Last build: {bins[0].name}")
        else:
            self.last_build_var.set("Last build: (none)")

    # ---- Build Firmware Pipeline ----

    def _on_build_clicked(self) -> None:
        if self._building:
            return
        role = self.build_role_var.get()
        validated = self._validate_config(role)
        if validated is None:
            return

        self._building = True
        self.btn_build.configure(state=tk.DISABLED)
        threading.Thread(
            target=self._worker_build_firmware,
            args=(role, validated),
            daemon=True,
        ).start()

    @staticmethod
    def _apply_kconfig_to_sdkconfig(sdkconfig_path: Path, specs: List[ParamSpec], values: Dict[str, Any]) -> None:
        """Update CONFIG_DOORBELL_* entries in sdkconfig in-place for fast incremental compilation."""
        lines = sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        spec_by_key = {s.kconfig: s for s in specs}
        handled = set()
        new_lines: List[str] = []

        for line in lines:
            matched_key: Optional[str] = None
            if line.startswith("CONFIG_"):
                k = line.split("=", 1)[0][7:]
                if k in spec_by_key:
                    matched_key = k
            elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
                k = line[len("# CONFIG_") : -len(" is not set")]
                if k in spec_by_key:
                    matched_key = k

            if matched_key:
                spec = spec_by_key[matched_key]
                val = values[matched_key]
                handled.add(matched_key)
                if spec.kind == "bool":
                    if val:
                        new_lines.append(f"CONFIG_{matched_key}=y")
                    else:
                        new_lines.append(f"# CONFIG_{matched_key} is not set")
                elif spec.kind == "str":
                    escaped = escape_quoted(val)
                    new_lines.append(f'CONFIG_{matched_key}="{escaped}"')
                else:
                    new_lines.append(f"CONFIG_{matched_key}={val}")
            else:
                new_lines.append(line)

        for spec in specs:
            if spec.kconfig not in handled:
                val = values[spec.kconfig]
                if spec.kind == "bool":
                    if val:
                        new_lines.append(f"CONFIG_{spec.kconfig}=y")
                    else:
                        new_lines.append(f"# CONFIG_{spec.kconfig} is not set")
                elif spec.kind == "str":
                    escaped = escape_quoted(val)
                    new_lines.append(f'CONFIG_{spec.kconfig}="{escaped}"')
                else:
                    new_lines.append(f"CONFIG_{spec.kconfig}={val}")

        sdkconfig_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")

    def _build_idf_env(self) -> Dict[str, str]:
        env = os.environ.copy()
        idf_path = Path(env.get("IDF_PATH", str(IDF_PATH_DEFAULT)))
        env["IDF_PATH"] = str(idf_path)
        env["IDF_TOOLS_PATH"] = env.get("IDF_TOOLS_PATH", str(IDF_TOOLS_PATH_DEFAULT))
        env["IDF_PYTHON_ENV_PATH"] = str(IDF_VENV_PYTHON.parent.parent)
        env["PYTHONUTF8"] = "1"

        # Prepend ESP-IDF toolchains, cmake, ninja, ccache, and venv Scripts to PATH
        extra_paths: List[str] = [str(IDF_VENV_PYTHON.parent)]
        tools_root = Path(env["IDF_TOOLS_PATH"]) / "tools"
        if tools_root.is_dir():
            for bin_dir in tools_root.glob("*/*/bin"):
                extra_paths.append(str(bin_dir))
            for top_dir in tools_root.glob("*/*"):
                if (top_dir / "ninja.exe").is_file() or (top_dir / "ccache.exe").is_file():
                    extra_paths.append(str(top_dir))
        env["PATH"] = os.pathsep.join(extra_paths + [env.get("PATH", "")])
        return env

    def _worker_build_firmware(self, role: str, values: Dict[str, Any]) -> None:
        is_panel = role == "panel"
        fw_dir = PANEL_FW_DIR if is_panel else UNIT_FW_DIR
        specs = PANEL_PARAMS if is_panel else APARTMENT_PARAMS
        app_bin_name = "doorbell_panel.bin" if is_panel else "doorbell_unit.bin"
        prefix = "panel" if is_panel else "apartment"

        self.log(f"\n=== Building {prefix} firmware ===\n")
        env = self._build_idf_env()
        idf_py = Path(env["IDF_PATH"]) / "tools" / "idf.py"
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)

        try:
            sdkconfig_path = fw_dir / "sdkconfig"
            if not sdkconfig_path.is_file():
                self.log("[Build] Initializing sdkconfig via idf.py reconfigure...\n")
                self._run_logged_cmd([python_exe, str(idf_py), "reconfigure"], cwd=fw_dir, env=env)

            self._apply_kconfig_to_sdkconfig(sdkconfig_path, specs, values)
            self.log("[Build] Applied GUI Kconfig settings to sdkconfig. Running idf.py build...\n")

            rc = self._run_logged_cmd([python_exe, str(idf_py), "build"], cwd=fw_dir, env=env)
            if rc != 0:
                self.log(f"[Build] ERROR: idf.py build exited with code {rc}\n")
                return

            build_dir = fw_dir / "build"
            bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
            part_bin = build_dir / "partition_table" / "partition-table.bin"
            app_bin = build_dir / app_bin_name

            if not (bootloader_bin.is_file() and part_bin.is_file() and app_bin.is_file()):
                self.log("[Build] ERROR: Expected build artifacts not found.\n")
                return

            BUILDS_DIR.mkdir(parents=True, exist_ok=True)
            ts = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            out_name = f"{prefix}_{ts}.bin"
            out_path = BUILDS_DIR / out_name

            merge_cmd = [
                python_exe,
                "-m",
                "esptool",
                "--chip",
                "esp32",
                "merge-bin",
                "-o",
                str(out_path),
                "--flash-mode",
                "dio",
                "--flash-freq",
                "40m",
                "--flash-size",
                "2MB",
                "0x1000",
                str(bootloader_bin),
                "0x8000",
                str(part_bin),
                "0x10000",
                str(app_bin),
            ]
            rc = self._run_logged_cmd(merge_cmd, cwd=fw_dir, env=env)
            if rc != 0 or not out_path.is_file():
                self.log("[Build] ERROR: esptool merge-bin failed.\n")
                return

            self.log(f"[Build] SUCCESS -> flasher/builds/{out_name} ({out_path.stat().st_size} bytes)\n")

            def _on_done() -> None:
                self._update_last_build_label(out_name)
                for card in self._device_cards:
                    card.refresh_firmware_list()

            self.post_ui(_on_done)
        except Exception as exc:
            self.log(f"[Build] Exception: {exc}\n")
        finally:
            def _unlock() -> None:
                self._building = False
                self.btn_build.configure(state=tk.NORMAL)

            self.post_ui(_unlock)

    def _run_logged_cmd(
        self,
        cmd: List[str],
        cwd: Path,
        env: Optional[Dict[str, str]] = None,
        progress_cb: Optional[Callable[[float], None]] = None,
        progress_weights: Optional[List[int]] = None,
    ) -> int:
        """Run a command, stream its output to the log, and report write-flash progress.

        esptool 5.x prints one "Writing at 0x... [bar]  45.2% ..." line per update and restarts at 0%
        for every image; `progress_weights` (bytes per image, in write order) folds them into one
        monotonic 0-100 value.
        """
        weights = [max(1, w) for w in (progress_weights or [1])]
        total_weight = float(sum(weights))
        part_idx = 0
        last_part_pct = 0.0
        last_overall = 0.0
        creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=creationflags,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            self.log(line)
            if progress_cb:
                m_pct = _WRITE_PROGRESS_RE.search(line)
                if m_pct:
                    pct = max(0.0, min(100.0, float(m_pct.group(1))))
                    if pct + 5.0 < last_part_pct and part_idx < len(weights) - 1:
                        part_idx += 1  # percentage restarted: esptool moved on to the next image
                    last_part_pct = pct
                    done = sum(weights[:part_idx]) + weights[part_idx] * pct / 100.0
                    overall = min(100.0, 100.0 * done / total_weight)
                    if overall > last_overall:
                        last_overall = overall
                        progress_cb(overall)
        return proc.wait()

    # ---- Right Column: Port Detection & Device Operations ----

    def detect_ports(self) -> None:
        if self._detecting:
            return
        if any(c._busy for c in self._device_cards):
            messagebox.showwarning(
                "Device Operation in Progress",
                "Cannot re-detect COM ports while a device is flashing or busy. Please wait for the operation to finish.",
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

    def _worker_probe_ports(self, cards: List[DeviceCard]) -> None:
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

    def worker_flash_firmware(self, card: DeviceCard, fw_path: Path, layout: str) -> None:
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        self.log(f"\n[{port}] Flashing '{fw_path.name}' (layout={layout}, NVS 0x9000 preserved)...\n")

        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp = Path(tmpdir)
                if layout == "merged":
                    # Slice the single merged binary into (bootloader+partition_table @ 0x1000..0x9000) and
                    # (application @ 0x10000..end) so NVS at 0x9000..0xF000 is strictly untouched (I-13, I-14)!
                    raw = fw_path.read_bytes()
                    boot_part = raw[0x1000:0x9000]
                    app_part = raw[0x10000:]
                    boot_part_file = tmp / "boot_and_part.bin"
                    app_part_file = tmp / "app.bin"
                    boot_part_file.write_bytes(boot_part)
                    app_part_file.write_bytes(app_part)
                    weights = [len(boot_part), len(app_part)]

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
                        "--flash-mode",
                        "dio",
                        "--flash-size",
                        "2MB",
                        "--flash-freq",
                        "40m",
                        "0x1000",
                        str(boot_part_file),
                        "0x10000",
                        str(app_part_file),
                    ]
                else:
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
                        "--flash-mode",
                        "dio",
                        "--flash-size",
                        "2MB",
                        "--flash-freq",
                        "40m",
                        "0x10000",
                        str(fw_path),
                    ]
                    weights = [1]

                rc = self._run_logged_cmd(
                    cmd,
                    cwd=PROJECT_ROOT,
                    progress_cb=lambda pct: self.post_ui(lambda p=pct: card.set_status(f"Flashing {int(p)}%...", "#b36b00", p)),
                    progress_weights=weights,
                )
                if rc == 0:
                    self.log(f"[{port}] Firmware flash complete.\n")
                    self.post_ui(lambda: card.set_status("Flash OK", "#1b7e2b", 100.0))
                else:
                    self.log(f"[{port}] Firmware flash FAILED (code {rc}).\n")
                    self.post_ui(lambda: card.set_status("Flash failed", "#b33a3a", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Flash exception: {exc}\n")
            self.post_ui(lambda: card.set_status("Flash error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))

    def worker_flash_panel_nvs(
        self,
        card: DeviceCard,
        admin_code: str,
        user_code: str,
        wifi_ssid: str,
        wifi_pass: str,
    ) -> None:
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        idf_path = Path(os.environ.get("IDF_PATH", str(IDF_PATH_DEFAULT)))
        nvs_gen_script = idf_path / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"

        self.log(f"\n[{port}] Generating and flashing 24 KB factory NVS partition (0x9000)...\n")
        try:
            blob = pack_panel_cfg_blob(wifi_ssid, wifi_pass, admin_code, user_code)
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp = Path(tmpdir)
                csv_path = tmp / "nvs.csv"
                bin_path = tmp / "panel_nvs.bin"
                with open(csv_path, "w", newline="", encoding="utf-8") as f:
                    writer = csv.writer(f)
                    writer.writerow(["key", "type", "encoding", "value"])
                    writer.writerow(["doorbell", "namespace", "", ""])
                    writer.writerow(["panel_cfg", "data", "hex2bin", blob.hex()])

                gen_cmd = [python_exe, str(nvs_gen_script), "generate", str(csv_path), str(bin_path), "0x6000"]
                rc = self._run_logged_cmd(gen_cmd, cwd=PROJECT_ROOT)
                if rc != 0 or not bin_path.is_file():
                    self.log(f"[{port}] Failed to generate NVS partition image.\n")
                    self.post_ui(lambda: card.set_status("NVS gen failed", "#b33a3a", 0.0))
                    return

                flash_cmd = [
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
                    "0x9000",
                    str(bin_path),
                ]
                rc = self._run_logged_cmd(
                    flash_cmd,
                    cwd=PROJECT_ROOT,
                    progress_cb=lambda pct: self.post_ui(lambda p=pct: card.set_status(f"NVS {int(p)}%...", "#b36b00", p)),
                )
                if rc == 0:
                    ssid_info = f"SSID='{wifi_ssid}'" if wifi_ssid else "SSID=(none)"
                    # Never log admin_code, user_code, or wifi_pass in the shared log (I-25)
                    self.log(f"[{port}] Flashed factory NVS at 0x9000 ({ssid_info}, codes redacted per I-25).\n")
                    self.post_ui(lambda: card.set_status("Flash NVS OK", "#1b7e2b", 100.0))
                else:
                    self.log(f"[{port}] Flash NVS failed (code {rc}).\n")
                    self.post_ui(lambda: card.set_status("Flash NVS failed", "#b33a3a", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Flash NVS exception: {exc}\n")
            self.post_ui(lambda: card.set_status("NVS error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))

    def worker_erase_nvs_region(self, card: DeviceCard, done_status: str) -> None:
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        self.log(f"\n[{port}] Erasing NVS partition (0x9000, size 0x6000)...\n")
        try:
            cmd = [
                python_exe,
                "-m",
                "esptool",
                "--chip",
                "esp32",
                "-p",
                port,
                "--before",
                "default-reset",
                "--after",
                "hard-reset",
                "erase-region",
                "0x9000",
                "0x6000",
            ]
            rc = self._run_logged_cmd(cmd, cwd=PROJECT_ROOT)
            if rc == 0:
                self.log(f"[{port}] {done_status}.\n")
                self.post_ui(lambda: card.set_status(done_status, "#1b7e2b", 100.0))
            else:
                self.post_ui(lambda: card.set_status("Erase NVS failed", "#b33a3a", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Erase NVS error: {exc}\n")
            self.post_ui(lambda: card.set_status("Error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))

    def worker_erase_full_flash(self, card: DeviceCard) -> None:
        port = card.port
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        self.log(f"\n[{port}] Erasing entire 2 MB flash...\n")
        try:
            cmd = [
                python_exe,
                "-m",
                "esptool",
                "--chip",
                "esp32",
                "-p",
                port,
                "--before",
                "default-reset",
                "--after",
                "hard-reset",
                "erase-flash",
            ]
            rc = self._run_logged_cmd(cmd, cwd=PROJECT_ROOT)
            if rc == 0:
                self.log(f"[{port}] Full flash erase complete.\n")
                self.post_ui(lambda: card.set_status("Erase done", "#1b7e2b", 100.0))
            else:
                self.post_ui(lambda: card.set_status("Erase failed", "#b33a3a", 0.0))
        except Exception as exc:
            self.log(f"[{port}] Erase flash error: {exc}\n")
            self.post_ui(lambda: card.set_status("Error", "#b33a3a", 0.0))
        finally:
            self.post_ui(lambda: card.set_busy(False))

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


def main() -> int:
    root = tk.Tk()
    _app = DoorbellFlasherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

