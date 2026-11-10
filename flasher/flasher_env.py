"""ESP-IDF install locations, and the re-launch into the ESP-IDF Python environment. Imports only the standard
library, so it works before pyserial / esptool / esp_kconfiglib are known to be importable."""

from __future__ import annotations

import os
import sys
from pathlib import Path

IDF_VENV_PYTHON = Path(r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe")
IDF_PATH_DEFAULT = Path(r"C:\esp\v6.1\esp-idf")
IDF_TOOLS_PATH_DEFAULT = Path(r"C:\Espressif\tools")


def ensure_idf_python() -> None:
    """Re-run the current script under the ESP-IDF v6.1 Python venv if pyserial is missing here."""
    try:
        import serial.tools.list_ports  # noqa: F401  (only checks that pyserial is installed)
    except ImportError:
        if IDF_VENV_PYTHON.is_file() and Path(sys.executable).resolve() != IDF_VENV_PYTHON.resolve():
            os.execv(str(IDF_VENV_PYTHON), [str(IDF_VENV_PYTHON), *sys.argv])
        raise
