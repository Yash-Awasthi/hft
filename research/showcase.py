"""Builds a static results page (one HTML file, inline CSS and SVG, no scripts, no fonts).

Usage: python research/showcase.py --out site [--bench bench.json] [--pm-tsv tokens.tsv]
           [--pm-dir pm-data]

Sections come from what is available: the before/after table in README.md, a Google
Benchmark JSON file, and the output of pm_stats for a prediction-market recording.
"""

import argparse
import csv
import datetime
import html
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
UNITS = {"ns": 1e-9, "us": 1e-6, "µs": 1e-6, "ms": 1e-3, "s": 1.0}

CSS = """
:root{--bg:#fbfbfa;--fg:#1c1c1a;--mute:#6b6b66;--line:#e2e2dd;--a:#c2410c;--b:#1d6f8a;--card:#fff}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#141413;--fg:#ececea;--mute:#9a9a94;--line:#2c2c29;--a:#fb923c;--b:#5eb5d1;--card:#1c1c1a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,sans-serif}
main{max-width:920px;margin:0 auto;padding:32px 16px 64px}h1{font-size:1.9rem;margin:0 0 4px}
h2{font-size:1.2rem;margin:40px 0 8px;border-top:1px solid var(--line);padding-top:20px}
p.lead{color:var(--mute);margin:0 0 8px}table{border-collapse:collapse;width:100%;font-size:.92rem}
th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:middle}
th{color:var(--mute);font-weight:500}td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}
.bar{height:10px;border-radius:2px;background:var(--b)}.bar.a{background:var(--a)}
.cell{display:flex;align-items:center;gap:8px}.cell span{min-width:72px;text-align:right;font-variant-numeric:tabular-nums}
.stat{display:inline-block;margin:0 24px 8px 0}.stat b{display:block;font-size:1.5rem}.stat i{color:var(--mute);font-style:normal;font-size:.85rem}
small{color:var(--mute)}.wide{overflow-x:auto}
"""


def seconds(cell):
    m = re.fullmatch(r"\s*([\d,]+(?:\.\d+)?)\s*(ns|µs|us|ms|s)\s*", cell)
    return float(m.group(1).replace(",", "")) * UNITS[m.group(2)] if m else None


def fmt(sec):
    for unit, scale in (("s", 1), ("ms", 1e-3), ("µs", 1e-6), ("ns", 1e-9)):
        if sec >= scale:
            return f"{sec / scale:,.1f} {unit}" if unit in ("s", "ms", "µs") else f"{sec / scale:,.0f} {unit}"
    return f"{sec / 1e-9:,.0f} ns"


def readme_pairs():
    """Rows of the README's before/after table whose two cells are plain durations."""
    rows = []
    for line in (ROOT / "README.md").read_text().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 3 and cells[0] not in ("Path", "---"):
            b, a = seconds(cells[1]), seconds(cells[2])
            if b and a:
                rows.append((cells[0], b, a))
    return rows


def speed_section():
    rows = readme_pairs()
    if not rows:
        return ""
    out = ["<h2>Speed, before and after</h2>",
           "<p class=lead>Each pair was timed back to back on the same machine, so the ratio is comparable "
           "even though absolute times drift between sessions.</p>",
           "<div class=wide><table><tr><th>Path</th><th>Before</th><th>After</th><th class=n>Change</th></tr>"]
    for name, b, a in rows:
        out.append(f"<tr><td>{html.escape(name)}</td>"
                   f"<td><div class=cell><div class='bar a' style='width:{100}px'></div><span>{fmt(b)}</span></div></td>"
                   f"<td><div class=cell><div class=bar style='width:{max(2, 100 * a / b):.0f}px'></div><span>{fmt(a)}</span></div></td>"
                   f"<td class=n>{b / a:.1f}x faster</td></tr>")
    out.append("</table></div>")
    return "".join(out)


