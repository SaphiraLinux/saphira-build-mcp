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

# Install location follows privilege unless overridden: root installs
# system-wide (sudo make install -> /usr/bin), an ordinary user
# installs into their own tree (make install -> ~/.local/bin).
# Explicit PREFIX=... (environment or command line) always wins, and
# DESTDIR still applies on top for staged installs.
ifeq ($(origin PREFIX),undefined)
ifeq ($(shell id -u 2>/dev/null),0)
PREFIX := /usr
else
PREFIX := $(or $(HOME),/tmp)/.local
endif
endif
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
MAN1DIR  ?= $(MANDIR)/man1
DESTDIR  ?=

SRC := src/main.c src/mcp.c src/json.c src/gbuf.c src/db.c src/conf.c src/runner.c src/sha256.c src/tools.c
OBJ := $(SRC:.c=.o)
BIN := saphira-build-mcp

# Windows cross builds (MinGW-w64 on a Linux host; developed and
# Wine-tested on Arch). _WIN32 is predefined by the compiler, so no
# -D flag is needed for the compat paths. SQLITE_SRC points at the
# vendored SQLite amalgamation for the builtin target; SQLITE3_INC
# and SQLITE3_LIB point at the matching import library and headers
# for the dynamic target (standard MinGW sysroot layout by default,
# override both together).
MINGW_CC    ?= x86_64-w64-mingw32-gcc
MINGW_STRIP ?= x86_64-w64-mingw32-strip
WIN_CFLAGS  ?= -std=gnu11 -Os -Wall -Wextra
SQLITE3_INC ?= /usr/x86_64-w64-mingw32/include
SQLITE3_LIB ?= /usr/x86_64-w64-mingw32/lib
SQLITE_SRC  ?=
WIN_SQLITE_FLAGS ?= -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1

all: $(BIN)

windows: $(SRC)
	$(MINGW_CC) $(WIN_CFLAGS) -I$(SQLITE3_INC) $(SRC) -o saphira-build-mcp.windows.exe -L$(SQLITE3_LIB) -lsqlite3
	$(MINGW_STRIP) saphira-build-mcp.windows.exe

windows-builtin: $(SRC)
	@if [ -z "$(SQLITE_SRC)" ]; then echo "windows-builtin needs SQLITE_SRC=<sqlite3.c path>" >&2; exit 1; fi
	$(MINGW_CC) $(WIN_CFLAGS) $(WIN_SQLITE_FLAGS) -I$(dir $(SQLITE_SRC)) $(SRC) $(SQLITE_SRC) -o saphira-build-mcp.windows-builtin.exe
	$(MINGW_STRIP) saphira-build-mcp.windows-builtin.exe

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)
	strip $@

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	sh tests/test.sh

clean:
	rm -f $(OBJ) $(BIN) saphira-build-mcp.windows.exe saphira-build-mcp.windows-builtin.exe

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MAN1DIR)
	install -m 0644 saphira-build-mcp.1 $(DESTDIR)$(MAN1DIR)/$(BIN).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MAN1DIR)/$(BIN).1
	-rmdir $(DESTDIR)$(MAN1DIR) $(DESTDIR)$(MANDIR) 2>/dev/null || true

.PHONY: all test clean install uninstall windows windows-builtin
