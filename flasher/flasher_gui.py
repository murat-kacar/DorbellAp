#!/usr/bin/env python3
"""
DorbellAp — Customization, Build, Detection, and Flashing GUI.

Provides a two-column desktop interface:
- Left column ("Customization and build"):
  1. Profile selection: Production (-Os, unlocked) or Burn (-Os + Secure Boot v2 + release
     flash encryption + NVS encryption, irreversible per chip - ADR 0015)
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
    and action buttons: Flash (erases the chip, then writes firmware + the card's panel codes / Wi-Fi), Monitor,
    Scan.

This module is the entry point and holds the window, the settings form and its validation. The build pipeline
(flasher_build.BuildMixin) and the device operations (flasher_device.DeviceMixin) are mixed into the app class;
flasher_config, flasher_images, flasher_widgets and flasher_env hold the rest.
"""

from __future__ import annotations

import queue
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from flasher_env import ensure_idf_python

ensure_idf_python()

# After ensure_idf_python(), which may re-launch this script in the ESP-IDF Python environment
import tkinter as tk
from tkinter import messagebox, ttk

from flasher_build import BuildMixin
from flasher_config import (
    APARTMENT_PARAMS,
    BUILDS_DIR,
    ESP32_INPUT_ONLY_GPIOS,
    ESP32_VALID_GPIOS,
    ESP32_VALID_OUTPUT_GPIOS,
    INTEROP_KCONFIG_KEYS,
    PANEL_FW_DIR,
    PANEL_PARAMS,
    UNIT_FW_DIR,
    ParamSpec,
    unescape_quoted,
)
from flasher_device import DeviceMixin
from flasher_widgets import DeviceCard, ToolTip


