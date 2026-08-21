#!/usr/bin/env python3
"""Summarise perflog runs, rejecting any that did not run the same workload.

A snapshot restores into a scene that keeps running, and a restore can land
somewhere else entirely: one Morrowind run came back with 736 draw batches a
frame against the usual 1172, and averaging it in reported a 50% gain that did
not exist. Guest work per frame is deterministic enough to detect that, so
compare it across runs and drop the outliers rather than averaging them.
"""
import glob, os, sys, statistics as st

def stats(p):
    rows = [l.rstrip("\n") for l in open(p)]
    names = ("elapsed_s frames fps frame_ms_avg frame_ms_p50 frame_ms_p99 frame_ms_max "
             "fps_1pct_low stutters download_ms upload_ms tex_cache_mb resolution "
             "cpu_temp_c cpu_mhz").split()
    for l in rows:
        for k in ("# counters", "# gpu_busy_ms", "# gpu_wait_ms", "# vertex ("):
            if l.startswith(k):
                ns = l.split(":", 1)[1].split()
                names += ["wait:" + x for x in ns] if k == "# gpu_wait_ms" else ns
    w = [l.split(",")[1:] for l in rows if l.startswith("window,")]
    if not w:
        return None
    i = {n: k for k, n in enumerate(names)}
    def num(r, n):
        try:
            return float(r[i[n]])
        except Exception:
            return None
    t = w[len(w) // 2:]
    out = {n: sum(v) / len(v) for n in names
           for v in [[x for x in (num(r, n) for r in t) if x is not None]] if v}
    out["_windows"] = len(w)
    out["_file"] = os.path.basename(p)
    return out

def accept(runs, tol=0.20):
    """Keep runs whose guest work per frame is within tol of the median."""
    ok = [r for r in runs if r and "begin_ends" in r]
    if len(ok) < 2:
        return ok, []
    med = st.median([r["begin_ends"] for r in ok])
    if med == 0:
        return ok, []
    good = [r for r in ok if abs(r["begin_ends"] - med) / med <= tol]
    bad = [r for r in ok if r not in good]
    return good, bad

def summarise(pattern, label):
    runs = [stats(f) for f in sorted(glob.glob(pattern))]
    good, bad = accept([r for r in runs if r])
    for b in bad:
        print(f"    REJECTED {b['_file']}: begin_ends={b['begin_ends']:.0f} "
              f"windows={b['_windows']} - different workload")
    if not good:
        print(f"    {label}: no usable runs")
        return None
    m = lambda n: st.mean([x[n] for x in good if n in x]) if any(n in x for x in good) else 0.0
    print(f"    {label:22s} n={len(good)} fps={m('fps'):6.2f} p50={m('frame_ms_p50'):7.2f} "
          f"p99={m('frame_ms_p99'):7.2f} low1={m('fps_1pct_low'):5.2f} gpu={m('gpu_busy'):6.2f} "
          f"rp={m('renderpasses'):6.2f} b_ends={m('begin_ends'):6.0f}")
    return m

if __name__ == "__main__":
    summarise(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "runs")
