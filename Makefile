CC       ?= gcc
CFLAGS   ?= -std=gnu11 -Os -Wall -Wextra
LDLIBS   ?= -lsqlite3

# Static linking is the Linux/musl ideal; override with
#   make STATIC=1     (force static anywhere)
# The Saphira package builds with STATIC=0 (dynamic musl libc).
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    STATIC_DEFAULT := 0
else
    STATIC_DEFAULT := 1
endif
STATIC   ?= $(STATIC_DEFAULT)
ifeq ($(STATIC),1)
    LDFLAGS ?= -static
else
    LDFLAGS ?=
endif

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
MAN1DIR  ?= $(MANDIR)/man1
DESTDIR  ?=

SRC := src/main.c src/mcp.c src/json.c src/gbuf.c src/db.c src/conf.c src/runner.c src/tools.c
OBJ := $(SRC:.c=.o)
BIN := saphira-build-mcp

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)
	strip $@

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	sh tests/test.sh

clean:
	rm -f $(OBJ) $(BIN)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MAN1DIR)
	install -m 0644 saphira-build-mcp.1 $(DESTDIR)$(MAN1DIR)/$(BIN).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MAN1DIR)/$(BIN).1
	-rmdir $(DESTDIR)$(MAN1DIR) $(DESTDIR)$(MANDIR) 2>/dev/null || true

.PHONY: all test clean install uninstall
