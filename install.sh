#!/usr/bin/env bash
#
# localchat installer.
#
# Downloads sources from GitHub, compiles, installs to /usr/local, and
# enables the localchatd systemd service. Usage:
#
#   curl -fsSL https://localchat.michjzuman.com/install.sh | sudo bash
#
# Environment overrides:
#   GITHUB_USER, GITHUB_REPO, GITHUB_BRANCH, RAW_BASE
#   INSTALL_BIN, INSTALL_SBIN, SERVICE_FILE
#   LOCAL_SOURCE=1     Build from the current working directory instead of
#                      downloading sources (useful for development).

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
LEGACY_SOCKET="/run/localchat.sock"

LOCAL_SOURCE="${LOCAL_SOURCE:-0}"

# When run via `curl ... | sudo bash` there is no source file on disk and
# BASH_SOURCE[0] is unset; only resolve SCRIPT_DIR when actually needed.
SCRIPT_DIR=""
if [ "$LOCAL_SOURCE" = "1" ]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-}")" && pwd)"
fi

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
  Fedora:        sudo dnf install gcc make curl ncurses-devel
  RHEL/CentOS:   sudo yum install gcc make curl ncurses-devel
  Arch:          sudo pacman -S base-devel curl ncurses
EOF
}

has_ncursesw() {
    if ! command -v gcc >/dev/null 2>&1; then return 1; fi
    local src bin rc
    src="$(mktemp)"
    bin="$(mktemp)"
    cat > "$src" <<'EOF'
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncurses.h>
int main(void){ initscr(); endwin(); return 0; }
EOF
    gcc -x c "$src" -o "$bin" -lncursesw >/dev/null 2>&1
    rc=$?
    rm -f "$src" "$bin"
    return "$rc"
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

    for cmd in gcc curl install mktemp make; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            needs_install=1
        fi
    done

    if ! has_ncursesw; then
        needs_install=1
    fi

    if [ "$needs_install" -eq 1 ]; then
        echo "Installing missing build dependencies..."
        install_dependencies
    fi

    for cmd in gcc curl install mktemp make; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "Missing command after dependency installation: $cmd" >&2
            print_dependency_hint
            exit 1
        fi
    done

    if ! has_ncursesw; then
        echo "ncursesw is missing after dependency installation." >&2
        print_dependency_hint
        exit 1
    fi
}

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    die "Run this script with sudo/root, e.g. curl -fsSL ${RAW_BASE}/install.sh | sudo bash"
fi

if [ "$(uname -s)" != "Linux" ]; then
    die "localchat requires Linux, systemd, and SO_PEERCRED."
fi

if ! command -v systemctl >/dev/null 2>&1; then
    die "systemctl was not found. localchat needs a Linux system with systemd."
fi

trap cleanup EXIT

echo "[1/6] Checking dependencies..."
ensure_dependencies

TMP_DIR="$(mktemp -d)"
cd "$TMP_DIR"

echo "[2/6] Fetching sources..."
if [ "$LOCAL_SOURCE" = "1" ]; then
    cp "$SCRIPT_DIR/localchatd.c" .
    cp "$SCRIPT_DIR/localchat.c" .
    cp "$SCRIPT_DIR/Makefile" .
    cp "$SCRIPT_DIR/localchatd.service" .
else
    curl -fsSL "${RAW_BASE}/localchatd.c"        -o localchatd.c
    curl -fsSL "${RAW_BASE}/localchat.c"         -o localchat.c
    curl -fsSL "${RAW_BASE}/Makefile"            -o Makefile
    curl -fsSL "${RAW_BASE}/localchatd.service"  -o localchatd.service
fi

echo "[3/6] Compiling..."
make clean >/dev/null 2>&1 || true
make all

echo "[4/6] Stopping existing service (if any)..."
systemctl stop localchatd >/dev/null 2>&1 || true

echo "[5/6] Installing files..."
install -d -m 755 "$INSTALL_BIN" "$INSTALL_SBIN" "$(dirname "$SERVICE_FILE")"
install -m 755 "$SERVER_NAME" "$INSTALL_SBIN/$SERVER_NAME"
install -m 755 "$APP_NAME"    "$INSTALL_BIN/$APP_NAME"
install -m 644 localchatd.service "$SERVICE_FILE"

# Remove the legacy socket from previous installs.
rm -f "$LEGACY_SOCKET" || true

echo "[6/6] Enabling service..."
systemctl daemon-reload
systemctl enable localchatd
systemctl restart localchatd

# Briefly verify the service came up.
sleep 0.3
if ! systemctl is-active --quiet localchatd; then
    echo
    echo "Warning: localchatd is not active. Check logs with:" >&2
    echo "  journalctl -u localchatd --no-pager -n 50" >&2
fi

echo
echo "Done."
echo "Start the chat with:"
echo "    $APP_NAME"
