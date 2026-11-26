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

## What's new in 2.0.0

- **Runs in the background.** `mechsim on` starts the player and gives you your
  terminal back.
- **One binary.** Start, stop, switch packs and set the volume with `mechsim`
  itself.
- **Remembers your settings** in a config file.
- **Low latency, no sudo.** A keystroke reaches the speaker in about 10 ms.
- **Mouse sounds** for clicks and the wheel, with optional separate mouse packs.

The command line has changed. If you're coming from an earlier version, read
[Upgrading to 2.0.0](#upgrading-to-200).

## Quick Usage Guide

```bash
mechsim on               # start in the background (eg-oreo at 50% by default)
mechsim packs            # list sound packs
mechsim pack turquoise   # switch packs
mechsim vol 60           # set the volume
mechsim off              # stop
```

## Installation

### Arch Linux (AUR)

```bash
yay -S mechsim
```

### Build From Source

```bash
git clone https://github.com/cjlangan/mechsim
cd mechsim
make
sudo make install
```

### Input permission

mechsim reads your keyboard from `/dev/input`, so your user needs to be in the
`input` group. This is the only permission it needs.

```bash
sudo usermod -a -G input "$USER"   # log out and back in afterwards
```

Run `mechsim` as your normal user, not with `sudo`. Note that membership in the
`input` group lets any program running as you read your keystrokes.

## Commands

    Usage: mechsim COMMAND [ARG]

| command | what it does |
|---|---|
| `on` | start the background player |
| `off` | stop it |
| `toggle` | start it if stopped, stop it if running |
| `rest`, `restart` | restart and re-read the config (starts it if stopped) |
| `status` | show what is playing; exit status 0 if on, 1 if off |
| `packs`, `list` | list sound packs: `*` is selected, `(playing)` is in use |
| `pack [NAME]` | show or switch the sound pack |
| `next`, `prev` | cycle through sound packs, skipping any that fail to load |
| `vol [N]`, `volume [N]` | show or set the volume, 0-100 |
| `mouse [on\|off\|toggle]` | show or set click and wheel sounds |
| `mousepack [NAME\|none]` | show or set the pack used for clicks |
| `help`, `-h`, `--help` | show help, including the paths in use |

Running `mechsim` with no command also shows the help.

Changes made with `pack`, `next`, `prev`, `vol`, `mouse` and `mousepack` are
saved to your config. If the player is on, it restarts to apply them. If it is
off, they apply the next time you turn it on.

```console
$ mechsim on
on (eg-oreo, 50%) pid 4213
$ mechsim status
on (pid 4213, pack eg-oreo, mouse borrowed, volume 50%)
```

## Configuration

Settings live in `~/.config/mechsim/config`, or
`$XDG_CONFIG_HOME/mechsim/config` if that variable is set. A commented default is
created the first time you turn the player on or change a setting.

```bash
PACK=eg-oreo
MOUSEPACK=""
VOLUME=50
MOUSE=1
NOTIFY=1
```

| key | default | meaning |
|---|---|---|
| `PACK` | `eg-oreo` | keyboard sound pack |
| `MOUSEPACK` | empty | pack for mouse sounds; empty borrows from the keyboard pack |
| `VOLUME` | `50` | 0-100 |
| `MOUSE` | `1` | `0` turns off click and wheel sounds |
| `NOTIFY` | `1` | `0` turns off desktop notifications |

After editing the file by hand, run `mechsim rest`. The file is read as plain
`KEY=value` lines, with optional quotes and comments. It is not run as a shell
script, so variables such as `$HOME` are not expanded.

## Keybinds and autostart

Hyprland (`hyprland.conf`):

```ini
exec-once = mechsim on
bind = SUPER, F10, exec, mechsim toggle
bind = SUPER, F11, exec, mechsim next
```

Sway or i3:

```
exec mechsim on
bindsym $mod+F10 exec mechsim toggle
bindsym $mod+F11 exec mechsim next
```

At login, `mechsim on` retries for up to 20 seconds while the audio server
starts.

Desktop notifications are sent with `notify-send` when it is installed.

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

## Custom sound packs

A pack is a folder containing a `config.json` and its audio files. The folder
name is the pack name. mechsim reads the Mechvibes V1 (`single` and `multi`) and
V2 (`definitions`) layouts.

- Keys are numeric Linux evdev codes. Add `-up` or `_up` for a release sound.
- Mouse entries can use `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseSide`,
  `MouseExtra`, `MouseWheelUp`, `MouseWheelDown`, `MouseWheelLeft` or
  `MouseWheelRight`.

Keyboard packs go in an `audio` directory and mouse packs in a `mousepacks`
directory. For each, mechsim uses the **first** of these that exists:

1. next to the `mechsim` binary
2. `../share/mechsim/` relative to the binary
3. `/usr/local/share/mechsim/`
4. `/usr/share/mechsim/`

To use a different directory, set `MECHSIM_AUDIO_DIR` or `MECHSIM_MOUSE_DIR`.
Set them wherever you run `mechsim`, including keybinds and autostart.

## Troubleshooting

The player writes its log to `$XDG_RUNTIME_DIR/mechsim.log`, which starts fresh
each time the player starts. Without `XDG_RUNTIME_DIR`, look in `/run/user/<uid>/`
or `/tmp/mechsim-<uid>/`.

| problem | try |
|---|---|
| `no readable keyboard devices` | add yourself to the [input group](#input-permission), then log out and back in |
| `no such sound pack` | run `mechsim packs`; `mechsim help` shows which directory is searched |
| `cannot open audio output` | check that PulseAudio, or PipeWire with `pipewire-pulse`, is running |
| sounds play twice | an older player is still running: `pkill -x mechsim`, then `mechsim on` |

Developers can set `MECHSIM_TRACE=1` when starting the player to log key and mix
timestamps for latency measurements.

## Upgrading to 2.0.0

**Stop the old player first** with `mechsim-toggle off` (or `pkill -x mechsim`).
2.0.0 does not detect a player started by an earlier version.

Then update any keybinds, autostart entries and scripts. `mechsim-toggle` is
replaced by `mechsim`, with the same command names.

| before | now |
|---|---|
| `mechsim` (start playing) | `mechsim on` |
| `mechsim-toggle` | `mechsim toggle` |
| `mechsim-toggle <command>` | `mechsim <command>` |
| `-s NAME`, `--sound` | `mechsim pack NAME` |
| `-V N`, `--volume` | `mechsim vol N` |
| `-l`, `--list` | `mechsim packs` |
| `-m`, `--no-mouse` | `mechsim mouse off` |
| `-M DIR`, `--mouse-pack` | `mechsim mousepack NAME` (a name, not a path) |
| `-d DIR`, `--data-dir` | `MECHSIM_AUDIO_DIR=DIR` |
| `-v`, `--verbose` | check the log file |

Your existing config file still works, as long as it doesn't rely on shell
variables or commands.

## Dependencies

- build-essential
- pkg-config
- libpulse-dev
- libsndfile1-dev
- libnotify-bin (optional, for notifications)

## License

MIT, as upstream. Sound packs come from [Mechvibes](https://github.com/hainguyents13/mechvibes).
