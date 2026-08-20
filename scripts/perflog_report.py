#!/usr/bin/env python3
"""Summarise and compare xemu perflog CSVs (XEMU_PERFLOG output).

Usage:
  perflog_report.py summary RUN.csv
  perflog_report.py compare BEFORE.csv AFTER.csv

Every performance claim should come from `compare`: it reports per-window
means with a noise bound, so a delta inside measurement noise is called out
as such instead of being read as a win. If a sibling RUN.log exists it is
used to check the two runs are fairly comparable (same guest progress) and
to report which binary produced each run.
"""
import math
import os
import re
import sys


def load(path):
    counters, waits, vertex, has_busy = [], [], [], False
    windows, stutters, events = [], [], []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                if "counters (per frame):" in line:
                    counters = line.split(":", 1)[1].split()
                elif "gpu_wait_ms (per frame):" in line:
                    waits = line.split(":", 1)[1].split()
                elif "vertex (per frame):" in line:
                    vertex = line.split(":", 1)[1].split()
                elif "gpu_busy_ms" in line:
                    has_busy = True
                continue
            parts = line.split(",")
            if parts[0] == "window":
                windows.append(parts)
            elif parts[0] == "stutter":
                stutters.append(parts)
            elif parts[0] == "event":
                events.append(parts)
            # anything else (wrapped header fragments) is tolerated
    return {
        "path": path,
        "counters": counters,
        "waits": waits,
        "vertex": vertex,
        "has_busy": has_busy,
        "windows": windows,
        "stutters": stutters,
        "events": events,
    }


def _fl(v):
    try:
        return float(v)
    except ValueError:
        return 0.0


# No real frame takes a second. A window containing one means the guest was
# stopped - restoring a snapshot, most often - and the gap was counted as a
# single enormous frame. Such a window poisons avg and worst while leaving p50
# alone, which is exactly the kind of silent contamination that makes an
# average look like a measurement.
PAUSE_MS = 1000.0


def aggregate(run):
    """Reduce window rows to one stats dict."""
    W = run["windows"]
    if not W:
        return None
    kept = [w for w in W if len(w) <= 7 or _fl(w[7]) < PAUSE_MS]
    dropped = len(W) - len(kept)
    if kept:
        W = kept
    base = {
        "fps": 3, "avg_ms": 4, "p50_ms": 5, "p99_ms": 6, "max_ms": 7,
        "low1pct": 8, "down_ms": 10, "up_ms": 11, "tex_mb": 12,
    }
    out = {"n_windows": len(W), "paused_windows": dropped}
    for name, idx in base.items():
        vals = [_fl(w[idx]) for w in W if len(w) > idx]
        out[name] = sum(vals) / len(vals)
        if name == "fps":
            m = out[name]
            var = sum((v - m) ** 2 for v in vals) / max(1, len(vals) - 1)
            out["fps_sd"] = math.sqrt(var)
    out["stutters"] = sum(int(_fl(w[9])) for w in W if len(w) > 9)
    out["minutes"] = sum(_fl(w[1]) for w in W if len(w) > 1) / 60.0
    temps = [_fl(w[14]) for w in W if len(w) > 14]
    mhz = [_fl(w[15]) for w in W if len(w) > 15 and _fl(w[15]) > 0]
    out["temp_max"] = max(temps) if temps else 0
    out["mhz_min"] = min(mhz) if mhz else 0
    out["res"] = sorted({w[13] for w in W if len(w) > 13})

    nc, nw = len(run["counters"]), len(run["waits"])
    out["counter"], out["wait"], out["vertex"] = {}, {}, {}
    out["gpu_busy"] = None
    for w in W:
        extra = w[16:]
        for i, name in enumerate(run["counters"]):
            if i < len(extra):
                out["counter"][name] = out["counter"].get(name, 0.0) + _fl(extra[i])
        pos = nc
        if run["has_busy"] and len(extra) > pos:
            out["gpu_busy"] = (out["gpu_busy"] or 0.0) + _fl(extra[pos])
            pos += 1
        for i, name in enumerate(run["waits"]):
            if pos + i < len(extra):
                out["wait"][name] = out["wait"].get(name, 0.0) + _fl(extra[pos + i])
        pos += nw
        for j, name in enumerate(run["vertex"]):
            if pos + j < len(extra):
                out["vertex"][name] = out["vertex"].get(name, 0.0) + _fl(extra[pos + j])
    n = len(W)
    out["counter"] = {k: v / n for k, v in out["counter"].items()}
    out["wait"] = {k: v / n for k, v in out["wait"].items()}
    out["vertex"] = {k: v / n for k, v in out["vertex"].items()}
    if out["gpu_busy"] is not None:
        out["gpu_busy"] /= n
    return out


