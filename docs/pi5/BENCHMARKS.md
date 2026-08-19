# Pi 5 benchmark ledger

Every performance claim on the Pi 5 branch traces back to a row here. The
rules that keep the numbers honest:

- **One variable at a time.** A/B pairs run on the same binary whenever the
  change is runtime-gated (env var / config), otherwise consecutive builds
  with only the change under test between them.
- **Bounded, repeatable runs.** `contrib/pi5/bench.sh` (175 s, Halo 2, 1080p
  output, `timeout -s KILL`), analysed with `scripts/perflog_report.py`
  (`summary` for one run, `compare` for a pair — its noise verdict decides
  whether a delta is real).
- **Fairness check.** Compare the peak `xtrace: surface_update count=` between
  the runs. A run whose guest did 1000x less drawing was starved; its fps is
  meaningless however plausible it looks. `perflog_report.py compare` flags
  this automatically from the sibling `.log`.
- **Record the binary.** `xemu_commit:` from the run log identifies what
  actually ran; a benchmark of a stale binary has happened once already.

## Environment

Raspberry Pi 5, 2 GB, Cortex-A76 4x2.8 GHz, V3D 7.1 @ 1350 MHz (VideoCore
VII), V3DV Mesa Vulkan 1.2, CMA 512 MB, Debian 12. Benchmark title: Halo 2
(caps at 30 fps — the "native" target for this table).

## Ledger

| # | Change | Before | After | Verdict |
|---|--------|--------|-------|---------|
| 1 | Index-buffer clamp (bounded allocation) | 855 MiB reserved, 254 tex evictions | 580 MiB, 0 evictions | kept |
| 2 | 1080p output instead of 4K | — | +24% fps | kept (launcher forces 1080p) |
| 3 | V3D 1350 MHz | — | +9% fps, readback 18.14 -> 14.57 ms | kept |
| 4 | -O3 | — | +1.1% | kept |
| 5 | LTO | — | +1.0% | kept |
| 6 | Fence instead of vkQueueWaitIdle (aux submits) | — | no change | kept for correctness |
| 7 | Non-blocking presenter on the readback path | 16.6 fps | 6.3 fps | **reverted** — starved the upload |
| 8 | Vulkan swapchain presentation (`XEMU_DISPLAY_BACKEND=vulkan`) | 18.0-18.4 fps, readback 10.9-11.7 ms, 1% low 7.8 | 19.4-19.9 fps, readback 0, 1% low 9.2 | kept, opt-in (+7% fps, +18% 1% low) |
| 9 | Deferred flip-stall wait, first attempt (`XEMU_ASYNC_FLIP=1`) | 19.8 fps | 5.8 fps, assert, guest starved 1000x | **broken** — reclaim ran after next-draw allocation; fix at HEAD, untested |
| 22 | TSO soak 2: Morrowind (200 s) + **ship** | — | 0 errors, banner active, guest 29-52 flips/s, 40 fps presented | PASS — `XEMU_TSO=1` enabled in the RetroPie launcher |
| 21 | TSO soak 1: GTA San Andreas (`XEMU_TSO=1`, 200 s) | — | 0 errors, banner active, **guest at its native 60.0 flips/s, p50 16.7 ms**, 5 stutters total | PASS — second title clean; a 60 fps-class title reaches 60 under TSO |
| 20 | **TSO v2 (`XEMU_TSO=1`): x86-TSO via alignment-checked LDAPR/STLR** | 19.3 fps, p50 49.3, guest 22-23 flips/s | **21.5 fps, p50 44.4 (-10%), p99 -9%, 1% low +11%, stutters -17%, guest 27-28 flips/s**, parity + 0 errors | kept opt-in; recovers ~half to two-thirds of the unsafe fence-elision bound (row 18) with ordering preserved — candidate for default-on after soak |
| 19 | TSO v1 (unconditional LDAPR/STLR) | — | guest made zero progress: LDAPR/STLR fault on unaligned addresses and the A76 lacks FEAT_LSE2 to relax that; x86 code is routinely unaligned | **replaced by v2** — runtime alignment check, aligned -> ordered access, unaligned -> barrier + plain |
| 18 | Fence-elision measurement (`XEMU_WEAK_MO=1`, UNSAFE, same binary) | 18.3 fps, p50 52.0, 1% low 8.2 | **22.8 fps (+25%), p50 42.8 (-18%), p99 -21%, 1% low +53%, stutters -27%**, 0 errors | measurement only — prices the per-access dmb barriers; the shippable form is x86-TSO via LDAR/STLR (M6), now the top work item |
| 17 | TB jump cache 4k -> 32k entries, A/B vs row-15 base | p50 48.8, 1% low 9.1, stutters/min 509 | p50 52.0 (+7%), 1% low 8.2 (-11%), stutters/min 553 (+9%) — all REGRESSED | **reverted** — probes went from L2-hot (64 KB) to L2-thrashing (512 KB); helper_lookup_tb_ptr is frequency-bound, not miss-bound |
| 16 | Descriptor pool 1024 -> 4096 (N5), A/B vs row 14 | nbs_descriptors 0.185/frame, nbs wait 3.65 ms | nbs 0.000, nbs wait 0.03 — but p50 +6%, **p99 +19% REGRESSED**, flip/vertex waits +5 ms (drains moved, grew) | **reverted** — wait is conserved on a TCG-paced frame; fewer-but-larger submissions worsen the tail |
| 14 | Overlap-only settle (bitmap cleared at reclaim), A/B on bc80be5 | 19.7 fps, p50 48.8 | 20.3 fps (noise), **p50 46.1 (-6%, IMPROVED)**, stutters/min -8%, 0 errors | kept — the deferral's honest final form: consistency feature |
| 15 | N2 instrumentation first read (same run) | — | aux_fence 0.59 ms (M3 demoted); renderpasses 10.7/frame (M5 tile-churn hypothesis dead, ~1.3 ms); **nbs_descriptors is the only exhausting pool** (0.19/frame ~= 690 batches / 1024 sets); queries 0 | three roadmap gates resolved in one run |
| 13 | N1 safety triad (race-free deferral), A/B on bdda32f | 19.7 fps, flip wait 9.55 | 20.2 fps (within noise), flip wait 10.44 (!), stutters/min -7%, p99 +7%, 0 errors | the settle-before-every-mirror-write reclaims the deferral immediately: safe but mechanism gone; refine to overlap-only settle |
| 10 | Deferred flip-stall wait, repaired (reclaim at begin_pre_draw) | 19.6 fps, host wait 20.4 ms | 20.4 fps (within noise), host wait 10.2 ms (flip 9.70 -> 0.60), stutters/min -12%, 1% low -5%, 0 errors, guest work fair | kept opt-in — correct, but fps unchanged: **pgraph fence waits are not the frame-rate limiter** |

