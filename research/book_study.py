"""Summarises book_study CSV output into the order book comparison table.

Usage: python research/book_study.py <csv> [<csv> ...] [--cg <variant>=<cachegrind.out> ...]

Per variant: median across repetitions of the per-event p50 / p99 / p99.9 latency and of the
batch cost per event, with 95% bootstrap intervals of the median, and a two-sided
Mann-Whitney test of the batch cost against the `tick` variant. Cachegrind files add
instructions and simulated cache misses per event (the region between the client requests);
the event count is read from the book_study output saved beside each file as .txt.
"""

import csv
import sys

import numpy as np
from scipy.stats import mannwhitneyu

RNG = np.random.default_rng(0)


def boot_median(x, n=2000):
    x = np.asarray(x, dtype=float)
    meds = np.median(RNG.choice(x, size=(n, len(x)), replace=True), axis=1)
    return np.median(x), np.percentile(meds, 2.5), np.percentile(meds, 97.5)


def fmt(m, lo, hi, digits=1):
    return f"{m:.{digits}f} [{lo:.{digits}f}, {hi:.{digits}f}]"


def cachegrind(path):
    events = None
    totals = {}
    with open(path) as f:
        for line in f:
            if line.startswith("events:"):
                events = line.split()[1:]
            elif line.startswith("summary:"):
                totals = dict(zip(events, map(int, line.split()[1:])))
    return totals


def cg_events(path):
    with open(path.rsplit(".", 1)[0] + ".txt") as f:
        for line in f:
            if line.startswith("events "):
                return int(line.split()[1])
    raise ValueError(f"no event count beside {path}")


def main():
    args = sys.argv[1:]
    cg = {}
    if "--cg" in args:
        i = args.index("--cg")
        cg = dict(a.split("=", 1) for a in args[i + 1 :])
        args = args[:i]
    rows = []
    for path in args:
        with open(path) as f:
            rows += list(csv.DictReader(f))

    for mode in ("decode", "replay"):
        r = [x for x in rows if x["mode"] == mode]
        if not r:
            continue
        n = int(r[0]["events"])
        rate = [n / float(x["total_ns"]) * 1e3 for x in r]
        secs = [float(x["total_ns"]) / 1e9 for x in r]
        print(f"{mode}: {n} messages, {len(r)} reps, "
              f"{fmt(*boot_median(rate))} M msg/s, {fmt(*boot_median(secs), 3)} s")

    book = [x for x in rows if x["mode"] == "book"]
    variants = list(dict.fromkeys(x["variant"] for x in book))
    if not variants:
        return
    batch = {v: [float(x["total_ns"]) / int(x["events"]) for x in book if x["variant"] == v]
             for v in variants}
    print()
    print("| Book | Batch ns/event | p50 ns | p99 ns | p99.9 ns | vs tick (Mann-Whitney p) "
          "| Instr/event | D1 miss/event | LL miss/event | Errors |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for v in variants:
        r = [x for x in book if x["variant"] == v]
        n = int(r[0]["events"])
        col = lambda k: fmt(*boot_median([float(x[k]) for x in r]))
        p = "-" if v == "tick" else f"{mannwhitneyu(batch[v], batch['tick']).pvalue:.2g}"
        ir = d1 = ll = "-"
        if v in cg:
            t = cachegrind(cg[v])
            m = cg_events(cg[v])
            ir = f"{t['Ir'] / m:.0f}"
            if "D1mr" in t:
                d1 = f"{(t['D1mr'] + t['D1mw']) / m:.2f}"
                ll = f"{(t['DLmr'] + t['DLmw']) / m:.3f}"
        errors = max(int(x["check"]) for x in r)
        print(f"| {v} | {fmt(*boot_median(batch[v]))} | {col('p50_ns')} | {col('p99_ns')} "
              f"| {col('p999_ns')} | {p} | {ir} | {d1} | {ll} | {errors} |")


if __name__ == "__main__":
    main()
