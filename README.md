# localchat

localchat is a small terminal chat for multi-user Unix systems. On Linux, a
systemd service (`localchatd`) runs in the background, provides the Unix
domain socket `/run/localchat/socket`, and identifies connected users by
their real UID via `SO_PEERCRED`. On macOS, the daemon can run manually on
`/tmp/localchat.sock` and identifies users via `getpeereid()`. The
`localchat` client connects to the socket and shows an ncurses TUI with chat
bubbles.

## Requirements

- Linux with systemd for the installer and service mode
- macOS for manual daemon/client use
- A C compiler, `make`, `curl`, and ncurses headers
- A supported Linux package manager for automatic dependency installation
  (`apt-get`, `dnf`, `yum`, or `pacman`)
- A UTF-8 locale (the client renders with wide characters)
- Root privileges for installation and Linux service setup

## Linux Installation

```sh
curl -fsSL https://localchat.michjzuman.com/install.sh | sudo bash
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
```

On Linux, install and start the systemd service:

```sh
sudo install -m 644 localchatd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now localchatd
```

On macOS, run the daemon manually in one terminal and the client in another:

```sh
make
./localchatd --socket /tmp/localchat.sock
./localchat --socket /tmp/localchat.sock
```

## Usage

After Linux installation the server runs as a systemd service:

```sh
localchat status
sudo localchat restart
sudo localchat stop
localchat logs -f
```

Start the chat as a normal user:

```sh
localchat
```

All users on the same system who run `localchat` share one chat room.
If `localchatd` restarts, the client keeps running and reconnects
automatically. New clients receive recent in-memory chat history from the
daemon.

### Keys

| Key                | Action                       |
|--------------------|------------------------------|
| Enter              | send message                 |
| ←/→                | move cursor                  |
| ↑/↓                | scroll back / forward (line) |
| Home / End         | start / end of input         |
| Backspace / Del    | edit                         |
| Ctrl-W             | delete previous word         |
| Ctrl-U / Ctrl-K    | clear input / kill to EOL    |
| Ctrl-L             | redraw                       |
| PgUp / PgDn        | scroll back / forward (page) |
| Ctrl-C / Ctrl-D    | quit                         |

### Command-line

```text
localchat [--socket PATH] [--color auto|always|never] [--no-color]
          [--version] [--help]
localchat update                 # download and run the Linux installer (root)
localchat update --check         # compare installed and latest versions
localchat status                 # show localchatd status
localchat start|stop|restart     # manage systemd service on Linux (root)
localchat logs [-f]              # show Linux daemon logs, optionally follow
localchat uninstall              # remove binaries + systemd unit (root)

localchatd [--socket PATH] [--version] [--help]
```

`--socket` is useful for ad-hoc / test setups, e.g. running a daemon on a
non-default path. Defaults are `/run/localchat/socket` on Linux and
`/tmp/localchat.sock` on macOS:

```sh
localchatd -s /tmp/lc.sock &
localchat  -s /tmp/lc.sock
```

Colors default to `auto` and respect `NO_COLOR=1`.

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
user's name (resolved from `SO_PEERCRED` on Linux or `getpeereid()` on
macOS/BSD, then `getpwuid`) and broadcasts to all connected clients.

## Linux Service Hardening

`localchatd.service` runs the daemon with `NoNewPrivileges`,
`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
`MemoryDenyWriteExecute`, `RestrictAddressFamilies=AF_UNIX`, and an empty
capability bounding set. The socket lives in `/run/localchat/` (created
by systemd as a `RuntimeDirectory`) with mode `0666` so any user on the
system can connect.

## Limitations

- The installer and service management commands are Linux/systemd-only.
- macOS currently uses manual daemon startup; no launchd service is
  installed.
- Bubble rendering assumes a UTF-8 terminal that supports box-drawing
  characters.
- Scrollback is limited to the last ~1024 messages of the current session.
- New clients receive recent in-memory chat history from the daemon, but
  history is not persisted across daemon restarts.
- The client reconnects automatically, but outbound messages are not queued
  while disconnected.
- Resizing the terminal re-renders from in-memory log; older messages may
  be lost if more than ~1024 messages have arrived.
