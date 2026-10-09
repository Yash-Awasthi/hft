"""Builds the feature and target dataset of one day for the study universe, as Parquet.

Features come from hftpy.features (the C++ streaming engine), targets from hftpy.labels (the
separate labeler); rows join on the feed sequence number. Each symbol is sampled every k-th
event so that it contributes about ROWS rows a day. Test days are refused (splits.toml).

Usage: python research/build_dataset.py <py-build-dir> <store-dir> <day YYYY-MM-DD> <out.parquet>
"""

import pathlib
import struct
import sys
import tomllib

import numpy as np
import polars as pl

ROOT = pathlib.Path(__file__).resolve().parents[1]
ROWS = 100_000
EVENT_H = [10, 100, 1000]
CLOCK_H_MS = [10, 100, 1000, 10000]


def messages(store, locate):
    data = (pathlib.Path(store) / f"{locate:05d}.idx").read_bytes()
    return sum(struct.unpack_from("<QIIIIQQ", data, i)[3] for i in range(0, len(data), 40))


def main():
    py_build, store, day, out = sys.argv[1:5]
    splits = tomllib.loads((ROOT / "configs/splits.toml").read_text())
    if day in splits["test"]["days"]:
        sys.exit(f"{day} is a locked test day")
    sys.path.insert(0, py_build)
    import hftpy

    universe = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    groups = {s: g for g in ("large_tick", "small_tick") for s in universe[g]["symbols"]}
    sym = hftpy.symbols(store)
    targets = [s for s in groups if s in sym]
    missing = sorted(set(groups) - set(targets))
    if missing:
        print(f"{day}: not listed that day: {missing}", file=sys.stderr)
    locs = [sym[s] for s in targets]
    every = [max(1, messages(store, l) // ROWS) for l in locs]
    feats = hftpy.features(store, locs, [sym["SPY"], sym["QQQ"]], every)
    names = list(feats["names"])
    names = [n.replace("index0", "spy").replace("index1", "qqq") for n in names]

    frames = []
    for s, loc in zip(targets, locs):
        f = feats[loc]
        lab = hftpy.labels(store, loc, EVENT_H, [h * 1_000_000 for h in CLOCK_H_MS])
        at = np.searchsorted(lab["seq"], f["seq"])
        assert np.array_equal(lab["seq"][at], f["seq"])
        cols = {"symbol": [s] * len(at), "group": [groups[s]] * len(at), "day": [day] * len(at),
                "seq": f["seq"], "ts": f["ts"], "mid": lab["mid"][at]}
        cols.update({n: f["X"][:, i] for i, n in enumerate(names)})
        for i, h in enumerate(EVENT_H):
            cols[f"y_e{h}"] = lab["Y"][at, i]
        for i, h in enumerate(CLOCK_H_MS):
            cols[f"y_{h}ms"] = lab["Y"][at, len(EVENT_H) + i]
        frames.append(pl.DataFrame(cols))
    df = pl.concat(frames)
    df.write_parquet(out)
    print(f"{day}: {df.height} rows, {len(targets)} symbols -> {out}")


if __name__ == "__main__":
    main()
