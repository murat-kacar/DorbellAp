@echo off
setlocal
set "IDF_PY=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
if exist "%IDF_PY%" (
    start "" "%IDF_PY%" "%~dp0flasher_gui.py" %*
) else (
    python "%~dp0flasher_gui.py" %*
)

