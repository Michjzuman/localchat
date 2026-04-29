# AGENTS.md

## Project Context

`localchat` is a small terminal chat for multi-user Unix-like systems.
Linux/systemd is the primary install target: a background daemon
(`localchatd`) runs as a systemd service, listens on the Unix domain socket
`/run/localchat/socket`, and identifies connected users from their real UID
via `SO_PEERCRED` and `getpwuid()`. macOS supports manual daemon/client use
with `/tmp/localchat.sock` and `getpeereid()`. Users start the `localchat`
client from their shell to chat with other users on the same machine.

The project is intentionally small and C-based. Keep changes direct,
readable, and close to the current implementation unless a larger refactor
is needed for correctness.

## Repository Layout

- `localchatd.c` — server daemon. Single-threaded, non-blocking poll() loop
  with per-client in/out buffers. Tracks up to 64 clients, maps connections
  to usernames via peer credentials, broadcasts messages, emits join/leave.
- `localchat.c` — ncursesw client. Single-threaded poll() over stdin and
  the daemon socket. Wide-character input, scrollback, status line,
  automatic reconnect, colorized bubble layout.
- `localchatd.service` — hardened systemd unit (RuntimeDirectory,
  NoNewPrivileges, ProtectSystem=strict, etc.).
- `install.sh` — root-run installer. Installs deps on supported distros,
  downloads sources from GitHub (or uses local with `LOCAL_SOURCE=1`),
  compiles via Makefile, installs binaries + service.
- `tests/smoke.sh` — smoke test for daemon framing and history replay on
  supported peer-credential platforms.
- `Makefile` — `make` / `make check` / `make test` / `make install`.
- `.github/workflows/ci.yml` — GitHub Actions CI (build + syntax check + smoke test).
- `design.py` — Python prototype for the chat-bubble layout. The C client
  follows this style: own messages right-aligned, others left-aligned,
  three-line boxed input.
- `README.md` — user-facing installation / usage docs.

## Build

```sh
make                     # builds localchatd + localchat
make check               # syntax check + bash -n install.sh
make test                # smoke test where supported
sudo make install        # installs binaries to /usr/local
```

Manual:

```sh
gcc -Wall -Wextra -O2 localchatd.c -o localchatd
gcc -Wall -Wextra -O2 localchat.c  -o localchat -lncursesw
```

The client links against `ncursesw` on Linux and `ncurses` on macOS via the
Makefile. On Debian/Ubuntu, `libncurses-dev` provides it.

`localchatd.c` supports Linux via `SO_PEERCRED` and macOS/BSD via
`getpeereid()`. Default socket paths are `/run/localchat/socket` on Linux
and `/tmp/localchat.sock` on macOS.

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

The installer is Linux/systemd-only and restarts `localchatd`, which
disconnects active clients. To install from a local working copy:

```sh
sudo LOCAL_SOURCE=1 bash install.sh
```

For macOS development or ad-hoc use:

```sh
make
./localchatd --socket /tmp/localchat.sock
./localchat --socket /tmp/localchat.sock
```

## UI Expectations

The ncurses UI follows `design.py`:

- own messages are right-aligned with the local username above the bubble
- other users' messages are left-aligned with the sender above the bubble
- bubble borders use Unicode box-drawing characters
- own messages use `❯`, others use `❮`
- the input area is a boxed field with an `⏎` marker and grows modestly for
  explicit newlines
- system messages are dim and centered
- the top debug status line is hidden by default and shown with `--debug`;
  it stays compact: app name, local username, connection state, socket path

Width is computed via `wcwidth`; chunking respects code-point boundaries and
prefers wrapping at whitespace before hard-wrapping long words.

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

Both sides sanitize control characters (kept: `\t` and `\n`; everything else
under 0x20 or 0x7f is replaced with space). Usernames are sanitized to avoid
`[`, `]`, whitespace, or control chars.

## Hardening

On Linux, `localchatd.service` runs the daemon with:

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
- Preserve the Linux/systemd install focus unless the task explicitly asks
  for portability. Keep macOS support manual and lightweight unless asked
  for launchd packaging.
- Do not introduce large frameworks or build systems without a clear
  need. Prefer focused C changes and simple shell in `install.sh`.
- Before committing, run `make check` and `make test`.
- Compile the touched C target where the local environment allows.
- Both client and server must stay binary-protocol compatible; if you
  change the framing or message format, update **both** in the same
  commit.

## Known Limitations

- Bubble rendering assumes a UTF-8 terminal with box-drawing characters.
- Scrollback is in-memory only (last ~1024 messages of the current
  session).
- Daemon-side history replay is in-memory only and intentionally stores chat
  messages, not join/leave/status notifications.
- The installer and `start`/`stop`/`restart`/`logs` commands are
  Linux/systemd-only. macOS currently uses manual daemon startup.
- `getpwuid` is invoked on each connect; `nss_*` plugins live in the
  daemon's address space (acceptable here, but worth knowing).
- The client reconnects automatically after daemon restarts, but does not
  queue outbound messages while disconnected.
- No federation, no persistence, no history across restarts.