def sibling_log(path):
    log = re.sub(r"\.csv$", ".log", path)
    if log == path or not os.path.exists(log):
        return {}
    text = open(log, errors="replace").read()
    prog = [int(m) for m in re.findall(r"surface_update count=(\d+)", text)]
    commit = re.search(r"xemu_commit: ([0-9a-f]+)", text)
    return {
        "progress": max(prog) if prog else None,
        "commit": commit.group(1)[:9] if commit else None,
    }


def print_summary(path):
    run = load(path)
    agg = aggregate(run)
    if not agg:
        print(f"{path}: no window rows"); return
    meta = sibling_log(path)
    print(f"== {os.path.basename(path)} ==")
    if meta.get("commit"):
        print(f"  binary        {meta['commit']}")
    if meta.get("progress") is not None:
        print(f"  guest work    surface_update peak {meta['progress']}")
    print(f"  windows       {agg['n_windows']}  ({agg['minutes']:.1f} min)  res {'/'.join(agg['res'])}")
    if agg.get("paused_windows"):
        print(f"  excluded      {agg['paused_windows']} window(s) containing a "
              f"frame over {PAUSE_MS:.0f} ms - the guest was stopped, not slow")
    print(f"  fps           {agg['fps']:.1f} +/- {agg['fps_sd']:.1f} (per-window sd)")
    print(f"  frame ms      avg {agg['avg_ms']:.1f}  p50 {agg['p50_ms']:.1f}  p99 {agg['p99_ms']:.1f}  worst {agg['max_ms']:.1f}")
    print(f"  1% low        {agg['low1pct']:.1f} fps    stutters {agg['stutters']} ({agg['stutters']/max(agg['minutes'],0.01):.0f}/min)")
    print(f"  readback      {agg['down_ms']:.2f} ms/frame   upload {agg['up_ms']:.2f} ms/frame")
    if agg["gpu_busy"] is not None:
        print(f"  GPU busy      {agg['gpu_busy']:.2f} ms/frame")
    tot = sum(agg["wait"].values())
    if agg["wait"]:
        print(f"  host waits    {tot:.2f} ms/frame:")
        for k, v in sorted(agg["wait"].items(), key=lambda kv: -kv[1]):
            if v >= 0.05:
                print(f"      {k:<22}{v:6.2f}")
    draws = agg["counter"].get("begin_ends", 0.0)
    if agg["counter"]:
        # Calibrated against four real gameplay sessions: THPS3 283, Halo 308,
        # Morrowind 478, GTA SA 585 batches per presented frame. Morrowind's
        # attract demo, which looks like gameplay in a screenshot, runs 4.7.
        scene = ("" if draws >= 50.0 else
                 " (NOT GAMEPLAY - real play measures 283-585)")
        print(f"  scene         {draws:.1f} draw batches per presented frame{scene}")

    vtx = agg.get("vertex") or {}
    if vtx.get("vtx_stalls"):
        stalls = vtx["vtx_stalls"]
        conflict = vtx.get("vtx_conflict_pages", 0.0)
        written = vtx.get("vtx_written_pages", 0.0)
        missed = vtx.get("vtx_missed_hazards", 0.0)
        if missed:
            print(f"  !! vertex hazards missed by the default guard: "
                  f"{missed:.2f}/frame - rewrites onto pages a recorded draw "
                  f"still reads")
        print(f"  vertex rewrite {stalls:.2f} stalls/frame, "
              f"{conflict:.1f} conflicting of {written:.1f} pages written"
              + (f" ({conflict/stalls:.1f} pages per stall)" if stalls else ""))

    if agg["gpu_busy"] is not None and agg["wait"]:
        host_work = agg["p50_ms"] - tot
        print(f"  overlap model p50 {agg['p50_ms']:.1f} = host-work ~{host_work:.1f} + wait {tot:.2f}; "
              f"ideal overlap ~= max(host-work, gpu {agg['gpu_busy']:.1f}) "
              f"=> ~{1000.0/max(host_work, agg['gpu_busy']):.0f} fps ceiling")
    if agg["temp_max"]:
        note = "  (THROTTLE?)" if agg["mhz_min"] and agg["mhz_min"] < 2700 else ""
        print(f"  cpu           max {agg['temp_max']:.0f}C  min {agg['mhz_min']:.0f} MHz{note}")


def verdict(a, b, sa, sb, na, nb, higher_is_better=True):
    d = b - a
    se = math.sqrt((sa * sa) / max(na, 1) + (sb * sb) / max(nb, 1))
    if se > 0 and abs(d) < 2 * se:
        return "within noise"
    good = (d > 0) == higher_is_better
    return "IMPROVED" if good else "REGRESSED"


