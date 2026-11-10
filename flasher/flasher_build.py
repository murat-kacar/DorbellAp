"""Build pipeline of the flasher app: sdkconfig generation, ESP-IDF environment, idf.py build and merge-bin."""

from __future__ import annotations

import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from collections.abc import Callable
from pathlib import Path
from typing import Any

from flasher_config import (
    APARTMENT_PARAMS,
    BUILDS_DIR,
    PANEL_FW_DIR,
    PANEL_PARAMS,
    UNIT_FW_DIR,
    ParamSpec,
    escape_quoted,
)
from flasher_env import IDF_PATH_DEFAULT, IDF_TOOLS_PATH_DEFAULT, IDF_VENV_PYTHON
from flasher_images import (
    BURN_OVERLAY_PATH,
    apply_sdkconfig_overlay,
    build_info_path,
    burn_layout_from_partition_table,
    check_sdkconfig_overlay,
    parse_partition_table,
    read_sdkconfig_overlay,
)

# ninja step line printed by idf.py build: "[123/1073] Building C object ..."
_NINJA_PROGRESS_RE = re.compile(r"^\[(\d+)/(\d+)\]")

# esptool 5.x write-flash progress line: "Writing at 0x00012345 [=====>    ]  45.2% ..."
_WRITE_PROGRESS_RE = re.compile(r"Writing at 0x[0-9a-fA-F]+\b.*?(\d+(?:\.\d+)?)\s*%")


