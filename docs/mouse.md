## Mouse and wheel

Left, right and middle clicks, the side buttons, and the scroll wheel make sound
too. Pass `-m` / `--no-mouse` to ignore them and capture keyboards only, or use
`mechsim-toggle mouse off` to flip it while the player keeps running; the setting
is remembered and the keyboard is unaffected either way.

### Click sounds from a separate pack

Keyboard and mouse can use different packs, which is how MechVibes organises
things (it ships `soundpacks/mouse/`). Point `-M` at a directory:

```
mechsim -s nk-cream -M ./mousepacks/ping
```

`mousepacks/` here holds the four MechVibes mouse packs (ping, chat, vibrate,
wooden) in their original V2 config format, which this loader reads directly:
`definition_method`, `audio_file`, and `definitions` with `timing` ranges where
the first pair is the press and the second, if present, the release.

Packs can instead name mouse entries inside the keyboard pack itself
(`MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseWheelUp`, `MouseWheelDown`,
`MouseWheelLeft`, `MouseWheelRight`), and numeric evdev codes work as before.
When nothing defines a given button, the click borrows a keyboard sample from
the same pack: the generic random press in `multi` packs, or the space bar
slice in `single` packs. Clicks stay audible without a second pack to manage,
and anything a pack does define wins over the borrowed sample.

