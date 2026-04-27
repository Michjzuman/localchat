# AGENTS.md

## Project Context

`localchat` is a small terminal chat for Linux multi-user systems. A background
daemon (`localchatd`) runs as a systemd service, listens on the Unix domain
socket `/run/localchat.sock`, and identifies connected users from their real
UID via `SO_PEERCRED` and `getpwuid()`. Users start the `localchat` client from
their shell to chat with other users on the same machine.

The project is intentionally small and C-based. Keep changes direct, readable,
and close to the current implementation unless a larger refactor is needed for
correctness.

## Repository Layout

- `localchatd.c`: Linux server daemon. Accepts Unix socket clients, tracks up to
  64 clients, maps sockets to usernames, broadcasts messages, and emits
  join/leave events.
- `localchat.c`: ncurses client. Connects to the daemon, reads input in the main
  thread, receives messages in a pthread, and renders a terminal UI.
- `design.py`: visual prototype for the chat bubble layout. The C client should
  follow this style: own messages right-aligned, other users left-aligned, and
  a boxed input area.
- `install.sh`: root-run installer. Installs missing dependencies on supported
  Linux distributions, downloads source files from GitHub, compiles binaries,
  installs them under `/usr/local`, and creates/restarts `localchatd.service`.
- `README.md`: user-facing installation and usage documentation.
- `localchat`, `localchatd`: tracked Linux binaries. They may be stale when
  source changes are made from macOS. Prefer source changes; rebuild on Linux
  when binary updates are required.

## Build Commands

Manual Linux build:

```sh
gcc -Wall -Wextra -O2 localchatd.c -o localchatd
gcc -Wall -Wextra -O2 localchat.c -o localchat -pthread -lncurses
```

Installer syntax check:

```sh
bash -n install.sh
```

Whitespace/diff check:

```sh
git diff --check
```

On macOS, `localchat.c` may compile if ncurses is available, but
`localchatd.c` is Linux-specific because it uses `struct ucred` and
`SO_PEERCRED`.

## Install/Update Flow

The public install/update command is:

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh | sudo bash
```

The installer should remain non-interactive after sudo authentication. It should
install missing dependencies automatically on supported package managers:

- `apt-get`: `build-essential curl libncurses-dev`
- `dnf`: `gcc make curl ncurses-devel`
- `yum`: `gcc make curl ncurses-devel`
- `pacman`: `base-devel curl ncurses`

The installer restarts `localchatd`, which disconnects active clients.

## UI Expectations

The ncurses UI should visually follow `design.py`:

- own messages are right-aligned and show the local username above the bubble
- other users' messages are left-aligned and show the sender above the bubble
- bubble borders use Unicode box drawing characters
- own messages use `❯`; other messages use `❮`
- the input area is a three-line boxed field with an `⏎` marker
- system messages can stay simple and centered/dim

Keep in mind that current width calculations are byte-based and not fully
Unicode-width aware. If improving UTF-8 handling, prefer a deliberate
`ncursesw`/wide-character approach rather than small ad hoc fixes.

## Protocol Notes

The current protocol is a simple stream of text over an Unix domain socket.
Server messages are formatted as:

- `[system] message`
- `[username] message`

This means the client currently parses display semantics from formatted text.
There is no robust message framing yet, so socket reads can split or combine
messages. If changing protocol behavior, update both server and client together
and keep backwards compatibility in mind only if explicitly required.

## Known Technical Debt

- Server writes ignore partial writes, `EINTR`, `EPIPE`, and `SIGPIPE`.
- Server polling does not fully handle `POLLHUP`, `POLLERR`, or `POLLNVAL`.
- The protocol lacks explicit message framing.
- The TUI has limited scrollback/history behavior.
- Unicode rendering is visual-only right now; width calculations are not
  correct for multi-byte or wide characters.
- There are no automated tests or Linux CI jobs.

## Contribution Guidelines

- Keep user-facing project name lowercase: `localchat`, not `LocalChat`.
- Keep user-facing repository text in English.
- Preserve the Linux/systemd focus unless the task explicitly asks for
  portability.
- Do not introduce large frameworks or build systems without a clear need.
- Prefer focused C changes and simple shell in `install.sh`.
- Before committing, run at least `bash -n install.sh` and `git diff --check`.
  Compile the touched C target where the local environment supports it.
