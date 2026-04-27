# localchat

localchat is a small terminal chat for Linux multi-user systems. A systemd
service (`localchatd`) runs in the background, provides the Unix domain socket
`/run/localchat.sock`, and identifies connected users by their real UID
(`SO_PEERCRED`). The `localchat` client connects to the socket and shows a
simple ncurses TUI.

## Requirements

- Linux with systemd
- supported package manager (`apt-get`, `dnf`, `yum`, or `pacman`)
- root privileges for installation and service setup

## Installation

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh | sudo bash
```

The script installs missing build dependencies, downloads `localchat.c` and
`localchatd.c`, compiles both programs, installs the client to
`/usr/local/bin/localchat`, installs the server to `/usr/local/sbin/localchatd`,
and creates the `localchatd.service` systemd unit.

Repository, branch, and target paths can be overridden with environment
variables when needed:

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh \
  | sudo GITHUB_BRANCH=main INSTALL_BIN=/usr/local/bin bash
```

## Manual Build

```sh
gcc -Wall -Wextra -O2 localchatd.c -o localchatd
gcc -Wall -Wextra -O2 localchat.c -o localchat -pthread -lncurses
```

## Usage

After installation, the server runs as a systemd service:

```sh
sudo systemctl status localchatd
sudo systemctl restart localchatd
sudo systemctl stop localchatd
```

Start the chat as a normal user:

```sh
localchat
```

All users on the same system who start `localchat` join the same local chat.

## Current State

- server with `poll()` support for up to 64 clients
- UID-based user identification via `SO_PEERCRED`
- broadcast messages and join/leave events
- ncurses client with a message window and input bar
- Python prototype `design.py` as a visual reference for chat bubbles

## Open Tasks

- more robust error handling for partial writes, `SIGPIPE`, `POLLHUP`, and
  socket disconnects
- explicit message framing instead of raw stream reads
- optional chat history and scrollback in the client
- UTF-8 support through ncursesw/wide chars
- tests and Linux CI for builds and baseline behavior