## Current frame budget (HEAD, Vulkan presentation, measured)

```
frame p50                47.1 ms   (19.9 fps)
GPU busy (timestamps)    17.71 ms  (GPU idle 62% of the frame)
host blocked on fences   20.21 ms
    flip_stall            9.59
    vertex_buffer_dirty   6.81
    need_buffer_space     3.62
    presenting            0.17
host work                ~26.9 ms
ideal CPU/GPU overlap    max(26.9, 17.7) ~= 27 ms  => ~37 fps ceiling
```

Host wait ~= GPU busy: the pipeline is serialised (record -> submit -> wait).

**Update (row 10):** removing half the host wait (20.4 -> 10.2 ms) left fps
within noise. The serialisation was real but not rate-limiting.

**Update (row 11, measured):** per-thread CPU during live gameplay
(vk + async flip, named via `-name xemu,debug-threads=on`):

```
  87.5%  CPU_0/TCG        <- guest CPU emulation: the wall
  35.0%  renderer (pfifo/pgraph, all Vulkan work)
   7.4%  mcpx.apu_thread
   1.4%  vblank-timer
 total  185% of 400% available (two cores idle)
```

The frame is paced by the TCG vCPU thread. GPU-side and renderer-side work
target components at 32-35% duty; the guest-CPU thread at 87.5% is where
frame time now lives.

**Update (row 12, measured):** perf on the TCG thread (30 s @ 997 Hz,
`-perfmap`, symbols >= 0.4%):

```
 10.4%  helper_lookup_tb_ptr        TB chaining / indirect-branch lookup
  2.3%  tlb_reset_dirty_range_all   dirty-page tracking
 ~8.5%  x87 softfloat               floatx80_mul/addsub, fsts/flds/fmul helpers
 ~5.4%  SSE FP helpers              mulss/mulps/addps/comiss + soft_f32_mul
 ~4.5%  exec machinery              cpu_exec_loop, tb_lookup_cmp, tlb_set_page
 ~69%   JIT-generated code + tail   guest code itself, incl. inline TSO fences
```

Reads on the roadmap gates: FP helper share is ~14% visible at the 0.4%
cutoff (x87 alone ~8.5%) — borderline for the L1 hard-FPU GO gate (>=15-20%),
with more FP certainly in the tail. `soft_f32_mul` appearing at all means the
float32 hardfloat fast path is falling through — worth checking whether the
guest sets FTZ/DAZ, which disables QEMU's hardfloat. `helper_lookup_tb_ptr`
at 10.4% is the single largest symbol and was on nobody's roadmap: indirect
branches missing the TB jump cache. The ~69% JIT tail is where M6's per-load
`dmb ishld` fences live, invisible to symbol profiling — only the elision A/B
can size them.
