@echo off
rem Сборка OLED-Monitor.exe из monitor.py. Запускать НА Windows.
cd /d %~dp0

set APP_NAME=OLED-Monitor

if not exist .venv-win (
    py -3 -m venv .venv-win
)
call .venv-win\Scripts\activate
python -m pip install --upgrade pip
pip install -r requirements-windows.txt

rmdir /s /q build dist 2>nul
pyinstaller --clean --noconfirm --onefile --name %APP_NAME% ^
    --hidden-import="bleak.backends.winrt.discovery" ^
    --hidden-import="bleak.backends.winrt.client" ^
    --hidden-import="win32timezone" ^
    --add-data "win_bridge.py;." ^
    monitor.py

echo Готово: dist\%APP_NAME%.exe
pause