#!/usr/bin/env bash
set -euo pipefail

case "$(uname -s)" in
    Linux|Darwin|FreeBSD|NetBSD|OpenBSD) ;;
    *)
        echo "smoke test skipped: unsupported peer-credential platform"
        exit 0
        ;;
esac

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
SOCKET_PATH="$TMP_DIR/socket"
SERVER_LOG="$TMP_DIR/localchatd.log"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

"$ROOT_DIR/localchatd" --socket "$SOCKET_PATH" >"$SERVER_LOG" 2>&1 &
SERVER_PID="$!"

for _ in $(seq 1 50); do
    [ -S "$SOCKET_PATH" ] && break
    sleep 0.1
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "localchatd did not create socket" >&2
    cat "$SERVER_LOG" >&2 || true
    exit 1
fi

python3 - "$SOCKET_PATH" <<'PY'
import os
import socket
import struct
import sys

sock_path = sys.argv[1]

def connect_client():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(sock_path)
    return s

def recv_exact(sock, n):
    chunks = []
    remaining = n
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("socket closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)

def recv_frame(sock):
    raw_len = recv_exact(sock, 4)
    (size,) = struct.unpack("!I", raw_len)
    payload = recv_exact(sock, size)
    return payload.decode("utf-8", "replace")

def send_frame(sock, text):
    payload = text.encode("utf-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)

first = connect_client()
assert recv_frame(first) == "[system] welcome to localchat"

send_frame(first, "hello history")
own_echo = recv_frame(first)
assert own_echo.endswith("hello history"), own_echo

second = connect_client()
history = recv_frame(second)
welcome = recv_frame(second)

assert history.endswith("hello history"), (history, welcome)
assert welcome == "[system] welcome to localchat", (history, welcome)

first.close()
second.close()
PY
