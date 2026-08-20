# N64 hi-res texture packs on a 2 GB Pi 5

## HD works, 4K does not

| Tier | Result |
|---|---|
| HD | Stable. Ocarina (9.5 GB pack) plateaus at 286 MB resident, Majora (3.9 GB) at 307 MB, both flat over six minutes with 1.2 GB free. |
| 4K | Fails. Ocarina (30 GB) OOM-killed system daemons during real play and RetroArch segfaulted. Majora (22 GB) with the texture cache bounded to 1500 produced 12 OOM kills and died after 200 s. |

Reducing `MaxTxCacheSize` does not rescue 4K, and the measurement says why:

```
  t(s)   RSS_MB  avail_MB
    20      248      1450
    60      285       711
   100      258       507
```

Process memory stays under 290 MB while *system* memory collapses. The pressure
is the kernel page cache backing the memory-mapped pack, not the emulator's own
texture cache, so no emulator setting reaches it. This is a memory wall, not a
tuning problem.

## The setting that made packs load at all

GLideN64 stamps a config word into every cache and rejects any pack whose word
differs - and rejection *clears* the pack rather than leaving it alone. Every
distributed pack carries `0x40220000`. RetroPie ships `txCacheCompression=True`,
which adds `0x00800000` and yields `0x40A20000`, so nothing loaded and packs
were being destroyed on contact.

```
txHiresEnable                = True
EnableEnhancedHighResStorage = True
txHiresFullAlphaChannel      = True
txCacheCompression           = False
EnableHiResAltCRC            = True
```

## Handling the packs

`contrib/pi5/n64-texture-tier.sh "<PACK NAME>" hd|4k` switches a pack's active
tier by hardlinking, so it costs no disk and is reversible.

Two rules learned by losing 9.5 GB twice:

- The cache directory must be `dr-xr-xr-x`. File permissions do not govern
  deletion - directory write permission does, so `chmod 444` on the files is no
  protection at all against a stray `rm`.
- Verify the target tier exists *before* unlinking the current one, and keep the
  current data reachable under a second name first.
