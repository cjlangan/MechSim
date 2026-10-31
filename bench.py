#!/usr/bin/env python3
"""Latency benchmark for mechsim.

Injects synthetic keystrokes with ./keyprobe (needs /dev/uinput), captures the
default sink's monitor with parec, and pairs one audio onset per key. Reports
median/p90/max key-to-audible latency.

Run under `sg input -c 'python3 bench.py'` on a system where your login shell
has the input group but the shell you are measuring from does not.
"""
import os
import shutil
import statistics
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "mechsim")
KEYPROBE = os.path.join(HERE, "keyprobe")
RATE = 48000
CHUNK_MS = 2.5
BYTES = int(RATE * 2 * 4 * CHUNK_MS / 1000)  # s32le stereo


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


def bench(pack="eg-oreo", n=40, gap=400):
    app = subprocess.Popen(["sg", "input", "-c", f"exec {BIN} -s {pack}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.8)
    monitor = default_monitor()
    rec = subprocess.Popen(["parec", "--format=s32le", f"--rate={RATE}", "--channels=2",
                            "-d", monitor, "--raw", "--latency-msec=1"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
    time.sleep(0.9)

    chunks, stop = [], [False]

    def reader():
        while not stop[0]:
            data = rec.stdout.read(BYTES)
            if not data:
                break
            chunks.append((time.monotonic(), data))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(0.2)

    kp = subprocess.Popen(["sg", "input", "-c", f"{KEYPROBE} {n} {gap} 300"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True, bufsize=1)
    keys = []
    for line in kp.stdout:
        parts = line.split()
        if parts and parts[0] == "KEY":
            keys.append(float(parts[1]) / 1000.0)
    kp.wait()
    time.sleep(1.0)
    stop[0] = True
    th.join(2)
    rec.terminate()
    app.terminate()
    try:
        app.wait(timeout=5)
    except Exception:
        app.kill()

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
    if not shutil.which("parec"):
        sys.exit("parec not found (pulseaudio-utils)")
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
