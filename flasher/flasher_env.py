"""ESP-IDF install locations, and the re-launch into the ESP-IDF Python environment. Imports only the standard
library, so it works before pyserial / esptool / esp_kconfiglib are known to be importable."""

from __future__ import annotations

import os
import sys
from pathlib import Path

FLASHER_DIR = Path(__file__).resolve().parent
if str(FLASHER_DIR) not in sys.path:
    sys.path.insert(0, str(FLASHER_DIR))

IDF_VENV_PYTHON_DEFAULT = Path(r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe")
IDF_PATH_DEFAULT = Path(r"C:\esp\v6.1\esp-idf")
IDF_TOOLS_PATH_DEFAULT = Path(r"C:\Espressif\tools")


def get_idf_venv_python() -> Path:
    """Return the ESP-IDF Python executable from IDF_PYTHON_ENV_PATH or the project default location."""
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path:
        exe = Path(env_path) / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        if exe.is_file():
            return exe
    return IDF_VENV_PYTHON_DEFAULT


IDF_VENV_PYTHON = get_idf_venv_python()


def ensure_idf_python() -> None:
    """Re-run the current script under the ESP-IDF v6.1 Python venv if any required tool is missing here."""
    try:
        import esp_kconfiglib  # noqa: F401
        import esptool  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
    except ImportError:
        target_py = get_idf_venv_python()
        if target_py.is_file() and Path(sys.executable).resolve() != target_py.resolve():
            os.execv(str(target_py), [str(target_py), *sys.argv])
        raise
