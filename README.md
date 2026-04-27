# LocalChat

LocalChat ist ein kleiner Terminal-Chat fuer Linux-Mehrbenutzersysteme. Ein
systemd-Dienst (`localchatd`) laeuft im Hintergrund, stellt den Unix-Domain-
Socket `/run/localchat.sock` bereit und identifiziert verbundene Benutzer ueber
ihre echte UID (`SO_PEERCRED`). Der Client `localchat` verbindet sich mit dem
Socket und zeigt eine einfache ncurses-TUI.

## Voraussetzungen

- Linux mit systemd
- `gcc`
- `curl`
- ncurses-Entwicklungspaket (`libncurses-dev`, `ncurses-devel` oder `ncurses`)
- root-Rechte fuer Installation und Service-Setup

## Installation

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh | sudo bash
```

Das Skript laedt `localchat.c` und `localchatd.c`, kompiliert beide Programme,
installiert den Client nach `/usr/local/bin/localchat`, den Server nach
`/usr/local/sbin/localchatd` und richtet den systemd-Service
`localchatd.service` ein.

Repository, Branch und Zielpfade koennen bei Bedarf per Umgebungsvariable
ueberschrieben werden:

```sh
curl -fsSL https://raw.githubusercontent.com/michjzuman/localchat/main/install.sh \
  | sudo GITHUB_BRANCH=main INSTALL_BIN=/usr/local/bin bash
```

## Manuell bauen

```sh
gcc -Wall -Wextra -O2 localchatd.c -o localchatd
gcc -Wall -Wextra -O2 localchat.c -o localchat -pthread -lncurses
```

## Nutzung

Nach der Installation laeuft der Server als systemd-Service:

```sh
sudo systemctl status localchatd
sudo systemctl restart localchatd
sudo systemctl stop localchatd
```

Den Chat startest du als normaler Benutzer:

```sh
localchat
```

Alle Benutzer auf demselben System, die `localchat` starten, landen im gleichen
lokalen Chat.

## Aktueller Stand

- Server mit `poll()` fuer bis zu 64 Clients
- UID-basierte Benutzererkennung per `SO_PEERCRED`
- Broadcast-Nachrichten und Join/Leave-Events
- ncurses-Client mit Nachrichtenfenster und Eingabeleiste
- Python-Prototyp `design.py` als visuelle Referenz fuer Chat-Bubbles

## Offene Punkte

- robustere Fehlerbehandlung fuer Partial Writes, `SIGPIPE`, `POLLHUP` und
  Socket-Abbrueche
- klares Message-Framing statt roher Stream-Reads
- optionaler Chatverlauf und Scrollback im Client
- UTF-8-Unterstuetzung ueber ncursesw/wide chars
- Tests und Linux-CI fuer Build und Basisverhalten
