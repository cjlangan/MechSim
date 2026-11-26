<h1 align="center">mechsim</h1>

<div align="center">
<p>
<a href="https://github.com/cjlangan/mechsim/stargazers"><img src="https://img.shields.io/github/stars/cjlangan/mechsim?style=for-the-badge&logo=starship&color=C9CBFF&logoColor=C9CBFF&labelColor=302D41" alt="stars"><a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/"><img src="https://img.shields.io/github/repo-size/cjlangan/mechsim?style=for-the-badge&logo=hyprland&logoColor=f9e2af&label=Size&labelColor=302D41&color=f9e2af" alt="REPO SIZE"></a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/releases"><img src="https://img.shields.io/github/v/release/cjlangan/mechsim?style=for-the-badge&logo=github&logoColor=eba0ac&label=Release&labelColor=302D41&color=eba0ac" alt="Releases"></a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/blob/main/LICENSE"><img src="https://img.shields.io/github/license/cjlangan/mechsim?style=for-the-badge&logo=&color=CBA6F7&logoColor=CBA6F7&labelColor=302D41" alt="LICENSE"></a>&nbsp;&nbsp;
</p>
</div>

<p align="center">A CLI-Based Mechanical Keyboard Sound Simulator</p>

<br>

https://github.com/user-attachments/assets/622ff7b3-11c9-4f51-be4d-5f19565adbf5

<div align="center">
<p align="center">The video shows just a few of the many options</p>
</div>

> **This branch replaces the audio path.** Same CLI, same sound packs, but a
> keystroke now reaches the speaker in ~9 ms instead of ~60 ms, and it runs
> without sudo. See [The audio rewrite](#the-audio-rewrite) for what changed and
> why.

## Quick Usage Guide

```bash
mechsim         # default uses sound eg-oreo at volume 50

mechsim -l      # list out all sound packs

mechsim -s turquoise -V 60  # choose soundpack turquoise at 60% volume
```

## Installation

### Arch Linux (AUR)

```bash
yay -S mechsim
```

### Build From Source

```bash
git clone https://github.com/Splayer0/MechSim
cd MechSim
make
sudo make install
```

Then make sure you can read `/dev/input`, which is all the permission this needs:

```bash
sudo usermod -a -G input "$USER"   # log out and back in afterwards
```

No `sudo` is required to *run* it. Only `libinput`, which this branch no longer
uses, needed that.

## Full Usage

    Usage: mechsim [OPTIONS]

    Options:
      -s, --sound SOUND_NAME   Select sound pack (default: eg-oreo)
      -V, --volume VOLUME      Set volume [0-100] (default: 50)
      -d, --data-dir DIR       Directory holding the sound packs
      -M, --mouse-pack DIR     Separate pack directory for click sounds
      -m, --no-mouse           Ignore mouse buttons and the wheel
      -l, --list               List available sound packs
      -h, --help               Show this help message
      -v, --verbose            Enable verbose output

    Examples:
      mechsim                       # Use default sound (eg-oreo)
      mechsim -s cherrymx-blue-abs  # Use Cherry MX Blue ABS sound
      mechsim -l                    # List all available sounds

It runs in the foreground until Ctrl+C. `mechsim-toggle` (below) daemonises it
and is what you want for a keybind.

## Available Sounds

All 18 packs, ordered from deep thock to sharp click. The numbers are measured
with `sox stat` on a real click from each pack, not guessed from the names.
**bright Hz** is rough spectral frequency, **peak** is amplitude, so you can
compare loudness without turning the volume up and down.

| pack | bright Hz | length | peak | character |
|---|---:|---:|---:|---|
| nk-cream | 1530 | 206 ms | 0.22 | deepest thock, long tail |
| cherrymx-red-pbt | 1840 | 158 ms | 0.34 | linear, soft, dry |
| cherrymx-red-abs | 1840 | 151 ms | 0.17 | same pitch, much quieter |
| holy-pandas | 2370 | 104 ms | 0.32 | short, snappy thock |
| mxbrown-travel | 2370 | 130 ms | 0.22 | muted tactile |
| cream-travel | 2800 | 130 ms | 0.34 | mid, punchy |
| topre-purple-hybrid-pbt | 2890 | 177 ms | 0.19 | Topre-style soft thump |
| turquoise | 2950 | 104 ms | 0.19 | short tactile tick, quiet |
| cherrymx-black-pbt | 2990 | 160 ms | 0.40 | linear, fuller |
| cherrymx-black-abs | 3020 | 99 ms | 0.39 | shorter, sharper |
| eg-oreo | 3200 | 109 ms | 0.13 | thin and quiet (default) |
| mxblack-travel | 3230 | 104 ms | 0.24 | linear, light |
| cherrymx-blue-pbt | 3380 | 164 ms | 0.43 | clicky, loud |
| cherrymx-brown-pbt | 3420 | 198 ms | 0.30 | long tactile |
| eg-crystal-purple | 3800 | 129 ms | 0.12 | bright but very quiet |
| mxblue-travel | 3830 | 104 ms | 0.45 | clicky, hard hit |
| cherrymx-brown-abs | 4400 | 167 ms | 0.93 | very bright, very loud |
| cherrymx-blue-abs | 5950 | 196 ms | 0.91 | classic sharp click |

Packs differ a lot in loudness (peak 0.12 to 0.93), so adjust volume when you
switch between them.

## Dependencies

- build-essential
- pkg-config
- libpulse-dev
- libsndfile1-dev

## mechsim-toggle

A small shell wrapper so it can live behind a keybind instead of a terminal:

```bash
mechsim-toggle            # flip: start, or stop if running
mechsim-toggle on
mechsim-toggle off
mechsim-toggle rest       # restart, re-reading the config
mechsim-toggle status     # "on (pid …, pack …, volume …%)" or "off"; exit 0 if on
mechsim-toggle packs      # list packs, * marks the current one
mechsim-toggle pack NAME  # switch pack and restart
mechsim-toggle next       # cycle packs (prev goes backwards)
mechsim-toggle vol 40     # volume 0-100
mechsim-toggle mouse       # show whether clicks are on
mechsim-toggle mouse off   # mute clicks and wheel, keyboard keeps working
mechsim-toggle mouse toggle
mechsim-toggle mousepack  # list click packs
mechsim-toggle mousepack ping   # clicks use mousepacks/ping
mechsim-toggle mousepack none   # borrow clicks from the keyboard pack
mechsim-toggle help
```

Settings live in `~/.config/mechsim/config`:

```bash
PACK=nk-cream
VOLUME=45
NOTIFY=1     # 0 to suppress desktop notifications
```

`start` waits for a real sink to exist before launching, because at login PipeWire
can answer before the ALSA card is bound, and it only reports success once the
player prints `ready:`, so a missing `input` group fails loudly instead of running
silently. Put it on your `PATH` if you want it by name:

```bash
ln -s "$PWD/mechsim-toggle" ~/.local/bin/mechsim-toggle
```

For niri, in `~/.config/niri/config.kdl`:

```kdl
spawn-sh-at-startup "~/path/to/MechSim/mechsim-toggle on"
Mod+Shift+S hotkey-overlay-title="Toggle Keyboard Sounds" { spawn-sh "~/path/to/MechSim/mechsim-toggle"; }
```

## License

MIT, as upstream. Sound packs come from [Mechvibes](https://github.com/hainguyents13/mechvibes).