def print_compare(pa, pb):
    ra, rb = load(pa), load(pb)
    a, b = aggregate(ra), aggregate(rb)
    if not a or not b:
        print("one of the runs has no window rows"); sys.exit(1)
    ma, mb = sibling_log(pa), sibling_log(pb)
    print(f"BEFORE {os.path.basename(pa)}  (binary {ma.get('commit','?')})")
    print(f"AFTER  {os.path.basename(pb)}  (binary {mb.get('commit','?')})")
    if ma.get("progress") and mb.get("progress"):
        lo, hi = sorted([ma["progress"], mb["progress"]])
        if hi > lo * 10:
            print(f"  !! UNFAIR COMPARISON: guest progress differs 10x "
                  f"({ma['progress']} vs {mb['progress']}) — one run was starved; "
                  f"fps numbers below are not comparable")

    # The progress counter above saturates, so on its own it will happily call
    # two different scenes comparable. These counters are driven by what the
    # guest asked for, not by how the renderer serves it, so a renderer change
    # must leave them alone; if it did not, the two runs saw different work and
    # nothing below can be attributed to the change.
    drift = []
    for k in ("begin_ends", "draw_arrays", "clears"):
        av, bv = a["counter"].get(k, 0.0), b["counter"].get(k, 0.0)
        if max(av, bv) < 0.01:
            continue
        rel = abs(bv - av) / max(av, bv)
        if rel > 0.15:
            drift.append(f"{k} {av:.2f}->{bv:.2f} ({rel*100:.0f}%)")
    if drift:
        print("  !! SCENE MISMATCH: guest-driven counters differ - "
              + ", ".join(drift))
        print("     The runs did different work. Treat every number below as "
              "unattributable until the arms match.")
    elif a["counter"] and b["counter"]:
        print("  guest work comparable (guest-driven counters within 15%)")
    print()
    da = a["counter"].get("begin_ends", 0.0)
    db = b["counter"].get("begin_ends", 0.0)
    if a["counter"] and b["counter"] and max(da, db) < 50.0:
        print(f"  !! NOT GAMEPLAY: {da:.1f}/{db:.1f} draw batches per presented frame,")
        print("     against 283-585 measured in real play. A menu or attract demo")
        print("     barely exercises the renderer - notably it shows no vertex stall")
        print("     at all, which is the cost that separates the slow games from the")
        print("     fast one. Re-run from a gameplay snapshot.")

    v = verdict(a["fps"], b["fps"], a["fps_sd"], b["fps_sd"], a["n_windows"], b["n_windows"])
    rows = [("fps", a["fps"], b["fps"], True, v)]
    for key, hib in [("p50_ms", False), ("p99_ms", False), ("low1pct", True),
                     ("down_ms", False), ("up_ms", False)]:
        av, bv = a[key], b[key]
        vv = "~" if av == 0 and bv == 0 else (
            "within noise" if abs(bv - av) < 0.05 * max(abs(av), 1e-9) else
            ("IMPROVED" if (bv > av) == hib else "REGRESSED"))
        rows.append((key, av, bv, hib, vv))
    spm_a = a["stutters"] / max(a["minutes"], 0.01)
    spm_b = b["stutters"] / max(b["minutes"], 0.01)
    rows.append(("stutters/min", spm_a, spm_b, False,
                 "within noise" if abs(spm_b - spm_a) < 0.05 * max(spm_a, 1e-9)
                 else ("IMPROVED" if spm_b < spm_a else "REGRESSED")))
    if a["gpu_busy"] is not None and b["gpu_busy"] is not None:
        rows.append(("gpu_busy_ms", a["gpu_busy"], b["gpu_busy"], False, "~"))
    for k in sorted(set(a["wait"]) | set(b["wait"])):
        av, bv = a["wait"].get(k, 0.0), b["wait"].get(k, 0.0)
        if max(av, bv) >= 0.05:
            rows.append(("wait:" + k, av, bv, False, "~"))
    for k in sorted(set(a.get("vertex", {})) | set(b.get("vertex", {}))):
        av, bv = a.get("vertex", {}).get(k, 0.0), b.get("vertex", {}).get(k, 0.0)
        if av or bv:
            rows.append((k, av, bv, False, "~"))
    print(f"{'metric':<26}{'before':>10}{'after':>10}{'delta':>10}  verdict")
    for name, av, bv, hib, vv in rows:
        d = bv - av
        pct = f" ({100.0*d/av:+.0f}%)" if av else ""
        print(f"{name:<26}{av:>10.2f}{bv:>10.2f}{d:>+10.2f}  {vv}{pct}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "summary":
        print_summary(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "compare":
        print_compare(sys.argv[2], sys.argv[3])
    else:
        print(__doc__.strip()); sys.exit(2)


if __name__ == "__main__":
    main()
