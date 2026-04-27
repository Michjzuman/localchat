#!/usr/bin/env bash

set -e

APP_NAME="localchat"
SERVER_NAME="localchatd"

INSTALL_BIN="/usr/local/bin"
INSTALL_SBIN="/usr/local/sbin"
SERVICE_FILE="/etc/systemd/system/localchatd.service"

if [ "$EUID" -ne 0 ]; then
    echo "Bitte mit sudo/root ausführen:"
    echo "curl -fsSL <url>/install.sh | sudo bash"
    exit 1
fi

echo "[1/5] Prüfe Abhängigkeiten..."

if ! command -v gcc >/dev/null 2>&1; then
    echo "gcc fehlt. Installiere es zuerst:"
    echo "Debian/Ubuntu: sudo apt install build-essential"
    echo "Fedora: sudo dnf install gcc make"
    echo "Arch: sudo pacman -S base-devel"
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemd wurde nicht gefunden."
    exit 1
fi

TMP_DIR="$(mktemp -d)"
cd "$TMP_DIR"

echo "[2/5] Lade Source-Dateien..."

curl -fsSL "https://raw.githubusercontent.com/DEIN_USERNAME/localchat/main/localchatd.c" -o localchatd.c
curl -fsSL "https://raw.githubusercontent.com/DEIN_USERNAME/localchat/main/localchat.c" -o localchat.c

echo "[3/5] Kompiliere..."

gcc localchatd.c -o "$SERVER_NAME"
gcc localchat.c -o "$APP_NAME" -pthread -lncurses

echo "[4/5] Installiere Dateien..."
 
install -m 755 "$SERVER_NAME" "$INSTALL_SBIN/$SERVER_NAME"
install -m 755 "$APP_NAME" "$INSTALL_BIN/$APP_NAME"

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=LocalChat server
After=network.target

[Service]
Type=simple
ExecStart=$INSTALL_SBIN/$SERVER_NAME
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

echo "[5/5] Starte Service..."

systemctl daemon-reload
systemctl enable localchatd
systemctl restart localchatd

rm -rf "$TMP_DIR"

echo
echo "Fertig."
echo "Starte den Chat mit:"
echo "    localchat"
