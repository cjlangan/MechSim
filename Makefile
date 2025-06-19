CC = gcc
CFLAGS = -Wall -Wextra -std=c99
PREFIX ?= /usr

# Pass PACKAGE_PREFIX macro for config.h
CPPFLAGS = -DPACKAGE_PREFIX=\"$(PREFIX)\" $(shell pkg-config --cflags libevdev)

LDFLAGS_SOUND = -ljson-c -lpulse -lpulse-simple -lsndfile -lpthread
LDFLAGS_KEYBOARD = $(shell pkg-config --libs libevdev libinput libudev) -lpthread

# Targets
MECHSIM_TARGET = mechsim
SOUND_TARGET = keyboard_sound_player
KEYBOARD_TARGET = get_key_presses

# Sources
MECHSIM_SOURCE = mechsim.c
SOUND_SOURCE = keyboard_sound_player.c
KEYBOARD_SOURCE = get_key_presses.c

# Install paths
BINDIR = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/mechsim

all: $(MECHSIM_TARGET) $(SOUND_TARGET) $(KEYBOARD_TARGET)

$(MECHSIM_TARGET): $(MECHSIM_SOURCE)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $<

$(SOUND_TARGET): $(SOUND_SOURCE)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS_SOUND)

$(KEYBOARD_TARGET): $(KEYBOARD_SOURCE)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS_KEYBOARD)

clean:
	rm -f $(MECHSIM_TARGET) $(SOUND_TARGET) $(KEYBOARD_TARGET)

test: all
	@echo "Testing sound packs:"
	./$(MECHSIM_TARGET) --list
	@echo ""
	@echo "To run MechSim:"
	@echo "  sudo ./$(MECHSIM_TARGET)                    # Default sound"
	@echo "  sudo ./$(MECHSIM_TARGET) -s cherrymx-blue-abs  # Specific sound"
	@echo "  sudo ./$(MECHSIM_TARGET) --help             # Show help"

install:
	@echo "Installing MechSim to $(DESTDIR)$(BINDIR) and $(DESTDIR)$(SHAREDIR)..."
	install -Dm755 $(MECHSIM_TARGET) $(DESTDIR)$(BINDIR)/$(MECHSIM_TARGET)
	install -Dm755 $(SOUND_TARGET) $(DESTDIR)$(BINDIR)/$(SOUND_TARGET)
	install -Dm755 $(KEYBOARD_TARGET) $(DESTDIR)$(BINDIR)/$(KEYBOARD_TARGET)
	install -d $(DESTDIR)$(SHAREDIR)
	cp -r audio $(DESTDIR)$(SHAREDIR)/
	@echo "Installation complete."

uninstall:
	@echo "Uninstalling MechSim from $(DESTDIR)$(BINDIR) and $(DESTDIR)$(SHAREDIR)..."
	rm -f $(DESTDIR)$(BINDIR)/$(MECHSIM_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(SOUND_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(KEYBOARD_TARGET)
	rm -rf $(DESTDIR)$(SHAREDIR)
	@echo "Uninstallation complete."

.PHONY: all clean test install uninstall
