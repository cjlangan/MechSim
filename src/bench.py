#!/usr/bin/env python3
"""Latency benchmark for mechsim.

Injects synthetic keystrokes with ./keyprobe (needs /dev/uinput), captures the
default sink's monitor with parec, and pairs one audio onset per key. Reports
median/p90/max key-to-audible latency.

Run `python3 bench.py` as your normal user (not under sudo). The player and
keyprobe are started with `sudo -g input`, so the shell you are measuring from
does not need the input group. The player gets a temporary config, leaving yours
untouched. Stop any running player first with `mechsim off`.
"""
import contextlib
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "../mechsim")
KEYPROBE = os.path.join(HERE, "../keyprobe")
RATE = 48000
CHUNK_MS = 2.5
BYTES = int(RATE * 2 * 4 * CHUNK_MS / 1000)  # s32le stereo
GROUP = "input"


def as_group(*cmd):
    return ["sudo", "-g", GROUP, *cmd]


@contextlib.contextmanager
def player(pack):
    """Run the background player with this pack for the duration of the block."""
    home = os.path.expanduser("~")
    config_home = os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    with tempfile.TemporaryDirectory(prefix="mechsim-bench-") as tmp:
        os.mkdir(os.path.join(tmp, "mechsim"))
        with open(os.path.join(tmp, "mechsim", "config"), "w") as f:
            # MOUSE=0 keeps real clicks out of the recording.
            f.write(f'PACK="{pack}"\nVOLUME=50\nMOUSE=0\nNOTIFY=0\n')
        # PulseAudio client settings live under XDG_CONFIG_HOME too.
        pulse = os.path.join(config_home, "pulse")
        if os.path.isdir(pulse):
            os.symlink(pulse, os.path.join(tmp, "pulse"))

        env = {k: v for k, v in os.environ.items()
               if k.startswith("PULSE_") or k == "MECHSIM_AUDIO_DIR"}
        env["HOME"] = home
        env["XDG_RUNTIME_DIR"] = (os.environ.get("XDG_RUNTIME_DIR")
                                  or f"/run/user/{os.getuid()}")
        env["XDG_CONFIG_HOME"] = tmp
        # sudo resets the environment, so hand it over explicitly.
        start = as_group("env", *(f"{k}={v}" for k, v in env.items()), BIN, "on")

        ours = True
        try:
            # `on` returns once the player has opened audio and input.
            res = subprocess.run(start, capture_output=True, text=True)
            if res.stdout.startswith("already on"):
                ours = False
                sys.exit("a mechsim player is already running; run `mechsim off` first")
            if res.returncode != 0:
                sys.exit(f"could not start mechsim with pack {pack}:\n{res.stderr.strip()}")
            yield
        finally:
            if ours:
                subprocess.run([BIN, "off"], env={**os.environ, **env},
                               stdout=subprocess.DEVNULL)


def default_monitor():
    out = subprocess.run(["pactl", "info"], capture_output=True, text=True).stdout
    sink = None
    for line in out.splitlines():
        if line.startswith("Default Sink:"):
            sink = line.split(":", 1)[1].strip()
    if not sink:
        sys.exit("could not determine the default sink from `pactl info`")
    return sink + ".monitor"


def sink_format(sink):
    out = subprocess.run(["pactl", "list", "sinks"], capture_output=True, text=True).stdout
    cur, spec = None, None
    for line in out.splitlines():
        if line.strip().startswith("Name:"):
            cur = line.split(":", 1)[1].strip()
        elif cur == sink and "Sample Specification:" in line:
            spec = line.split(":", 1)[1].strip()
            break
    return spec


def pair(keys, events, gap):
    lat, used = [], set()
    window = min(gap / 1000.0, 0.30)
    for k in keys:
        for i, (start, _end) in enumerate(events):
            if i in used:
                continue
            if 0 <= start - k < window:
                lat.append((start - k) * 1000)
                used.add(i)
                break
    return sorted(lat)


def record(monitor, n, gap):
    """Inject n keys while recording; return (key times, timestamped audio)."""
    rec = subprocess.Popen(["parec", "--format=s32le", f"--rate={RATE}", "--channels=2",
                            "-d", monitor, "--raw", "--latency-msec=1"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
    chunks, stop = [], [False]

    def reader():
        while not stop[0]:
            data = rec.stdout.read(BYTES)
            if not data:
                break
            chunks.append((time.monotonic(), data))

    th = threading.Thread(target=reader, daemon=True)
    try:
        time.sleep(0.9)
        th.start()
        time.sleep(0.2)

        kp = subprocess.Popen(as_group(KEYPROBE, str(n), str(gap), "300"),
                              stdout=subprocess.PIPE, text=True, bufsize=1)
        keys = []
        for line in kp.stdout:
            parts = line.split()
            if parts and parts[0] == "KEY":
                keys.append(float(parts[1]) / 1000.0)
        if kp.wait() != 0:
            sys.exit(f"keyprobe failed with exit status {kp.returncode}")
        time.sleep(1.0)
        stop[0] = True
        th.join(2)
    finally:
        stop[0] = True
        rec.terminate()
        rec.wait()
    return keys, chunks


def bench(pack="eg-oreo", n=40, gap=400):
    with player(pack):
        time.sleep(0.5)
        keys, chunks = record(default_monitor(), n, gap)

    peaks = []
    for ts, data in chunks:
        vals = struct.unpack("<%di" % (len(data) // 4), data)
        peaks.append((ts, max(abs(x) for x in vals)))
    peak = max((p for _, p in peaks), default=0)
    if peak == 0:
        return 0, [], 0

    events = []
    for ts, amp in peaks:
        if amp > peak * 0.05:
            if events and ts - events[-1][1] < 0.06:
                events[-1][1] = ts
            else:
                events.append([ts, ts])

    return len(keys), pair(keys, events, gap), len(events)


def main():
    if os.geteuid() == 0:
        sys.exit("run this as your normal user; it calls sudo itself")
    if not shutil.which("parec"):
        sys.exit("parec not found (pulseaudio-utils)")
    for path in (BIN, KEYPROBE):
        if not os.access(path, os.X_OK):
            sys.exit(f"not found or not executable: {path}")
    # Ask for any password now rather than in the middle of a measurement.
    if subprocess.run(as_group("true")).returncode != 0:
        sys.exit(f"`sudo -g {GROUP}` failed")
    monitor = default_monitor()
    sink = monitor[: -len(".monitor")]
    print(f"binary:  {BIN}")
    print(f"monitor: {monitor}  ({sink_format(sink)})")
    print(f"injector:{KEYPROBE}\n")
    print(f'{"pack":<16}{"gap":>6}{"used":>7}{"median":>9}{"p90":>8}{"max":>8}')
    for pack in ["eg-oreo", "cream-travel"]:
        for gap in [150, 400]:
            nkeys, lat, nev = bench(pack, gap=gap)
            if not lat:
                print(f"{pack:<16}{gap:>6}{0:>7}   no match ({nkeys} keys, {nev} events)")
                continue
            print(f'{pack:<16}{gap:>6}{len(lat):>7}{statistics.median(lat):>9.1f}'
                  f'{lat[int(len(lat) * 0.9)]:>8.1f}{lat[-1]:>8.1f}')


if __name__ == "__main__":
    main()
