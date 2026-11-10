"""Tk widgets of the flasher: tooltips, the serial monitor window and the per-device card."""

from __future__ import annotations

import codecs
import queue
import re
import threading
import time
import tkinter as tk
from collections.abc import Callable
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk
from typing import TYPE_CHECKING, Any

import serial
import serial.tools.list_ports

from flasher_config import BUILDS_DIR, PROJECT_ROOT, escape_quoted, strip_ansi
from flasher_images import BURN_CONFIRM_WORD, generate_random_digits, inspect_firmware_image, read_build_info

if TYPE_CHECKING:
    from flasher_gui import DoorbellFlasherApp


class ToolTip:
    """Lightweight hover tooltip for parameter labels."""

    def __init__(self, widget: tk.Widget, text: str) -> None:
        self.widget = widget
        self.text = text
        self.tip_win: tk.Toplevel | None = None
        widget.bind("<Enter>", self._show)
        widget.bind("<Leave>", self._hide)

    def _show(self, _event: tk.Event | None = None) -> None:
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

    def _hide(self, _event: tk.Event | None = None) -> None:
        if self.tip_win:
            self.tip_win.destroy()
            self.tip_win = None


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
        self._ser: serial.Serial | None = None
        self._rx_queue: queue.Queue[str] = queue.Queue()
        self._redact_lock = threading.Lock()
        self._redact_tokens: list[str] = []
        # While a doorbell_wifi command is in flight, the REPL echo (possibly redrawn char-by-char and
        # horizontally scrolled by linenoise) can carry fragments of secrets, so only known-safe lines pass.
        self._echo_mute_until = 0.0

        top_bar = ttk.Frame(self, padding=(8, 6))
        top_bar.pack(fill=tk.X)

        ttk.Button(top_bar, text="Status (doorbell_status)", command=self._send_status).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(top_bar, text="Reboot (RTS Reset)", command=self._reset_board).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(top_bar, text="Clear", command=self._clear).pack(side=tk.LEFT, padx=(0, 6))

        wifi_bar = ttk.LabelFrame(
            self, text="Authenticated Panel Wi-Fi Update (doorbell_wifi — redacted from logs)", padding=(8, 4)
        )
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
            kept: list[str] = []
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
        app: DoorbellFlasherApp,
        port: str,
        description: str,
    ) -> None:
        title = f"{description} ({port})" if description else port
        super().__init__(parent, text=title, padding=(10, 6))
        self.app = app
        self.port = port
        self.description = description
        self._busy = False
        self._monitor_win: SerialMonitorWindow | None = None

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
        self.fw_cb.bind("<<ComboboxSelected>>", lambda _e: self._sync_code_lengths_to_firmware())

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
            command=lambda: self.admin_code_var.set(generate_random_digits(self._required_code_lengths()[0])),
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
            command=lambda: self.user_code_var.set(generate_random_digits(self._required_code_lengths()[1])),
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

        self.btn_monitor = ttk.Button(row_btns, text="Monitor", width=9, command=self._on_monitor)
        self.btn_monitor.pack(side=tk.LEFT, padx=(0, 5))

        self.btn_scan = ttk.Button(row_btns, text="Scan", width=8, command=self._on_scan)
        self.btn_scan.pack(side=tk.LEFT)
        self._burned = False  # set after a confirmed Burn: this chip can never be flashed again
        self.fw_var.trace_add("write", lambda *_a: self._update_buttons_for_firmware())

        # 5. Progress bar & status row
        row_prog = ttk.Frame(self)
        row_prog.pack(fill=tk.X)

        self.progress_var = tk.DoubleVar(value=0.0)
        self.progressbar = ttk.Progressbar(row_prog, variable=self.progress_var, maximum=100.0, length=240)
        self.progressbar.pack(side=tk.LEFT, padx=(0, 10))

        self.status_lbl = tk.Label(row_prog, text="Detecting...", fg="#555555", font=("Segoe UI", 9))
        self.status_lbl.pack(side=tk.LEFT)

        self.refresh_firmware_list()

    def set_status(self, text: str, color: str = "#1b7e2b", progress: float | None = None) -> None:
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
            self.btn_monitor,
            self.btn_scan,
            self.browse_btn,
        ):
            btn.configure(state=state)
        if self._burned:
            self.btn_flash.configure(state=tk.DISABLED)
        self.role_cb.configure(state=tk.DISABLED if busy else "readonly")

    def _update_buttons_for_firmware(self) -> None:
        """Name the flash action after the selected image: "Burn" for a Burn image (or a burned chip)."""
        if not self.winfo_exists():
            return
        fw_path = self._resolve_fw_path()
        info = read_build_info(fw_path) if fw_path else None
        burn = bool(info and info.get("profile") == "burn") or self._burned
        self.btn_flash.configure(text="Burn" if burn else "Flash")

    def mark_burned(self) -> None:
        """After a confirmed Burn: the chip accepts no further flashing; Monitor and Scan stay available."""
        self._burned = True
        self._update_buttons_for_firmware()
        self.btn_flash.configure(state=tk.DISABLED)
        self.chip_info_var.set(self.chip_info_var.get() + " | BURNED (locked)")

    def _on_role_changed(self, _event: tk.Event | None = None) -> None:
        role = self.role_var.get()
        if role == "panel":
            self.panel_prov_frame.pack(fill=tk.X, pady=(0, 6), before=self.btn_flash.master)
        else:
            self.panel_prov_frame.pack_forget()
        self.refresh_firmware_list()

    def refresh_firmware_list(self) -> None:
        BUILDS_DIR.mkdir(parents=True, exist_ok=True)
        bins = sorted(BUILDS_DIR.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
        names = [b.name for b in bins if (read_build_info(b) or {}).get("profile") in ("production", "burn")]
        self.fw_cb["values"] = names

        role = self.role_var.get()
        prefix = "panel_" if role == "panel" else "apartment_"
        # Never auto-select an irreversible Burn image: it must be chosen explicitly in the list
        selected = next((n for n in names if n.startswith(prefix) and not n.endswith("_burn.bin")), None)
        fallback = next((n for n in names if not n.endswith("_burn.bin")), None)
        if selected:
            self.fw_var.set(selected)
        elif fallback and not self.fw_var.get():
            self.fw_var.set(fallback)
        self._sync_code_lengths_to_firmware()

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
            self._sync_code_lengths_to_firmware()

    def _resolve_fw_path(self) -> Path | None:
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

        info = read_build_info(fw_path)
        if info and info.get("profile") == "burn":
            self._start_burn(fw_path, info)
            return
        if fw_path.stem.endswith("_burn"):
            messagebox.showerror(
                "Burn Image Without Build Info",
                f"'{fw_path.name}' looks like a Burn image but its build-info file is missing. Nothing was flashed.",
            )
            return

        detected_role = inspect_firmware_image(fw_path)
        if detected_role is None:
            messagebox.showerror(
                "Unsupported Firmware Image",
                f"'{fw_path.name}' is not a complete flasher image (bootloader @0x1000, partition table @0x8000, "
                "app @0x10000). Flash erases the whole chip, so only complete images are accepted. Nothing was "
                "flashed.",
            )
            return
        selected_role = self.role_var.get()
        if detected_role in ("panel", "apartment") and detected_role != selected_role:
            messagebox.showerror(
                "Role Mismatch",
                f"'{fw_path.name}' is a '{detected_role}' image but this card is set to '{selected_role}'. "
                "Nothing was flashed.",
            )
            return

        # Flashing = installing from scratch: the chip is erased and gets firmware plus (panel) the codes and Wi-Fi
        # from this card. Those become the factory values the hidden reset button returns to.
        fields: tuple[str, str, str, str] | None = None
        if selected_role == "panel":
            fields = self._validated_panel_fields()
            if fields is None:
                return
            what = (
                f"the panel firmware with Admin code ({len(fields[0])} digits), User code ({len(fields[1])} digits) "
                "and "
                + (f"building Wi-Fi '{fields[2]}'" if fields[2] else "no building Wi-Fi (set it later with the portal)")
                + " from this card. These become the panel's factory values (label, hidden reset button)."
            )
        else:
            what = "the apartment-module firmware; the module starts unpaired."
        ok = messagebox.askyesno(
            "Erase and Flash?",
            f"The whole flash of {self.port} is erased first: all codes, Wi-Fi settings and pairings on it are "
            "lost.\n\n"
            f"Then it receives {what}\n\nContinue?",
        )
        if not ok:
            return

        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Flashing...", "#b36b00", 5.0)
        threading.Thread(
            target=self.app.worker_flash_full,
            args=(self, fw_path, fields),
            daemon=True,
        ).start()

    def _sync_code_lengths_to_firmware(self) -> None:
        """Regenerate a code whose length does not match the firmware now selected on this card."""
        if self.role_var.get() != "panel":
            return
        admin_len, user_len, _src = self._required_code_lengths()
        if len(self.admin_code_var.get().strip()) != admin_len:
            self.admin_code_var.set(generate_random_digits(admin_len))
        if len(self.user_code_var.get().strip()) != user_len:
            self.user_code_var.set(generate_random_digits(user_len))

    def _required_code_lengths(self) -> tuple[int, int, str]:
        """Admin/user code lengths of the firmware selected on this card (from its build-info sidecar).

        Falls back to the GUI configuration only when the selected image has no sidecar (e.g. browsed from
        elsewhere), and says so, because the codes must match the firmware actually running on the panel.
        """
        fw_path = self._resolve_fw_path()
        info = read_build_info(fw_path) if fw_path else None
        if info and info.get("role") == "panel":
            try:
                return int(info["admin_code_len"]), int(info["user_code_len"]), f"firmware {fw_path.name}"
            except (KeyError, TypeError, ValueError):
                self.app.log(f"[{self.port}] Ignoring malformed build info for {fw_path.name}.\n")
        return (
            self.app.get_configured_admin_len(),
            self.app.get_configured_user_len(),
            "current GUI settings (selected firmware has no build info - make sure it matches)",
        )

    def _validated_panel_fields(self) -> tuple[str, str, str, str] | None:
        """(admin, user, ssid, password) from this card, or None after showing why they are invalid."""
        admin_code = self.admin_code_var.get().strip()
        user_code = self.user_code_var.get().strip()
        ssid = self.wifi_ssid_var.get().strip()
        pw = self.wifi_pass_var.get()

        req_admin_len, req_user_len, len_source = self._required_code_lengths()

        if not admin_code.isdigit() or len(admin_code) != req_admin_len:
            messagebox.showerror(
                "Invalid Admin Code",
                f"Admin code must be exactly {req_admin_len} digits (0-9), as required by the {len_source}.",
            )
            return None
        if not user_code.isdigit() or len(user_code) != req_user_len:
            messagebox.showerror(
                "Invalid User Code",
                f"User code must be exactly {req_user_len} digits (0-9), as required by the {len_source}.",
            )
            return None
        if user_code == admin_code:
            messagebox.showerror("Invalid Codes", "User code and Admin code cannot be identical.")
            return None
        if "\x00" in ssid or "\x00" in pw:
            messagebox.showerror("Invalid Wi-Fi Credentials", "SSID and password cannot contain null bytes.")
            return None
        ssid_bytes = len(ssid.encode("utf-8"))
        pw_bytes = len(pw.encode("utf-8"))
        if ssid_bytes > 32 or (pw and (pw_bytes < 8 or pw_bytes > 63)):
            messagebox.showerror(
                "Invalid Wi-Fi Credentials",
                "Wi-Fi SSID must be 0-32 UTF-8 bytes, and password must be empty or 8-63 UTF-8 bytes.",
            )
            return None
        return admin_code, user_code, ssid, pw

    def _start_burn(self, fw_path: Path, info: dict[str, Any]) -> None:
        """Burn flow: one irreversible write of signed firmware (+ encrypted panel NVS), then the locking first boot."""
        role = self.role_var.get()
        if info.get("role") != role:
            messagebox.showerror(
                "Role Mismatch",
                f"'{fw_path.name}' is a '{info.get('role')}' Burn image but this card is set to '{role}'. "
                "Burning is irreversible, so a mismatch is never flashed.",
            )
            return
        fields: tuple[str, str, str, str] | None = None
        if role == "panel":
            fields = self._validated_panel_fields()
            if fields is None:
                return

        what = (
            "the signed panel firmware together with its encrypted NVS (the admin/user codes and optional Wi-Fi "
            "from this card)"
            if role == "panel"
            else "the signed apartment-module firmware (it generates its own "
            "NVS keys on first boot and starts unpaired)"
        )
        ok = messagebox.askyesno(
            "IRREVERSIBLE: Burn this chip?",
            f"Port {self.port} will receive {what}.\n\n"
            "On its first boot the chip permanently burns eFuses: Secure Boot v2, flash encryption (release) and "
            "disabled UART download mode. Afterwards it can NEVER be re-flashed, read out, erased or debugged; "
            "firmware bugs and compile-time settings can only be fixed by replacing the hardware.\n\n"
            + (
                "Print the label with these codes first: they cannot be read back from the chip.\n\n"
                if role == "panel"
                else ""
            )
            + "Do not unplug the board until the flasher reports the result (about one minute).\n\nContinue?",
            icon=messagebox.WARNING,
        )
        if not ok:
            return
        typed = simpledialog.askstring(
            "Final Confirmation",
            f"Type {BURN_CONFIRM_WORD} to burn the chip on {self.port}:",
            parent=self,
        )
        if (typed or "").strip() != BURN_CONFIRM_WORD:
            self.app.log(f"[{self.port}] Burn cancelled (confirmation word not entered).\n")
            return

        self._close_monitor_if_open()
        self.set_busy(True)
        self.set_status("Burn: checking eFuses...", "#b36b00", 2.0)
        threading.Thread(target=self.app.worker_burn_device, args=(self, fw_path, info, fields), daemon=True).start()

    def _on_monitor(self) -> None:
        if self._busy:
            return
        if self._monitor_win and self._monitor_win.winfo_exists():
            self._monitor_win.lift()
            return
        self._monitor_win = SerialMonitorWindow(self, self.port, self.app.log)

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