def bench_section(path):
    if not path:
        return ""
    data = json.loads(pathlib.Path(path).read_text())
    rows = [(b["name"], b["real_time"] * UNITS.get(b.get("time_unit", "ns"), 1e-9))
            for b in data["benchmarks"] if b.get("run_type", "iteration") == "iteration"]
    if not rows:
        return ""
    top = max(t for _, t in rows)
    ctx = data.get("context", {})
    out = ["<h2>Latency benchmarks</h2>",
           f"<p class=lead>{html.escape(str(ctx.get('host_name', '')))}, {ctx.get('num_cpus', '?')} CPUs, "
           f"{html.escape(str(ctx.get('date', '')))}. Log scale.</p>",
           "<div class=wide><table><tr><th>Benchmark</th><th>Time</th></tr>"]
    import math
    lo = min(t for _, t in rows)
    for name, t in rows:
        w = 6 + 260 * (math.log10(t / lo) / max(math.log10(top / lo), 1e-9)) if top > lo else 100
        out.append(f"<tr><td>{html.escape(name)}</td><td><div class=cell><div class=bar style='width:{w:.0f}px'></div>"
                   f"<span>{fmt(t)}</span></div></td></tr>")
    out.append("</table></div>")
    return "".join(out)


def pm_section(tsv, pm_dir):
    if not tsv:
        return ""
    rows = list(csv.DictReader(open(tsv), delimiter="\t"))
    if not rows:
        return ""
    tot_msg = sum(int(r["snapshots"]) + int(r["deltas"]) + int(r["trades"]) for r in rows)
    hours = max(float(r["observed_h"]) for r in rows)
    notional = sum(float(r["notional"]) for r in rows)
    mism = sum(int(r["bba_mismatch"]) for r in rows)
    deltas = sum(int(r["deltas"]) for r in rows)
    reconnects = 0
    if pm_dir:
        ev = pathlib.Path(pm_dir) / "events.log"
        if ev.exists():
            reconnects = max(0, sum(" connected " in l for l in ev.read_text().splitlines()) - 2)
    stats = [(len(rows), "tokens"), (f"{tot_msg:,}", "messages replayed"), (f"{hours:.1f} h", "longest recording"),
             (f"${notional:,.0f}", "traded in the window"), (f"{100 * mism / max(deltas, 1):.2f}%", "deltas where our book "
              "differs from the exchange's best prices"), (reconnects, "reconnects")]
    out = ["<h2>Prediction-market order books</h2>",
           "<p class=lead>A hand-written WebSocket recorder stores the public order-book stream; the book of every "
           "outcome token is rebuilt from snapshots and deltas and checked against the exchange's own best prices.</p>",
           "".join(f"<div class=stat><b>{v}</b><i>{html.escape(str(k))}</i></div>" for v, k in stats),
           "<div class=wide><table><tr><th>Market</th><th>Outcome</th><th class=n>Trades</th><th class=n>Traded $</th>"
           "<th>Spread (cents)</th><th class=n>Depth within 5c</th></tr>"]
    top = sorted(rows, key=lambda r: -float(r["notional"]))[:12]
    smax = max(float(r["spread_cents"]) for r in top) or 1
    for r in top:
        s = float(r["spread_cents"])
        out.append(f"<tr><td>{html.escape(r['slug'][:48])}</td><td>{html.escape(r['outcome'][:24])}</td>"
                   f"<td class=n>{int(r['trades']):,}</td><td class=n>{float(r['notional']):,.0f}</td>"
                   f"<td><div class=cell><div class=bar style='width:{max(2, 80 * s / smax):.0f}px'></div><span>{s:.2f}</span></div></td>"
                   f"<td class=n>{float(r['depth5c']):,.0f}</td></tr>")
    out.append("</table></div>")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--bench")
    ap.add_argument("--pm-tsv")
    ap.add_argument("--pm-dir")
    a = ap.parse_args()
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    body = (f"<h1>hft</h1><p class=lead>An order-by-order Nasdaq ITCH replay with a deterministic matching engine, "
            f"a market-making backtester and an in-loop AVX2 transformer, in C++23.</p>"
            f"<small>Generated {datetime.datetime.now(datetime.UTC):%Y-%m-%d %H:%M} UTC from commit {commit}.</small>"
            + speed_section() + bench_section(a.bench) + pm_section(a.pm_tsv, a.pm_dir))
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(
        f"<!doctype html><html lang=en><head><meta charset=utf-8>"
        f"<meta name=viewport content='width=device-width,initial-scale=1'><title>hft results</title>"
        f"<style>{CSS}</style></head><body><main>{body}</main></body></html>")
    print(out / "index.html")


if __name__ == "__main__":
    main()
