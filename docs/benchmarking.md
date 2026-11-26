## Benchmarking

```bash
make keyprobe        # synthetic keyboard injector, uses /dev/uinput
make bench           # 40 keys, reports median/p90/max
```

`bench.py` captures the default sink's monitor and pairs one audio onset per key.
It prints the matched count beside the percentiles: packs that ship press *and*
release sounds emit two events per key, so a low count means the pairing needs a
look, not that the player is slow.

