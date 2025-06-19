CC = gcc
CFLAGS = -Wall -Wextra -std=c99
CPPFLAGS = $(shell pkg-config --cflags libevdev)
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

# Package dependencies (Ubuntu/Debian)
PACKAGES = libjson-c-dev libpulse-dev libsndfile1-dev libinput-dev libevdev-dev libudev-dev

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
	@echo "Installing MechSim to /usr/local/bin..."
	sudo cp $(MECHSIM_TARGET) /usr/local/bin/
	sudo cp $(SOUND_TARGET) /usr/local/bin/
	sudo cp $(KEYBOARD_TARGET) /usr/local/bin/
	sudo mkdir -p /usr/local/share/mechsim
	sudo cp -r audio /usr/local/share/mechsim/
	@echo "Installation complete!"
	@echo "You can now run: mechsim"

.PHONY: all clean test install
