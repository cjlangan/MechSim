CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
PREFIX  ?= /usr/local

PKGS    := libpulse-simple sndfile
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS)) -lpthread -lm

BIN     := mechsim
SRC     := mechsim.c minijson.c

all: $(BIN)

$(BIN): $(SRC) minijson.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

# Synthetic keystroke injector used for the latency benchmark.
keyprobe: keyprobe.c
	$(CC) $(CFLAGS) -o $@ keyprobe.c

bench: $(BIN) keyprobe
	python3 bench.py

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm755 mechsim-toggle $(DESTDIR)$(PREFIX)/bin/mechsim-toggle
	install -d $(DESTDIR)$(PREFIX)/share/mechsim
	cp -r audio $(DESTDIR)$(PREFIX)/share/mechsim/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/mechsim-toggle
	rm -rf $(DESTDIR)$(PREFIX)/share/mechsim

clean:
	rm -f $(BIN) keyprobe

.PHONY: all install uninstall clean bench
