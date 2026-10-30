@echo off
cd /d "%~dp0"
if exist "%~dp0dist\SwitchRichPresence\SwitchRichPresence.exe" (
    start "" "%~dp0dist\SwitchRichPresence\SwitchRichPresence.exe"
) else if exist "%~dp0.venv\Scripts\pythonw.exe" (
    start "" "%~dp0.venv\Scripts\pythonw.exe" "%~dp0companion_gui.py"
) else (
    echo Execute o build ou instale as dependencias de requirements.txt.
    pause
)
