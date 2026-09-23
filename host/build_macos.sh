#!/usr/bin/env bash
# Сборка OLED-Monitor.dmg из monitor.py. Запускать НА macOS.
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="OLED-Monitor"

python3 -m venv .venv-mac
source .venv-mac/bin/activate
pip install --upgrade pip
pip install -r requirements-macos.txt

rm -rf build dist
pyinstaller --clean --noconfirm --windowed --name "$APP_NAME" \
    --hidden-import="bleak.backends.corebluetooth.scanner" \
    --hidden-import="bleak.backends.corebluetooth.client" \
    --add-data "mac_bridge.py:." \
    monitor.py

PLIST="dist/$APP_NAME.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Delete :LSUIElement" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :LSUIElement bool true" "$PLIST"
codesign --force --deep --sign - "dist/$APP_NAME.app"

STAGE="dist/$APP_NAME.dmg-staging"
mkdir -p "$STAGE"
cp -R "dist/$APP_NAME.app" "$STAGE/"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "dist/$APP_NAME.dmg"
rm -rf "$STAGE"

echo "Готово: dist/$APP_NAME.dmg"