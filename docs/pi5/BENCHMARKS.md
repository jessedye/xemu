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
| 43 | **Hole found in the default vertex guard** (`vertex.c`, source reasoning - not yet reproduced) | guard tests pages *uploaded* since the last reclaim | a page uploaded in an earlier command buffer, read by a draw recorded in the current one without re-upload, carries no bit; a guest rewrite then lands on data an unexecuted draw still reads | OPEN - `uploaded` is not the same set as `read by a recorded draw`. Needs a reproducer before any fix; the in-place mirror means the only complete fix is versioning the buffer |
| 42 | **Precise vertex-rewrite tracking** (`XEMU_VTX_PRECISE`, off by default) | guard fires on any page *uploaded* | conflicting pages 10.76 -> 0.15/frame; stalls 0.12 -> 0.15/frame (+20%), p99 +43% | RECLASSIFIED as a CORRECTNESS fix, not a speed one - see row 43. It is slower because it tracks a strictly larger set, and that set is the one correctness actually requires |
| 41 | **Attract-mode cross-fade mistaken for corruption** | ghosted frame read as a rendering bug, change reverted | the *default* guard produced a ghosted frame too, on a path that gave crisp frames twice before - 3 captures of identical rendering, 2 crisp 1 ghosted | CALIBRATION - ghosting is time-dependent, not code-dependent. Morrowind's attract demo cross-fades between viewpoints; it is unfit as a visual-regression scene. Needs a gameplay snapshot |
| 40 | **Stutter metric noise floor measured** | assumed meaningful at ~10% | two *functionally identical* binaries (70928c507 vs its revert b55b91b3c) differed by **+27% stutters/min** on the same snapshot | CALIBRATION — treat stutters/min deltas below ~30% as noise; do not call them regressions |
| 39 | **Conditional surface-upload drain** (`surface.c`, guarded by `draw_time`/`sampled_time`) | unconditional `pgraph_vk_finish` per upload | waits down (aux_fence -33%, flip_stall -45%); frame looked **ghosted**, which was read as corruption | REVERTED, but the reason was WRONG - see row 41. Ghosting is the attract demo cross-fading, not the change. Verdict on this change is UNKNOWN, not "corrupts" |
| 38 | **Snapshot-resumed benchmarking** (`make-snapshot.sh` + `bench.sh -x`) | arms started from disc boot and drifted; a scene difference read as a perf difference | both arms resume one snapshot; guest-driven counters now agree within 15% | PASS - immediately exposed row 39's corruption, which the drifting benchmark had scored as MATCH 0.10 |
| 37 | **Vulkan present loop paced to vblank** (`ui/xemu.c`) | mailbox present never blocks, so the UI loop free-ran | p99 -6%, 1% low +14%, stutters -17%, waits down across the board | PASS - the GL path was vsync-paced all along; only the Vulkan arm was uncapped |
| 1 | Index-buffer clamp (bounded allocation) | 855 MiB reserved, 254 tex evictions | 580 MiB, 0 evictions | kept |
| 2 | 1080p output instead of 4K | — | +24% fps | kept (launcher forces 1080p) |
| 3 | V3D 1350 MHz | — | +9% fps, readback 18.14 -> 14.57 ms | kept |
| 4 | -O3 | — | +1.1% | kept |
| 5 | LTO | — | +1.0% | kept |
| 6 | Fence instead of vkQueueWaitIdle (aux submits) | — | no change | kept for correctness |
| 7 | Non-blocking presenter on the readback path | 16.6 fps | 6.3 fps | **reverted** — starved the upload |
| 8 | Vulkan swapchain presentation (`XEMU_DISPLAY_BACKEND=vulkan`) | 18.0-18.4 fps, readback 10.9-11.7 ms, 1% low 7.8 | 19.4-19.9 fps, readback 0, 1% low 9.2 | kept, opt-in (+7% fps, +18% 1% low) |
| 9 | Deferred flip-stall wait, first attempt (`XEMU_ASYNC_FLIP=1`) | 19.8 fps | 5.8 fps, assert, guest starved 1000x | **broken** — reclaim ran after next-draw allocation; fix at HEAD, untested |
| 36 | SSE hardfloat fast path (`XEMU_SSE_HARDFLOAT`), Morrowind (uncapped) | 43.3 fps, p50 23.3, 97 stutters/min | 43.8 fps, p50 23.1 — all within noise; **stutters +6% REGRESSED** | **not shipped.** The gate diagnosis was correct (softfloat was running) but the win does not materialise: SSE FP was ~6% of a thread that is no longer the sole limiter at this frame rate. Kept opt-in, default off |
| 35 | **Benchmark saturation discovered** | Halo 2 assumed CPU-limited | **Halo 2 runs at 30.0 flips/s on both sides of any A/B — it is at its native cap** | the primary benchmark can no longer measure CPU-side work. Guest-rate ceiling reached for this title; further CPU A/Bs must use an uncapped title (Morrowind, 56 of ~60) |
| 33 | Vertex-drain sizing (instrumentation, no fix yet) | 5.18-5.38 ms/frame, occurrence count unknown | **0.88 drains/frame at ~6.1 ms each** (not many small ones); 42 mirror syncs/frame, only ~2% conflict | one expensive mid-frame full drain per frame — real, but see row 34 |
| 34 | **Pacer re-measured with the full shipped stack** | assumed possibly shifted after TSO + hard-FPU | `CPU_0/TCG` **80.6%** (was 87.5%), renderer 31.7% (was 35.0%) | **the vCPU thread is still the pacer.** M1 and per-draw work are renderer-side, so expect fps-neutral like rows 16/27; their value is consistency only. Do not build M1 for throughput |
| 32 | Fallback path verified on hardware (`renderer = OPENGL`, V3D) | claimed working, unverified | probe fails -> GL declines -> **Vulkan renderer initializes and runs the guest**; ran to completion | confirmed. **Also found: two GL ceilings, not one** — the GUI needs GLSL 1.50 (`gl-helpers.cc`) and V3D caps at 1.40, so xemu aborts in HUD init before renderer selection unless the GL HUD is bypassed. Reported to #2979 |
| 31 | L3 soaks + **ship** | — | GTA SA locked 60.0 flips/s, **Morrowind 56 (was 27-55 without it)**, 0 errors both | PASS — `XEMU_SURF_TEX_SAMPLE=1` joins the launcher; full stack is now vulkan + TSO + hard-FPU + direct sampling |
| 30 | **Direct surface sampling (`XEMU_SURF_TEX_SAMPLE`)**, fixed binary, full stack both sides | surf_to_tex 6.18/frame, stutters/min 427, 1% low 11.5 | surf_to_tex **1.15** (residue = designed zeta/feedback/format fallbacks), **stutters/min 362 (-15%), 1% low 12.2 (+7%)**, gpu_busy -9%, parity + 0 errors | works as measured-for; soak on GTA+Morrowind gates the launcher. Debug trail: a null-labeler crash masqueraded as OOM and starvation first (rows in skill) |
| 29 | **System audio fixed** (all RetroPie emulators silent) | display returns 0-byte EDID -> no ELD -> vc4 refuses audio; Pi 5 vc4 PCM additionally accepts only IEC958_SUBFRAME_LE | conformant firmware EDID override (4K60+audio, `drm.edid_firmware=` + removal of the `video=...60D` force that skipped EDID probing) + `iec958` ALSA plugin chain in asound.conf | **audio infoframe live on the wire**; tone + wav play. Both fixes persist across boots |
| 28 | Stutter attribution (132 windows, 4 full-config runs, no new data) | hypothesis: compile stutter (M4) | r(pipeline_gen)=-0.15 — **compilation exonerated**; stuttery third = heavy scenes: begin_ends 85->1268/frame (r+0.92), nbs drains r+0.91, vbd settles r+0.85, surf_to_tex ~10/frame r+0.71, GPU at ~50% duty | M4 killed for this workload; remaining stutter levers are structural: per-draw host cost and direct surface-as-texture sampling (L3). Pool regrowth stays refuted (row 16) |
| 27 | Present-at-flip (`XEMU_PRESENT_ON_FLIP`), full launcher config both sides | 22.7 fps, p50 43.5 | 22.6 fps, p50 43.4 — everything within noise; parity, 0 errors | **not shipped** — correct but flat; in the measured scene the guest's own flip rate matched the presented rate, so no coalescing existed to recover. Stays as an opt-in for scene-specific investigation |
| 26 | **Config-parity audit from real play sessions** (user-reported lag) | launcher shipped `XEMU_HARD_FPU=1` (= bitmask group 1: loads only, arithmetic still softfloat) and never set `XEMU_DISPLAY_BACKEND=vulkan` (GL readback path, 5-14 ms/frame in play) | sessions: THPS2x 17.9 fps/554 stutters, Halo 2 18.2/518, Morrowind 11.1 fps 1%low 4.4 | **fixed** — launcher sets vulkan + mask 31; parser maps 1->31. Lesson: the bench env and the launcher env must be one thing |
| 25 | Hard-FPU soaks + **ship** | — | GTA SA at a locked 60.0 flips/s, Morrowind 27-55 (faster than its TSO-only run), 0 errors both | PASS — `XEMU_HARD_FPU=1` joins `XEMU_TSO=1` in the RetroPie launcher |
| 24 | **Hard-FPU helpers (`XEMU_HARD_FPU`, native-double x87), TSO both sides** | 21.3 fps, p50 45.3, guest ~28 flips/s | **p50 42.9 (-5%), p99 -8%, 1% low 12.0 (+14%), stutters -11%, guest at the native 30.0 flips/s cap**, parity + 0 errors | kept opt-in pending soak — **Halo 2 reaches its native frame rate**; v1 (unconditional LDAPR-era layout change) and v2 (inline-tier crash) both documented on the way |
| 23 | MXCSR trace (Halo 2, one LDMXCSR ever) | hypothesis: non-nearest rounding kills hardfloat | `mxcsr=0x1f80 rc=0 ftz=0 daz=0` — power-on default, nearest rounding | question closed: SSE hardfloat is healthy; remaining soft_f32 share is the near-denormal fallback. The x87 floatx80 path (~8.5%) is the only real FP target left |
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

## Open follow-ups

- **Upstream `vk/present.c`** — the Vulkan swapchain presentation path is the
  one genuinely new file this work adds to the renderer (everything else is
  edits to existing files; upstream already ships a full Vulkan renderer).
  Blocked on completing the ImGui overlay for that path: it currently skips
  the GL HUD entirely, so a desktop user switching to it loses their menus.
  Needs `imgui_impl_vulkan` wired to the presenter's device/queue, then it is
  PR-ready as one new file plus edits to `ui/xemu.c` and `nv2a.h`.
- **GUI GLSL ceiling** — `ui/xui/gl-helpers.cc` uses `#version 150 core`;
  V3D caps at 1.40, so xemu aborts in HUD init on that hardware regardless of
  renderer fallback (ledger row 32). Lowering those shaders is what would make
  a GL-3.1 device genuinely supported upstream.
