#!/usr/bin/env python3
"""Plot the GPU-time rows written by `ns_demo --perf-csv` / `--perf-raw`.

Two capture flavours, same 5-column tab-separated format (t kind name
context ms):

  --perf-csv   one row per second per *active* effect, EMA ms - the smooth
               cost of the show over time:

                   t       kind      name              context           ms
                   0.00    post      post              bloom 6           0.47
                   13.00   scene     cathedral         renderScale 1.00  2.45
                   13.00   shadertoy tunnel_warp.glsl  renderScale 0.50  1.10

  --perf-raw   one row per COLLECTED sample, UNSMOOTHED ms - the spikes the
               EMA hides (stalls, allocator hiccups, a renderScale step):

                   t       kind      name              context           ms
                   13.01   scene     cathedral         renderScale 1.00  2.98
                   13.02   scene     cathedral         renderScale 1.00  2.41

Each row's `ms` is the GPU time during [t, t+1) (per-second) or of that one
frame (raw). This script overlays one line per effect so a renderScale
change (or a scene change mid-show) is immediately visible; the legend
annotates each line with the last context seen for it.

Usage:
    python docs/plot_perf.py                          # EMA, reads perf.csv
    python docs/plot_perf.py --mode=raw -o raw.png    # raw, reads perf.raw.csv
    python docs/plot_perf.py perf.raw.csv --mode=raw  # explicit path
    python docs/plot_perf.py --min-ms=1               # drop effects < 1 ms
"""

import argparse
import csv
import sys

try:
    import matplotlib
except ImportError:
    matplotlib = None


def load(path):
    """Return {name: [(t, ms, context), ...]} in file order, skipping junk."""
    try:
        f = open(path, newline="", encoding="utf-8", errors="replace")
    except OSError as e:
        sys.exit(f"error: cannot read {path}: {e.strerror or e}")
    series = {}
    with f:
        reader = csv.reader(f, delimiter="\t")
        header = next(reader, None)
        if not header or header[0].strip() != "t":
            sys.exit(f"error: {path} is not a perf capture (missing t/kind/name/context/ms header)")
        for row in reader:
            if len(row) < 5:
                continue  # blank or partial line
            try:
                t, ms = float(row[0]), float(row[4])
            except ValueError:
                continue  # not a data row
            if ms <= 0.0:
                continue  # no sample yet for that second
            name = row[2].strip()
            series.setdefault(name, []).append((t, ms, row[3].strip()))
    return series


def plot(series, output, mode, min_ms):
    if matplotlib is None:
        sys.exit("error: matplotlib is required - pip install matplotlib")
    if output:
        matplotlib.use("Agg", force=True)  # headless-safe when writing a file
    import matplotlib.pyplot as plt

    if not series:
        msg = "error: no timed-effect rows found - run with --perf-csv/--perf-raw for a few seconds first"
        if min_ms > 0:
            msg += f" (or nothing stayed above --min-ms={min_ms})"
        sys.exit(msg)
    raw = mode == "raw"
    fig, ax = plt.subplots(figsize=(10, 5.5))
    cmap = plt.get_cmap("tab10")
    for i, (name, pts) in enumerate(sorted(series.items())):
        ts = [p[0] for p in pts]
        ms = [p[1] for p in pts]
        ctx = pts[-1][2]  # last context seen for this effect (e.g. its renderScale)
        label = f"{name}  ({ctx})" if ctx else name
        if raw:
            # dense per-frame points: thin trace, no markers - spikes read
            # as sharp peaks, a scale step as an instant jump
            ax.plot(ts, ms, linewidth=0.8, alpha=0.9, color=cmap(i % 10), label=label)
        else:
            ax.plot(ts, ms, marker="o", markersize=4, linewidth=1.8,
                    color=cmap(i % 10), label=label)
    ax.set_xlabel("show time (s)")
    ax.set_ylabel("GPU time per frame, raw (ms)" if raw else "GPU time per frame, EMA (ms)")
    ax.set_title(("Per-effect GPU cost, every collected sample" if raw
                  else "Per-effect GPU cost over the show (per-second samples)"))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=8, framealpha=0.9)
    fig.tight_layout()
    if output:
        fig.savefig(output, dpi=150)
        print(f"wrote {output}")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description="Overlay each effect's ms column from a perf capture.")
    ap.add_argument("csv", nargs="?", help="path to the capture (default: perf.csv in ema mode, perf.raw.csv in raw mode)")
    ap.add_argument("--mode", choices=("ema", "raw"), default="ema",
                    help="ema = per-second EMA rows (--perf-csv); raw = every collected sample (--perf-raw, shows spikes)")
    ap.add_argument("--min-ms", type=float, default=0.0, metavar="N",
                    help="drop effects whose maximum ms stays below N - de-clutters the overlay")
    ap.add_argument("-o", "--output", help="write PNG to this path instead of opening a window")
    args = ap.parse_args()

    path = args.csv or ("perf.raw.csv" if args.mode == "raw" else "perf.csv")
    series = load(path)
    if args.min_ms > 0:
        series = {n: pts for n, pts in series.items()
                  if max(p[1] for p in pts) >= args.min_ms}
    plot(series, args.output, args.mode, args.min_ms)


if __name__ == "__main__":
    main()
