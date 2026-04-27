# AGENTS.md

## Project Context

`localchat` is a small terminal chat for Linux multi-user systems. A
background daemon (`localchatd`) runs as a systemd service, listens on the
Unix domain socket `/run/localchat/socket`, and identifies connected users
from their real UID via `SO_PEERCRED` and `getpwuid()`. Users start the
`localchat` client from their shell to chat with other users on the same
machine.

The project is intentionally small and C-based. Keep changes direct,
readable, and close to the current implementation unless a larger refactor
is needed for correctness.

## Repository Layout

- `localchatd.c` — Linux server daemon. Single-threaded, non-blocking
  poll() loop with per-client in/out buffers. Tracks up to 64 clients,
  maps connections to usernames, broadcasts messages, emits join/leave.
- `localchat.c` — ncursesw client. Single-threaded poll() over stdin and
  the daemon socket. Wide-character input, scrollback, bubble layout.
- `localchatd.service` — hardened systemd unit (RuntimeDirectory,
  NoNewPrivileges, ProtectSystem=strict, etc.).
- `install.sh` — root-run installer. Installs deps on supported distros,
  downloads sources from GitHub (or uses local with `LOCAL_SOURCE=1`),
  compiles via Makefile, installs binaries + service.
- `Makefile` — `make` / `make check` / `make test` / `make install`.
- `tests/test_client.c`, `tests/smoke.sh` — minimal Linux smoke test.
- `.github/workflows/ci.yml` — GitHub Actions CI (build + smoke test).
- `design.py` — Python prototype for the chat-bubble layout. The C client
  follows this style: own messages right-aligned, others left-aligned,
  three-line boxed input.
- `README.md` — user-facing installation / usage docs.

## Build

```sh
make                     # builds localchatd + localchat
make check               # syntax check + bash -n install.sh
make test                # builds + runs tests/smoke.sh (Linux only)
sudo make install        # installs binaries to /usr/local
```

Manual:

```sh
gcc -Wall -Wextra -O2 localchatd.c -o localchatd
gcc -Wall -Wextra -O2 localchat.c  -o localchat -lncursesw
```

The client links against `ncursesw` (wide-character ncurses). On
Debian/Ubuntu, `libncurses-dev` provides it.

`localchatd.c` is Linux-specific (uses `struct ucred` and `SO_PEERCRED`)
and contains a `#error` for non-Linux platforms; expect it to fail
syntax-checking on macOS. The client is portable enough to syntax-check
on macOS but is only useful against a Linux daemon.

## Install / Update Flow

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh | sudo bash
```

The installer is non-interactive after sudo authentication. Distro
packages installed on demand:

- `apt-get`: `build-essential curl libncurses-dev`
- `dnf`: `gcc make curl ncurses-devel`
- `yum`: `gcc make curl ncurses-devel`
- `pacman`: `base-devel curl ncurses`

The installer restarts `localchatd`, which disconnects active clients.
To install from a local working copy:

```sh
sudo LOCAL_SOURCE=1 bash install.sh
```

## UI Expectations

The ncurses UI follows `design.py`:

- own messages are right-aligned with the local username above the bubble
- other users' messages are left-aligned with the sender above the bubble
- bubble borders use Unicode box-drawing characters
- own messages use `❯`, others use `❮`
- the input area is a three-line boxed field with an `⏎` marker
- system messages are dim and centered

Width is computed via `wcwidth`; chunking respects code-point boundaries.

## Protocol

Length-prefixed binary framing on a Unix-domain stream socket:

```
+----------+----------------------+
| u32 BE   | payload (UTF-8)      |
| length   | <= 8192 bytes        |
+----------+----------------------+
```

- Client → server payload: raw message body, max **4096** bytes. Server
  drops the client if a length prefix exceeds this.
- Server → client payload: `[username] body` (chat) or `[system] body`
  (notifications). Max **8192** bytes total on the wire.

Both sides sanitize control characters (kept: `\t`; everything else under
0x20 or 0x7f is replaced with space). Usernames are sanitized to avoid
`[`, `]`, whitespace, or control chars.

## Hardening

`localchatd.service` runs the daemon with:

- `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
  `PrivateDevices`
- `ProtectKernelTunables`, `ProtectKernelModules`, `ProtectKernelLogs`,
  `ProtectControlGroups`, `ProtectClock`, `ProtectHostname`
- `RestrictNamespaces`, `RestrictRealtime`, `RestrictSUIDSGID`,
  `LockPersonality`, `MemoryDenyWriteExecute`
- `RestrictAddressFamilies=AF_UNIX`, `CapabilityBoundingSet=` (empty)
- `RuntimeDirectory=localchat` (creates `/run/localchat/` on start)

## Contribution Guidelines

- Keep the user-facing project name lowercase: `localchat`, not
  `LocalChat`.
- Keep user-facing repository text in English.
- Preserve the Linux/systemd focus unless the task explicitly asks for
  portability.
- Do not introduce large frameworks or build systems without a clear
  need. Prefer focused C changes and simple shell in `install.sh`.
- Before committing, run `make check` and (on Linux) `make test`.
- Compile the touched C target where the local environment allows.
- Both client and server must stay binary-protocol compatible; if you
  change the framing or message format, update **both** in the same
  commit.

## Known Limitations

- Bubble rendering assumes a UTF-8 terminal with box-drawing characters.
- Scrollback is in-memory only (last ~1024 messages of the current
  session).
- `getpwuid` is invoked on each connect; `nss_*` plugins live in the
  daemon's address space (acceptable here, but worth knowing).
- No automated reconnect in the client.
- No federation, no persistence, no history across restarts.
