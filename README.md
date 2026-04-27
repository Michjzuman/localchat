# localchat

localchat is a small terminal chat for Linux multi-user systems. A systemd
service (`localchatd`) runs in the background, provides the Unix domain
socket `/run/localchat/socket`, and identifies connected users by their real
UID via `SO_PEERCRED`. The `localchat` client connects to the socket and
shows an ncursesw TUI with chat bubbles.

## Requirements

- Linux with systemd
- A supported package manager (`apt-get`, `dnf`, `yum`, or `pacman`)
- A UTF-8 locale (the client renders with wide characters)
- Root privileges for installation and service setup

## Installation

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh | sudo bash
```

The script installs missing build dependencies, downloads the sources,
compiles both binaries, installs the client to `/usr/local/bin/localchat`,
the server to `/usr/local/sbin/localchatd`, and the hardened
`localchatd.service` unit. The systemd unit creates `/run/localchat/`
automatically (`RuntimeDirectory=`) and the daemon places the socket there.

Override repository, branch, or target paths via environment variables:

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh \
  | sudo GITHUB_BRANCH=main INSTALL_BIN=/usr/local/bin bash
```

To install from a local checkout (development):

```sh
sudo LOCAL_SOURCE=1 bash install.sh
```

## Manual Build

```sh
make
sudo make install
sudo install -m 644 localchatd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now localchatd
```

## Usage

After installation the server runs as a systemd service:

```sh
sudo systemctl status localchatd
sudo systemctl restart localchatd
sudo systemctl stop localchatd
journalctl -u localchatd -f
```

Start the chat as a normal user:

```sh
localchat
```

All users on the same system who run `localchat` share one chat room.

### Keys

| Key                | Action                       |
|--------------------|------------------------------|
| Enter              | send message                 |
| ←/→                | move cursor                  |
| Home / End         | start / end of input         |
| Backspace / Del    | edit                         |
| Ctrl-W             | delete previous word         |
| Ctrl-U / Ctrl-K    | clear input / kill to EOL    |
| Ctrl-L             | redraw                       |
| PgUp / PgDn        | scroll back / forward        |
| Ctrl-C / Ctrl-D    | quit                         |

### Command-line

```text
localchat [--socket PATH] [--version] [--help]
localchat uninstall              # remove binaries + systemd unit (root)

localchatd [--socket PATH] [--version] [--help]
```

`--socket` is useful for ad-hoc / test setups, e.g. running a daemon on a
non-default path:

```sh
localchatd -s /tmp/lc.sock &
localchat  -s /tmp/lc.sock
```

## Protocol

A trivial length-prefixed framing on a Unix domain socket:

```
+--------+--------------------+
| u32 BE | payload (UTF-8)    |
| length | up to 8192 bytes   |
+--------+--------------------+
```

Server-to-client payloads are formatted as `[username] body` for chat
messages and `[system] body` for notifications. Client-to-server payloads
are the raw message body (max 4096 bytes). The server prefixes with the
user's name (resolved from `SO_PEERCRED` + `getpwuid`) and broadcasts to
all connected clients.

## Tests

```sh
make test         # builds the daemon, runs tests/smoke.sh
```

The smoke test starts a daemon on a temporary socket, runs a small C
client to verify welcome + echo framing, and checks that oversized
messages get the client dropped.

## Service Hardening

`localchatd.service` runs the daemon with `NoNewPrivileges`,
`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
`MemoryDenyWriteExecute`, `RestrictAddressFamilies=AF_UNIX`, and an empty
capability bounding set. The socket lives in `/run/localchat/` (created
by systemd as a `RuntimeDirectory`) with mode `0666` so any user on the
system can connect.

## Limitations

- Linux only (uses `SO_PEERCRED`).
- Bubble rendering assumes a UTF-8 terminal that supports box-drawing
  characters.
- Scrollback is limited to the last ~1024 messages of the current session.
- No persistent history across daemon restarts.
- Resizing the terminal re-renders from in-memory log; older messages may
  be lost if more than ~1024 messages have arrived.
