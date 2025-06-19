# MechSim - Mechanical Keyboard Sound Simulator

### Wayland Compatible!



## Usage

```bash
mechsim # default uses sound eg-oreo at volume 20
```

## Full Usage

    Usage: mechsim [OPTIONS]

    Options:
      -s, --sound SOUND_NAME   Select sound pack (default: eg-oreo)
      -V, --volume VOLUME      Set volume [0-100] (default: 20)
      -l, --list               List available sound packs
      -h, --help               Show this help message
      -v, --verbose            Enable verbose output

    Examples:
      mechsim                       # Use default sound (eg-oreo)
      mechsim -s cherrymx-blue-abs  # Use Cherry MX Blue ABS sound
      mechsim -l                    # List all available sounds


## Dependencies

- build-essential
- pkg-config
- libjson-c-dev
- libpulse-dev
- libsndfile1-dev
- libinput-dev
- libevdev-dev
- libudev-d


## Build & Install

```bash
git clone https://github.com/cjlangan/mechsim
cd mechsim
make
make install
```

## Motivation 

The motivation behind this project was that I couldn't hear my keybaord presses
in screen-recordings or when I had headphones on. I tried other programs such
as [MechVibes](https://github.com/hainguyents13/mechvibes) (which I took the
sound files and configs from) -- but none were Wayland compatible.

I decided to take some backend code from [Show Me The
Key](https://github.com/AlynxZhou/showmethekey) which could detect global key
presses -- even on Wayland -- and fit it to my use case.

Admittedly, I used a lot of AI to help me complete this, since I was more
interested in just getting it to work to enjoy the sweet sweet keypresses. But
I have since decided to share the code if others found themselves in a similar
position.
