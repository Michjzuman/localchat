#!/usr/bin/env bash

set -euo pipefail

APP_NAME="localchat"
SERVER_NAME="localchatd"

GITHUB_USER="${GITHUB_USER:-michjzuman}"
GITHUB_REPO="${GITHUB_REPO:-localchat}"
GITHUB_BRANCH="${GITHUB_BRANCH:-main}"
RAW_BASE="${RAW_BASE:-https://raw.githubusercontent.com/${GITHUB_USER}/${GITHUB_REPO}/${GITHUB_BRANCH}}"

INSTALL_BIN="${INSTALL_BIN:-/usr/local/bin}"
INSTALL_SBIN="${INSTALL_SBIN:-/usr/local/sbin}"
SERVICE_FILE="${SERVICE_FILE:-/etc/systemd/system/localchatd.service}"

TMP_DIR=""

cleanup() {
    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
    fi
}

die() {
    echo "Error: $*" >&2
    exit 1
}

print_dependency_hint() {
    cat >&2 <<'EOF'

Required packages:
  Debian/Ubuntu: sudo apt install build-essential curl libncurses-dev
  Fedora:        sudo dnf install gcc curl ncurses-devel
  RHEL/CentOS:   sudo yum install gcc make curl ncurses-devel
  Arch:          sudo pacman -S base-devel curl ncurses
EOF
}

has_ncurses() {
    local test_file
    local test_bin

    if ! command -v gcc >/dev/null 2>&1; then
        return 1
    fi

    test_file="$(mktemp)"
    test_bin="$(mktemp)"

    cat > "$test_file" <<'EOF'
#include <ncurses.h>
int main(void) {
    initscr();
    endwin();
    return 0;
}
EOF

    gcc -x c "$test_file" -o "$test_bin" -lncurses >/dev/null 2>&1
    local ok=$?

    rm -f "$test_file" "$test_bin"
    return "$ok"
}

install_dependencies() {
    if command -v apt-get >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y build-essential curl libncurses-dev
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y gcc make curl ncurses-devel
    elif command -v yum >/dev/null 2>&1; then
        yum install -y gcc make curl ncurses-devel
    elif command -v pacman >/dev/null 2>&1; then
        pacman -Sy --needed --noconfirm base-devel curl ncurses
    else
        echo "No supported package manager found." >&2
        print_dependency_hint
        exit 1
    fi
}

ensure_dependencies() {
    local needs_install=0

    for command_name in gcc curl install mktemp; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            needs_install=1
        fi
    done

    if ! has_ncurses; then
        needs_install=1
    fi

    if [ "$needs_install" -eq 1 ]; then
        echo "Installing missing build dependencies..."
        install_dependencies
    fi

    for command_name in gcc curl install mktemp; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            echo "Missing command after dependency installation: $command_name" >&2
            print_dependency_hint
            exit 1
        fi
    done

    if ! has_ncurses; then
        echo "ncurses headers or libraries are missing after dependency installation." >&2
        print_dependency_hint
        exit 1
    fi
}

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    die "Run this script with sudo/root, for example: curl -fsSL ${RAW_BASE}/install.sh | sudo bash"
fi

if [ "$(uname -s)" != "Linux" ]; then
    die "localchat requires Linux, systemd, and SO_PEERCRED."
fi

trap cleanup EXIT

echo "[1/5] Checking dependencies..."

ensure_dependencies

if ! command -v systemctl >/dev/null 2>&1; then
    die "systemctl was not found. localchat needs a Linux system with systemd."
fi

TMP_DIR="$(mktemp -d)"
cd "$TMP_DIR"

echo "[2/5] Downloading source files..."

curl -fsSL "${RAW_BASE}/localchatd.c" -o localchatd.c
curl -fsSL "${RAW_BASE}/localchat.c" -o localchat.c

echo "[3/5] Compiling..."

gcc -Wall -Wextra -O2 localchatd.c -o "$SERVER_NAME"
gcc -Wall -Wextra -O2 localchat.c -o "$APP_NAME" -pthread -lncurses

echo "[4/5] Installing files..."

install -d -m 755 "$INSTALL_BIN" "$INSTALL_SBIN" "$(dirname "$SERVICE_FILE")"
install -m 755 "$SERVER_NAME" "$INSTALL_SBIN/$SERVER_NAME"
install -m 755 "$APP_NAME" "$INSTALL_BIN/$APP_NAME"

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=localchat server
After=network.target

[Service]
Type=simple
ExecStart=$INSTALL_SBIN/$SERVER_NAME
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

echo "[5/5] Starting service..."

systemctl daemon-reload
systemctl enable localchatd
systemctl restart localchatd

echo
echo "Done."
echo "Start the chat with:"
echo "    $APP_NAME"
