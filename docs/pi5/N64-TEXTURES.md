# N64 hi-res texture packs on a 2 GB Pi 5

## HD works; 4K works only with hi-res VRAM bounded

| Tier | Result |
|---|---|
| HD | Stable. Ocarina (9.5 GB pack) plateaus at 286 MB resident, Majora (3.9 GB) at 307 MB, both flat over six minutes with 1.2 GB free. |
| 4K | Fails. Ocarina (30 GB) OOM-killed system daemons during real play and RetroArch segfaulted. Majora (22 GB) with the texture cache bounded to 1500 produced 12 OOM kills and died after 200 s. |

This table describes the **unbounded** default, `MaxHiResTxVramLimit = 0`.
See the correction at the end: bounding it makes 4K work.

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

## Swap does not help, and the reason rules the approach out

The pressure is unswappable, so adding swap of any kind cannot reach it. Majora
4K under 1.2 GB of zram (lz4, swappiness 150), on top of the 3.2 GB of disk swap
the board already carries:

```
   t(s)   RSS_MB  avail_MB  Shmem_MB  SwapUsed_MB
     20      249      1365       143           86
     60      283       710       835           91
    120       33       492      1211          313
```

Dead at 140 s with six OOM kills, and only 14 MB of the zram device was ever
populated. Shmem grows monotonically to 1.2 GB while the emulator's own resident
set peaks at 283 MB and then *collapses to 33 MB* - the process is paged out to
feed GPU allocations and killed anyway. Those allocations are shmem-backed GEM
buffers, pinned so the GPU can DMA into them, so the swapper cannot touch them.

That also explains the earlier result that bounding `MaxTxCacheSize` to 1500
changed nothing: the knob governs textures converted from the ROM at runtime,
not the hi-res pack, and neither one is where the memory goes.

Pack size remains the only predictor - 302 MB and 2.1 GB load, 22 GB and 30 GB
do not. The only untried lever is a pack rebuilt with fewer textures.

## Correction: 4K does work, with hi-res VRAM bounded

The conclusion above was wrong about the cause being out of reach. It was drawn
from `MaxTxCacheSize`, which the core documents as *"Set Max texture cache size
(in elements)"* - a count of RAM cache entries. It never governed the GPU side.

`MaxHiResTxVramLimit` does: *"Limit High-Res textures size in VRAM (in MB,
0 = no limit)"*. It had been sitting at its default of 0 - unlimited - the whole
time, which is precisely why Shmem climbed until the OOM killer intervened.
The smallest value the core offers is 500.

Majora's 21 GB 4K pack, same scene, same duration:

| `MaxHiResTxVramLimit` | Shmem | Outcome |
|---|---|---|
| 0 (unlimited) | 143 -> 1211 MB, climbing | dead at 140 s, 6 OOM kills |
| 500 | plateaus ~600 MB, flat | survived 240 s, 0 OOM kills |

A plateau alone would also be consistent with the cap silently starving hi-res
loading, which would make the survival worthless. It does not - same cap, same
game, same 120 s:

| Tier | Shmem delta |
|---|---|
| HD | +190 MB |
| 4K | +660 MB |

3.5x more texture data resident on 4K, so the pack is genuinely loading and the
cap is bounding runaway growth rather than suppressing it.

The zram refutation above is what led here: proving the memory was pinned GPU
buffers rather than anything swappable is what redirected the search from cache
sizing to a GPU-side limit.

Cost of the cap: textures are evicted and reloaded during play, so expect some
pop-in or hitching that HD does not have. That is the price of fitting 4K into
2 GB.

## Per-game soak at the 500 MB cap

Six minutes each, 4K tier, nothing else running. Any game that died was to be
reverted to HD automatically; none did.

| Game | 4K pack | Peak Shmem | OOM kills | Result |
|---|---|---|---|---|
| Super Mario 64 | 4.9 GB | 648 MB | 0 | survived |
| Mario Kart 64 | 12 GB | 638 MB | 0 | survived |
| Majora's Mask | 21 GB | 676 MB | 0 | survived |
| Resident Evil 2 | 16 GB | 211 MB | 0 | survived |
| Ocarina (single pack) | 9.4 GB | 228 MB | 0 | survived |

Mario 64 and Mario Kart had both failed outright before the cap existed.

## Why the cap stays at 500 rather than 1000

1000 MB also survives 6 minutes with no OOM kills, but the margin is not
there:

| | cap 500 | cap 1000 | unbounded (fatal) |
|---|---|---|---|
| Shmem plateau | ~650 MB | 1090 MB | 1211 MB |
| Available memory | ~900 MB | 517 MB | 507 MB before death |
| Emulator RSS | steady ~290 MB | falls to 88 MB | falls to 33 MB |

At 1000 the emulator's own resident set collapses to 88 MB - it is being paged
out to feed textures, the same pattern that preceded the OOM kills, just not
yet fatal. 500 leaves ~900 MB free with the frontend still to account for.

The core offers only 0/500/1000/1500/... - `MaxHiResTxVramLimit` is parsed
with `atoi`, so an intermediate value would reach the core, but RetroArch
validates against its declared list first and falls back to the default of 0,
which is unlimited. Verify any intermediate value by checking the Shmem
plateau rather than trusting the config file.

Ocarina has no 4K tier on disk - only the 9.4 GB pack. The 30 GB 4K pack this
document refers to above is gone.