class DoorbellFlasherApp(BuildMixin, DeviceMixin):
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
        self._device_cards: list[DeviceCard] = []
        self._idf_export_vars: dict[str, str] | None = None  # ESP-IDF activation env, queried on first build

        # Store Tkinter variables for Panel and Apartment Kconfig parameters
        self.panel_vars: dict[str, tk.Variable] = {}
        self.unit_vars: dict[str, tk.Variable] = {}
        self._init_param_vars()

        self._build_layout()
        self._populate_config_panel()
        self._update_last_build_label()

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
        specs: list[ParamSpec],
        vars_map: dict[str, tk.Variable],
    ) -> None:
        if not sdkconfig_path.is_file():
            return
        try:
            lines = sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            return
        kv: dict[str, str] = {}
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

        self.profile_var = tk.StringVar(value="prod")
        ttk.Radiobutton(
            prof_box,
            text="Production (release build: -Os, unlocked)",
            variable=self.profile_var,
            value="prod",
        ).pack(anchor=tk.W)
        ttk.Radiobutton(
            prof_box,
            text="Burn (Production + Secure Boot v2 + flash/NVS encryption - IRREVERSIBLE)",
            variable=self.profile_var,
            value="burn",
        ).pack(anchor=tk.W, pady=(2, 2))
        self.profile_hint_lbl = tk.Label(prof_box, fg="#666666", font=("Segoe UI", 8), justify=tk.LEFT)
        self.profile_hint_lbl.pack(anchor=tk.W)

        self.profile_var.trace_add("write", lambda *_a: self._on_profile_changed())
        self._on_profile_changed()

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

        build_row = ttk.Frame(build_box)
        build_row.pack(fill=tk.X)
        self.btn_build = ttk.Button(build_row, text="Build firmware", width=15, command=self._on_build_clicked)
        self.btn_build.pack(side=tk.LEFT, padx=(0, 10))

        self.last_build_var = tk.StringVar(value="Last build: (none)")
        ttk.Label(build_row, textvariable=self.last_build_var, font=("Segoe UI", 9)).pack(side=tk.LEFT)

        # Build progress: ninja's "[n/total]" steps drive 0-90 %, signing / merging the rest
        progress_row = ttk.Frame(build_box)
        progress_row.pack(fill=tk.X, pady=(6, 0))
        self.build_progress_var = tk.DoubleVar(value=0.0)
        ttk.Progressbar(progress_row, variable=self.build_progress_var, maximum=100.0).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 10)
        )
        self.build_status_lbl = tk.Label(
            progress_row, text="Idle", fg="#555555", font=("Segoe UI", 9), width=26, anchor="w"
        )
        self.build_status_lbl.pack(side=tk.LEFT)

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

    def _validate_config(self, role: str) -> dict[str, Any] | None:
        """Validate all parameters and cross-parameter rules before invoking idf.py build."""
        is_panel = role == "panel"
        specs = PANEL_PARAMS if is_panel else APARTMENT_PARAMS
        vars_map = self.panel_vars if is_panel else self.unit_vars

        parsed: dict[str, Any] = {}
        used_gpios: dict[int, str] = {}

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
                        f"'{spec.label}' (GPIO {iv}) is not a valid GPIO on classic ESP32.\nAvoid flash pins 6-11.",
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

        # Settings both ends of the link depend on must be identical in the panel and apartment builds: a mismatched
        # UDP port or ACK/pairing timing breaks communication, and heartbeats disabled only on the modules would make
        # the panel's silence restart (I-52) reboot it periodically.
        other_vars = self.unit_vars if is_panel else self.panel_vars
        other_name = "Apartment" if is_panel else "Panel"
        for key in INTEROP_KCONFIG_KEYS:
            other_raw = other_vars[key].get()
            other_val = bool(other_raw) if isinstance(parsed[key], bool) else str(other_raw).strip()
            this_val = parsed[key] if isinstance(parsed[key], bool) else str(parsed[key])
            if other_val != this_val:
                label = next(s.label for s in specs if s.kconfig == key)
                messagebox.showerror(
                    "Panel / Apartment Mismatch",
                    f"'{label}' must be identical for panel and apartment builds.\n"
                    f"This role: {this_val}   {other_name}: {other_val}\n\n"
                    f"Set the same value in both role tabs (and rebuild the other role if it was already flashed).",
                )
                return None

        # Cross-parameter checks (matching doorbell_config_checks.h, I-66)
        pair_need_ms = 4 * parsed["DOORBELL_ACK_TIMEOUT_MS"] + 500 + 5000
        if parsed["DOORBELL_PAIRING_COMPLETE_TIMEOUT_S"] * 1000 < pair_need_ms:
            messagebox.showerror(
                "Validation Error",
                f"Pairing LAN timeout must be at least {(pair_need_ms + 999) // 1000} s with an ACK timeout of "
                f"{parsed['DOORBELL_ACK_TIMEOUT_MS']} ms (5 s LAN join + 4 x ACK timeout + 0.5 s).",
            )
            return None
        if parsed["DOORBELL_LED_ERR_DURATION_MS"] < parsed["DOORBELL_LED_FAST_PERIOD_MS"]:
            messagebox.showerror("Validation Error", "Error duration must be at least one fast blink period.")
            return None
        if parsed["DOORBELL_LED_NO_RESP_DURATION_MS"] < parsed["DOORBELL_LED_MEDIUM_PERIOD_MS"]:
            messagebox.showerror("Validation Error", "No-response duration must be at least one medium blink period.")
            return None
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
                    f"pull resistor. Either disable "
                    f"'{next(s.label for s in specs if s.kconfig == 'DOORBELL_PANEL_RESET_BTN_INTERNAL_PULL')}' "
                    "(with an external resistor) or choose another GPIO.",
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

    def _update_last_build_label(self, filename: str | None = None) -> None:
        if filename:
            self.last_build_var.set(f"Last build: {filename}")
            return
        bins = sorted(BUILDS_DIR.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
        if bins:
            self.last_build_var.set(f"Last build: {bins[0].name}")
        else:
            self.last_build_var.set("Last build: (none)")


def main() -> int:
    root = tk.Tk()
    _app = DoorbellFlasherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
