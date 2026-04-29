CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral -pedantic
LDFLAGS ?=

DESTDIR ?=
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin

SERVER_SRC = localchatd.c
CLIENT_SRC = localchat.c

SERVER_BIN = localchatd
CLIENT_BIN = localchat

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  NCURSES_LIB ?= -lncurses
else
  NCURSES_LIB ?= -lncursesw
endif

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SERVER_SRC) -o $@

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) $(CLIENT_SRC) -o $@ $(NCURSES_LIB)

check:
	$(CC) -fsyntax-only $(CFLAGS) $(SERVER_SRC)
	$(CC) -fsyntax-only $(CFLAGS) $(CLIENT_SRC)
	bash -n install.sh

test:
	$(MAKE) all
	tests/smoke.sh

install: all
	install -d -m 755 "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(SBINDIR)"
	install -m 755 $(SERVER_BIN) "$(DESTDIR)$(SBINDIR)/$(SERVER_BIN)"
	install -m 755 $(CLIENT_BIN) "$(DESTDIR)$(BINDIR)/$(CLIENT_BIN)"

uninstall:
	rm -f "$(DESTDIR)$(SBINDIR)/$(SERVER_BIN)" "$(DESTDIR)$(BINDIR)/$(CLIENT_BIN)"

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)

.PHONY: all check test install uninstall clean
