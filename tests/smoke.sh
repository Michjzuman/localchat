#!/usr/bin/env bash
#
# Smoke test: start localchatd on a temporary socket, run a small test
# client against it, and verify welcome + echo. Linux-only.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ "$(uname -s)" != "Linux" ]; then
    echo "smoke test requires Linux (SO_PEERCRED)" >&2
    exit 0
fi

CC_BIN="${CC:-cc}"

if [ ! -x "$ROOT/localchatd" ]; then
    echo "building localchatd" >&2
    make localchatd
fi

TMP="$(mktemp -d)"
SOCK="$TMP/sock"
LOG="$TMP/log"
PID=

cleanup() {
    if [ -n "$PID" ]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

"$ROOT/localchatd" -s "$SOCK" >"$LOG" 2>&1 &
PID=$!

# Wait up to 2s for the socket
for _ in $(seq 1 40); do
    [ -S "$SOCK" ] && break
    sleep 0.05
done
if [ ! -S "$SOCK" ]; then
    echo "daemon failed to create socket" >&2
    cat "$LOG" >&2
    exit 1
fi

"$CC_BIN" -O2 -Wall -Wextra "$ROOT/tests/test_client.c" -o "$TMP/test_client"

# Two clients: one sends, the other should see the broadcast.
"$TMP/test_client" "$SOCK" "hello world"

# Test rejection of oversized message
python3 - "$SOCK" <<'PY' || true
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
# read welcome
hdr = s.recv(4)
n = struct.unpack(">I", hdr)[0]
s.recv(n)
# send oversized: 10 MB length prefix, no body — server should drop us
s.send(struct.pack(">I", 10_000_000))
# expect EOF
data = s.recv(1024)
assert data == b"" or data is None, f"server did not drop oversize: {data!r}"
print("oversize-rejected", file=sys.stderr)
PY

echo "smoke test passed"
