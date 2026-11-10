"""Flasher settings model: project paths, ESP32 GPIO rules and the GUI form generated from Kconfig.projbuild."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

FLASHER_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = FLASHER_DIR.parent
BUILDS_DIR = FLASHER_DIR / "builds"
PANEL_FW_DIR = PROJECT_ROOT / "firmware" / "panel"
UNIT_FW_DIR = PROJECT_ROOT / "firmware" / "unit"
KCONFIG_PROJBUILD_PATH = PROJECT_ROOT / "components" / "doorbell_common" / "Kconfig.projbuild"

# Classic ESP32 GPIO rules (I-49)
ESP32_FLASH_GPIOS = {6, 7, 8, 9, 10, 11}
ESP32_INPUT_ONLY_GPIOS = {34, 35, 36, 37, 38, 39}
ESP32_VALID_GPIOS = set(range(0, 40)) - {20, 24, 28, 29, 30, 31} - ESP32_FLASH_GPIOS
ESP32_VALID_OUTPUT_GPIOS = ESP32_VALID_GPIOS - ESP32_INPUT_ONLY_GPIOS

# ANSI CSI escape sequences (colors, cursor moves, line erase) emitted by ESP_LOG and linenoise
_ANSI_CSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


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
    """One Kconfig option shown in the GUI form. Built from Kconfig.projbuild by load_param_specs(); only
    max_len and the GPIO direction come from the GUI-only tables below."""

    kconfig: str
    label: str
    section: str
    kind: str  # "int", "bool", "str"
    default: Any
    help_text: str
    min_val: int | None = None
    max_val: int | None = None
    max_len: int | None = None
    is_gpio_output: bool = False
    is_gpio_input: bool = False


# ---- GUI form from Kconfig.projbuild (single source of truth: label, menu, type, default, range, help) ----

# GUI-only metadata Kconfig cannot express
GPIO_DIRECTION = {
    "DOORBELL_PANEL_GPIO_ROW0": "out",
    "DOORBELL_PANEL_GPIO_ROW1": "out",
    "DOORBELL_PANEL_GPIO_ROW2": "out",
    "DOORBELL_PANEL_GPIO_ROW3": "out",
    "DOORBELL_PANEL_GPIO_DOOR_RELAY": "out",
    "DOORBELL_PANEL_GPIO_STATUS_LED": "out",
    "DOORBELL_PANEL_GPIO_COL0": "in",
    "DOORBELL_PANEL_GPIO_COL1": "in",
    "DOORBELL_PANEL_GPIO_COL2": "in",
    "DOORBELL_PANEL_GPIO_RESET_BTN": "in",
    "DOORBELL_UNIT_GPIO_BELL_RELAY": "out",
    "DOORBELL_UNIT_GPIO_STATUS_LED": "out",
    "DOORBELL_UNIT_GPIO_DOOR_BTN": "in",
    "DOORBELL_UNIT_GPIO_PAIR_BTN": "in",
    "DOORBELL_UNIT_GPIO_RESET_BTN": "in",
}
STRING_MAX_LEN = {"DOORBELL_PANEL_KEY_MAP": 12, "DOORBELL_WIFI_COUNTRY_CODE": 2}
# Not in the form: the role choice, the blank-NVS fallback codes (derived per build from the code lengths) and the
# fallback Wi-Fi (Flash writes the card's Wi-Fi into NVS)
HIDDEN_KCONFIG = {
    "DOORBELL_PANEL_DEFAULT_ADMIN_CODE",
    "DOORBELL_PANEL_DEFAULT_USER_CODE",
    "DOORBELL_PANEL_DEFAULT_WIFI_SSID",
    "DOORBELL_PANEL_DEFAULT_WIFI_PASS",
}


def load_param_specs(kconfig_path: Path, role_symbol: str) -> list[ParamSpec]:
    """Form parameters of one role, read with ESP-IDF's Kconfig library in menu order.

    Two evaluations: `kconf` with only the role selected gives the real values; `opts_on` additionally switches on
    every ordinary bool option, so an option that depends on another option (e.g. the country code on "set country")
    is still listed with its own default and range, while options that depend on the other role drop out.
    """
    from esp_kconfiglib import core as kconf_lib

    def load(all_bools_on: bool) -> Any:
        k = kconf_lib.Kconfig(str(kconfig_path), warn=False)
        k.syms[role_symbol].set_value("y")
        if all_bools_on:
            for sym in k.unique_defined_syms:
                if sym.type == kconf_lib.BOOL and sym.choice is None:
                    sym.set_value("y")
        return k

    kconf, opts_on = load(False), load(True)
    kinds = {kconf_lib.BOOL: "bool", kconf_lib.INT: "int", kconf_lib.HEX: "int", kconf_lib.STRING: "str"}
    specs: list[ParamSpec] = []

    def first_active(pairs: list) -> Any:
        return next((pair for pair in pairs if kconf_lib.expr_value(pair[-1])), None)

    def walk(node: Any, section: str) -> None:
        while node is not None:
            item = node.item
            if item == kconf_lib.MENU:
                if node.list is not None and kconf_lib.expr_value(node.dep):
                    walk(node.list, node.prompt[0])
            elif (
                isinstance(item, kconf_lib.Symbol)
                and node.prompt
                and item.name not in HIDDEN_KCONFIG
                and opts_on.syms[item.name].visibility
            ):
                kind = kinds[item.type]
                on = opts_on.syms[item.name]
                raw = item.str_value if item.visibility else on.str_value
                value: Any = raw == "y" if kind == "bool" else int(raw, 0) if kind == "int" else raw
                rng = first_active(on.ranges)
                direction = GPIO_DIRECTION.get(item.name)
                specs.append(
                    ParamSpec(
                        kconfig=item.name,
                        label=node.prompt[0],
                        section=section,
                        kind=kind,
                        default=value,
                        help_text=" ".join((node.help or "").split()),
                        min_val=int(rng[0].str_value, 0) if rng else None,
                        max_val=int(rng[1].str_value, 0) if rng else None,
                        max_len=STRING_MAX_LEN.get(item.name),
                        is_gpio_output=direction == "out",
                        is_gpio_input=direction == "in",
                    )
                )
                if node.list is not None:  # options that depend on this one are nested below it
                    walk(node.list, section)
            node = node.next

    walk(kconf.top_node.list, "")
    return specs


# Kconfig settings that must match between the panel and apartment builds for the two to interoperate
INTEROP_KCONFIG_KEYS = (
    "DOORBELL_UDP_PORT",
    "DOORBELL_ACK_TIMEOUT_MS",
    "DOORBELL_PAIRING_COMPLETE_TIMEOUT_S",
    "DOORBELL_HEARTBEAT_ENABLED",
)

PANEL_PARAMS = load_param_specs(KCONFIG_PROJBUILD_PATH, "DOORBELL_ROLE_PANEL")
APARTMENT_PARAMS = load_param_specs(KCONFIG_PROJBUILD_PATH, "DOORBELL_ROLE_UNIT")
