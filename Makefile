CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
PREFIX  ?= /usr/local
SRC_DIR := src

PKGS    := libpulse-simple sndfile
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS)) -lpthread -lm

BIN     := mechsim
SRC     := $(SRC_DIR)/mechsim.c $(SRC_DIR)/minijson.c

all: $(BIN)

$(BIN): $(SRC) $(SRC_DIR)/minijson.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

# Synthetic keystroke injector used for the latency benchmark.
keyprobe: $(SRC_DIR)/keyprobe.c
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/keyprobe.c

bench: $(BIN) keyprobe
	python3 $(SRC_DIR)/bench.py

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -d $(DESTDIR)$(PREFIX)/share/mechsim
	cp -r audio $(DESTDIR)$(PREFIX)/share/mechsim/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(PREFIX)/share/mechsim

clean:
	rm -f $(BIN) keyprobe

.PHONY: all install uninstall clean bench
