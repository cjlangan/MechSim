## mechsim-toggle

***In development, to be added to mechsim binary soon.***

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
