## The audio rewrite

The original opened a **new PulseAudio stream per keystroke**, after decoding the
sound file from disk inside the keystroke path:

1. `pthread_create` a thread
2. `sf_open()` + `sf_readf_short()`: reopen and decode the file
3. `pa_simple_new()`: new server connection
4. `pa_simple_write()` + `pa_simple_drain()`: write, then wait for it to finish
5. `pa_simple_free()`: tear the connection down

Disk I/O, a codec and a server handshake between the key and the click. This
branch instead:

- **decodes and resamples every sample once at startup** into memory at 48 kHz
- **keeps one stream** for the lifetime of the process
- **mixes with a 64-voice pool**, so fast typing overlaps instead of dropping keys
- **writes only while a voice is active**, which is the part that actually buys the latency
- **reads `/dev/input` directly** through evdev, so no root and no libinput

### Why the buffer is kept empty

The first version kept the sink topped up with silence in a free-running loop and
measured a stubborn ~25 ms regardless of period size or target buffer: 1, 3, 5, 10,
20, 30 and 50 ms targets all landed in the same place.

Latency here is mostly *how much audio is already queued ahead of your click*,
since queued silence has to play out before the click behind it is heard. Writing only
while a voice is active keeps the queue near-empty, which brought it to ~9 ms with
no other change. The stream idles between keystrokes, and a `pthread_cond_signal`
wakes the audio thread the instant a key registers a voice.

### Measured

ThinkPad T480 (i5-8250U), Void Linux, PipeWire 1.6.9 / `pipewire-pulse`, sink
`s32le 2ch 48000Hz`. Synthetic keystrokes via `/dev/uinput`, audio captured from
the sink monitor, both sides timestamped on `CLOCK_MONOTONIC`, one onset paired
per key. `make bench` reproduces it.

| | original | this branch |
|---|---|---|
| median | 60 ms | **8.9 ms** |
| p90 | 71 ms | 11.1 ms |
| max | stalls to ~2 s | 11.5 ms |
| CPU per keystroke | 3.6 ms | 5.4 ms |
| CPU idle | | 0.2 to 0.3 % of one core |
| resident memory | ~4 MB | 9.9 MB |