class BuildMixin:
    """Build methods of DoorbellFlasherApp (state and widgets live on the app)."""

    def _on_build_clicked(self) -> None:
        if self._building:
            return
        role = self.build_role_var.get()
        validated = self._validate_config(role)
        if validated is None:
            return

        profile = self.profile_var.get()
        self._building = True
        self.btn_build.configure(state=tk.DISABLED)
        threading.Thread(
            target=self._worker_build_firmware,
            args=(role, validated, profile),
            daemon=True,
        ).start()

    def _on_profile_changed(self) -> None:
        hints = {
            "prod": "Release build (-Os). No eFuses are burned; boards stay re-flashable (bench-test the release "
            "here).",
            "burn": "Release build, signed with a one-time key (nothing to store). Flashing it burns eFuses on first "
            "boot:\n"
            "the chip can never be re-flashed, read out or debugged again. Codes and Wi-Fi are written with the "
            "firmware.",
        }
        profile = self.profile_var.get()
        self.profile_hint_lbl.configure(text=hints.get(profile, ""), fg="#b33a3a" if profile == "burn" else "#666666")

    @staticmethod
    def _apply_kconfig_to_sdkconfig(sdkconfig_path: Path, specs: list[ParamSpec], values: dict[str, Any]) -> None:
        """Update CONFIG_DOORBELL_* entries in sdkconfig in-place for fast incremental compilation."""
        lines = sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        spec_by_key = {s.kconfig: s for s in specs}
        handled = set()
        new_lines: list[str] = []

        for line in lines:
            matched_key: str | None = None
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

    @staticmethod
    def _parse_key_value_env(text: str) -> dict[str, str]:
        result: dict[str, str] = {}
        for line in text.splitlines():
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", line.strip())
            if m:
                result[m.group(1)] = m.group(2)
        return result

    def _query_idf_export_vars(self) -> dict[str, str]:
        """Ask the installed ESP-IDF for its activation environment instead of guessing tool paths.

        1. EIM install: `<IDF_TOOLS_PATH>/Microsoft.v6.1.PowerShell_profile.ps1 -e` prints KEY=VALUE pairs.
        2. Classic install: `idf_tools.py export --format key-value`, plus ESP_IDF_VERSION from
           tools/cmake/version.cmake (what export.ps1 / activate.py set).
        """
        creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        tools_path = Path(os.environ.get("IDF_TOOLS_PATH", str(IDF_TOOLS_PATH_DEFAULT)))
        idf_path = Path(os.environ.get("IDF_PATH", str(IDF_PATH_DEFAULT)))

        eim_profile = tools_path / "Microsoft.v6.1.PowerShell_profile.ps1"
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if eim_profile.is_file() and shell:
            proc = subprocess.run(
                [shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(eim_profile), "-e"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
                creationflags=creationflags,
            )
            exported = self._parse_key_value_env(proc.stdout)
            if proc.returncode == 0 and exported.get("ESP_IDF_VERSION") and exported.get("PATH"):
                exported.pop("SYSTEM_PATH", None)  # EIM's snapshot of the system PATH; keep the live one
                return exported
            self.log(f"[Build] EIM profile export failed (code {proc.returncode}); trying idf_tools.py export.\n")

        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        base_env = dict(os.environ, IDF_PATH=str(idf_path), IDF_TOOLS_PATH=str(tools_path))
        proc = subprocess.run(
            [python_exe, str(idf_path / "tools" / "idf_tools.py"), "export", "--format", "key-value"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
            env=base_env,
            creationflags=creationflags,
        )
        exported = self._parse_key_value_env(proc.stdout)
        if proc.returncode != 0 or not exported.get("PATH"):
            raise RuntimeError(
                "Cannot obtain the ESP-IDF environment (no EIM profile and idf_tools.py export failed):\n"
                + (proc.stdout + proc.stderr).strip()
            )
        # key-value PATH ends with a reference to the caller's PATH; it is re-appended below
        exported["PATH"] = re.sub(r"[;:]?(%PATH%|\$PATH)$", "", exported["PATH"])
        version_txt = (idf_path / "tools" / "cmake" / "version.cmake").read_text(encoding="utf-8")
        ver = {k: v for k, v in re.findall(r"set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\)", version_txt)}
        exported.setdefault("IDF_PATH", str(idf_path))
        exported["ESP_IDF_VERSION"] = f"{ver['MAJOR']}.{ver['MINOR']}"
        exported["IDF_VERSION"] = f"{ver['MAJOR']}.{ver['MINOR']}.{ver.get('PATCH', '0')}"
        return exported

    def _build_idf_env(self) -> dict[str, str]:
        if self._idf_export_vars is None:
            self._idf_export_vars = self._query_idf_export_vars()
        exported = dict(self._idf_export_vars)
        env = os.environ.copy()
        tool_paths = exported.pop("PATH")
        env.update(exported)
        env.setdefault("IDF_PYTHON_ENV_PATH", str(IDF_VENV_PYTHON.parent.parent))
        env["PYTHONUTF8"] = "1"
        env["PATH"] = os.pathsep.join([tool_paths, str(IDF_VENV_PYTHON.parent), env.get("PATH", "")])
        return env

    @staticmethod
    def _apply_build_profile(sdkconfig_path: Path) -> None:
        """Force -Os (CONFIG_COMPILER_OPTIMIZATION_SIZE, the project default in sdkconfig.defaults) in an sdkconfig
        that an older flasher or a manual menuconfig left on another optimization level."""
        chosen = "SIZE"
        # Deprecated aliases kconfgen keeps in sdkconfig: LEVEL_RELEASE -> SIZE, LEVEL_DEBUG / DEFAULT -> DEBUG
        aliases = {"LEVEL_RELEASE": "SIZE", "LEVEL_DEBUG": "DEBUG", "DEFAULT": "DEBUG"}
        out: list[str] = []
        for line in sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines():
            m = re.match(r"^(?:# )?CONFIG_COMPILER_OPTIMIZATION_([A-Z_]+)(?:=y| is not set)$", line)
            if m and m.group(1) in ("DEBUG", "SIZE", "PERF", "NONE", *aliases):
                name = m.group(1)
                on = aliases.get(name, name) == chosen
                line = (
                    f"CONFIG_COMPILER_OPTIMIZATION_{name}=y"
                    if on
                    else f"# CONFIG_COMPILER_OPTIMIZATION_{name} is not set"
                )
            out.append(line)
        sdkconfig_path.write_text("\n".join(out) + "\n", encoding="utf-8")

    def _worker_build_firmware(self, role: str, values: dict[str, Any], profile: str = "prod") -> None:
        is_panel = role == "panel"
        fw_dir = PANEL_FW_DIR if is_panel else UNIT_FW_DIR
        specs = PANEL_PARAMS if is_panel else APARTMENT_PARAMS
        app_bin_name = "doorbell_panel.bin" if is_panel else "doorbell_unit.bin"
        prefix = "panel" if is_panel else "apartment"
        burn = profile == "burn"

        profile_name = {"prod": "Production (-Os)", "burn": "Burn (-Os, Secure Boot v2 + encryption)"}[profile]
        self.log(f"\n=== Building {prefix} firmware - {profile_name} ===\n")
        python_exe = str(IDF_VENV_PYTHON if IDF_VENV_PYTHON.is_file() else sys.executable)
        succeeded = False
        progress = {"pct": 0.0, "total": 0}

        def set_progress(text: str, pct: float | None = None, color: str = "#b36b00") -> None:
            if pct is not None:
                progress["pct"] = pct
            value = progress["pct"]
            self.post_ui(
                lambda: (self.build_progress_var.set(value), self.build_status_lbl.configure(text=text, fg=color))
            )

        def on_ninja_step(done: int, total: int) -> None:
            # The bootloader sub-build prints its own, smaller [n/total] counter inside the app build: follow only the
            # largest total seen, and never move the bar backwards
            if total < progress["total"]:
                return
            progress["total"] = total
            pct = 90.0 * done / max(total, 1)
            if pct > progress["pct"]:
                set_progress(f"Compiling {int(pct / 0.9)}% ({done}/{total})", pct)

        set_progress("Preparing...", 0.0)
        try:
            # Inside try: an environment failure must be logged and must unlock the Build button
            env = self._build_idf_env()
            idf_py = Path(env["IDF_PATH"]) / "tools" / "idf.py"
            sdkconfig_path = fw_dir / "sdkconfig"
            if not sdkconfig_path.is_file():
                self.log("[Build] Initializing sdkconfig via idf.py reconfigure...\n")
                self._run_logged_cmd([python_exe, str(idf_py), "reconfigure"], cwd=fw_dir, env=env)

            apply_specs, apply_values = list(specs), dict(values)
            if is_panel:
                # The blank-NVS fallback codes must match the configured lengths (compile-time check in
                # doorbell_config_checks.h); they are bench-only placeholders, real panels get unique codes from
                # the card fields on Flash.
                # Different digit strings so admin and user placeholders never coincide (e.g. both 8 digits).
                for key, length, digits in (
                    ("DOORBELL_PANEL_DEFAULT_ADMIN_CODE", values["DOORBELL_PANEL_ADMIN_CODE_LEN"], "1234567890"),
                    ("DOORBELL_PANEL_DEFAULT_USER_CODE", values["DOORBELL_PANEL_USER_CODE_LEN"], "12340987"),
                ):
                    apply_specs.append(ParamSpec(key, key, "", "str", "", ""))
                    apply_values[key] = digits[: int(length)]
            self._apply_kconfig_to_sdkconfig(sdkconfig_path, apply_specs, apply_values)
            if burn:
                # Separate tree and sdkconfig: the Production build state is never touched
                build_dir = fw_dir / "build_burn"
                build_dir.mkdir(exist_ok=True)
                burn_sdkconfig = build_dir / "sdkconfig"
                shutil.copyfile(sdkconfig_path, burn_sdkconfig)
                overlay = read_sdkconfig_overlay(BURN_OVERLAY_PATH)
                apply_sdkconfig_overlay(burn_sdkconfig, overlay)
                self._apply_build_profile(burn_sdkconfig)
                build_cmd = [
                    python_exe,
                    str(idf_py),
                    "-B",
                    str(build_dir),
                    "-D",
                    f"SDKCONFIG={burn_sdkconfig}",
                    "build",
                ]
                self.log(
                    f"[Build] Applied GUI settings + {BURN_OVERLAY_PATH.name} to build_burn/sdkconfig. Running idf.py "
                    "build...\n"
                )
            else:
                build_dir = fw_dir / "build"
                self._apply_build_profile(sdkconfig_path)
                build_cmd = [python_exe, str(idf_py), "build"]
                self.log("[Build] Applied GUI Kconfig settings to sdkconfig. Running idf.py build...\n")

            set_progress("Configuring...", 1.0)
            rc = self._run_logged_cmd(build_cmd, cwd=fw_dir, env=env, ninja_cb=on_ninja_step)
            if rc != 0:
                self.log(f"[Build] ERROR: idf.py build exited with code {rc}\n")
                return
            set_progress("Signing and merging..." if burn else "Merging image...", 92.0)

            bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
            part_bin = build_dir / "partition_table" / "partition-table.bin"
            app_bin = build_dir / app_bin_name

            if not (bootloader_bin.is_file() and part_bin.is_file() and app_bin.is_file()):
                self.log("[Build] ERROR: Expected build artifacts not found.\n")
                return

            BUILDS_DIR.mkdir(parents=True, exist_ok=True)
            ts = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            suffix = {"prod": "_prod", "burn": "_burn"}[profile]
            out_name = f"{prefix}_{ts}{suffix}.bin"
            out_path = BUILDS_DIR / out_name

            def _on_done() -> None:
                self._update_last_build_label(out_name)
                for card in self._device_cards:
                    card.refresh_firmware_list()

            if burn:
                # One-time signing key: created for this build, used to sign it, then destroyed. Burned chips keep
                # only its public-key digest and can never be re-flashed, so the key has no later use (ADR 0015).
                with tempfile.TemporaryDirectory(prefix="dorbellap_sbv2_") as key_dir:
                    signing_key = Path(key_dir) / "build_key.pem"
                    rc = self._run_logged_cmd(
                        [
                            python_exe,
                            "-m",
                            "espsecure",
                            "generate-signing-key",
                            "--version",
                            "2",
                            "--scheme",
                            "rsa3072",
                            str(signing_key),
                        ],
                        cwd=fw_dir,
                        env=env,
                    )
                    if rc != 0 or not signing_key.is_file():
                        self.log("[Build] ERROR: could not create the one-time signing key.\n")
                        return
                    try:
                        burn_info = self._finish_burn_build(
                            build_dir,
                            overlay,
                            bootloader_bin,
                            part_bin,
                            app_bin,
                            signing_key,
                            out_path,
                            python_exe,
                            fw_dir,
                            env,
                        )
                    finally:
                        signing_key.write_bytes(b"\0" * signing_key.stat().st_size)  # best effort before deletion
                self.log("[Build] One-time signing key destroyed.\n")
                if burn_info is None:
                    return
                info: dict[str, Any] = {"role": prefix, "profile": "burn", **burn_info}
                if is_panel:
                    info["admin_code_len"] = values["DOORBELL_PANEL_ADMIN_CODE_LEN"]
                    info["user_code_len"] = values["DOORBELL_PANEL_USER_CODE_LEN"]
                build_info_path(out_path).write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
                self.log(f"[Build] SUCCESS -> flasher/builds/{out_name} ({out_path.stat().st_size} bytes, signed)\n")
                self.post_ui(_on_done)
                succeeded = True
                return

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

            info = {"role": prefix, "profile": "production"}
            if is_panel:
                info["admin_code_len"] = values["DOORBELL_PANEL_ADMIN_CODE_LEN"]
                info["user_code_len"] = values["DOORBELL_PANEL_USER_CODE_LEN"]
            build_info_path(out_path).write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
            self.log(f"[Build] SUCCESS -> flasher/builds/{out_name} ({out_path.stat().st_size} bytes)\n")
            self.post_ui(_on_done)
            succeeded = True
        except Exception as exc:
            self.log(f"[Build] Exception: {exc}\n")
        finally:
            if succeeded:
                set_progress("Build complete", 100.0, "#1b7e2b")
            else:
                set_progress("Build failed (see log)", None, "#b33a3a")

            def _unlock() -> None:
                self._building = False
                self.btn_build.configure(state=tk.NORMAL)

            self.post_ui(_unlock)

    def _finish_burn_build(
        self,
        build_dir: Path,
        overlay: dict[str, str],
        bootloader_bin: Path,
        part_bin: Path,
        app_bin: Path,
        signing_key: Path,
        out_path: Path,
        python_exe: str,
        fw_dir: Path,
        env: dict[str, str],
    ) -> dict[str, Any] | None:
        """Check the resolved Burn config and layout, sign bootloader + app with the one-time key, and merge.

        Returns the layout facts for the build-info sidecar, or None after logging the reason.
        """
        sdk_lines = (build_dir / "sdkconfig").read_text(encoding="utf-8", errors="ignore").splitlines()
        problems = check_sdkconfig_overlay(build_dir / "sdkconfig", overlay)
        if "CONFIG_COMPILER_OPTIMIZATION_SIZE=y" not in sdk_lines:
            problems.append("expected 'CONFIG_COMPILER_OPTIMIZATION_SIZE=y'")
        if problems:
            self.log("[Build] ERROR: the resolved Burn configuration differs from firmware/sdkconfig.burn:\n")
            for p in problems:
                self.log(f"         - {p}\n")
            return None

        pt_offset = int(overlay["CONFIG_PARTITION_TABLE_OFFSET"].split("=", 1)[1], 0)
        part_data = part_bin.read_bytes()
        try:
            layout = burn_layout_from_partition_table(parse_partition_table(part_data))
        except ValueError as exc:
            self.log(f"[Build] ERROR: Burn partition table: {exc}\n")
            return None

        signed_dir = build_dir / "signed"
        signed_dir.mkdir(exist_ok=True)
        signed_boot = signed_dir / "bootloader-signed.bin"
        signed_app = signed_dir / "app-signed.bin"
        espsecure = [python_exe, "-m", "espsecure"]
        self.log("[Build] Signing bootloader and app with the one-time Secure Boot v2 key\n")
        for src, dst in ((bootloader_bin, signed_boot), (app_bin, signed_app)):
            dst.unlink(missing_ok=True)
            rc = self._run_logged_cmd(
                espsecure
                + ["sign-data", "--version", "2", "--keyfile", str(signing_key), "--output", str(dst), str(src)],
                cwd=fw_dir,
                env=env,
            )
            if rc != 0 or not dst.is_file():
                self.log(f"[Build] ERROR: espsecure sign-data failed for {src.name}.\n")
                return None
            rc = self._run_logged_cmd(
                espsecure + ["verify-signature", "--version", "2", "--keyfile", str(signing_key), str(dst)],
                cwd=fw_dir,
                env=env,
            )
            if rc != 0:
                self.log(f"[Build] ERROR: signature verification failed for {dst.name}.\n")
                return None

        digest_file = signed_dir / "secure_boot_key_digest.bin"
        digest_file.unlink(missing_ok=True)
        rc = self._run_logged_cmd(
            espsecure + ["digest-sbv2-public-key", "--keyfile", str(signing_key), "--output", str(digest_file)],
            cwd=fw_dir,
            env=env,
        )
        if rc != 0 or not digest_file.is_file():
            self.log("[Build] ERROR: cannot compute the public key digest.\n")
            return None
        key_digest = digest_file.read_bytes().hex()

        boot_size, app_size = signed_boot.stat().st_size, signed_app.stat().st_size
        boot_end = 0x1000 + boot_size
        if boot_end > pt_offset:
            self.log(
                f"[Build] ERROR: signed bootloader ends at 0x{boot_end:x}, past the partition table at "
                f"0x{pt_offset:x}. "
                "Raise CONFIG_PARTITION_TABLE_OFFSET in firmware/sdkconfig.burn and move partitions_burn.csv.\n"
            )
            return None
        if app_size > layout["app_size"]:
            self.log(
                f"[Build] ERROR: signed app ({app_size} B) does not fit the factory partition ({layout['app_size']} "
                "B).\n"
            )
            return None
        self.log(
            f"[Build] Signed bootloader 0x{boot_size:x} B (0x{pt_offset - boot_end:x} B free before the partition "
            "table); "
            f"signed app {app_size} B ({100 * (layout['app_size'] - app_size) // layout['app_size']} % of the factory "
            "slot free)\n"
        )

        # "keep": rewriting the flash parameters in a signed image header would invalidate its signature
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
            "keep",
            "--flash-freq",
            "keep",
            "--flash-size",
            "keep",
            "0x1000",
            str(signed_boot),
            hex(pt_offset),
            str(part_bin),
            hex(layout["app_offset"]),
            str(signed_app),
        ]
        rc = self._run_logged_cmd(merge_cmd, cwd=fw_dir, env=env)
        if rc != 0 or not out_path.is_file():
            self.log("[Build] ERROR: esptool merge-bin failed.\n")
            return None
        merged = out_path.read_bytes()
        for offset, part in (
            (0x1000, signed_boot.read_bytes()),
            (pt_offset, part_data),
            (layout["app_offset"], signed_app.read_bytes()),
        ):
            if merged[offset : offset + len(part)] != part:
                out_path.unlink(missing_ok=True)
                self.log(
                    f"[Build] ERROR: merged image differs from the signed input at 0x{offset:x}; output removed.\n"
                )
                return None

        self.log(f"[Build] Secure Boot v2 public key digest: {key_digest}\n")
        return {
            "partition_table_offset": pt_offset,
            **layout,
            "secure_boot_key_digest": key_digest,
            "chip_rev_min": "v3.0",
        }

    def _run_logged_cmd(
        self,
        cmd: list[str],
        cwd: Path,
        env: dict[str, str] | None = None,
        progress_cb: Callable[[float], None] | None = None,
        progress_weights: list[int] | None = None,
        ninja_cb: Callable[[int, int], None] | None = None,
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
            if ninja_cb:
                m_step = _NINJA_PROGRESS_RE.match(line)
                if m_step:
                    ninja_cb(int(m_step.group(1)), int(m_step.group(2)))
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
