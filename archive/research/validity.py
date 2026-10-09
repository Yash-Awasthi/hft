"""Counterfactual validity harness (DESIGN.md section 7).

Every subject moves one shared L2 book (`L2`, tick-grid prices) with the same mechanics:
generated or replayed events are applied as tokens (type, side, distance from the mid, shares),
interventions are market orders that walk the book. States are sampled from the validation day
(L2 snapshot, the event history and the real events that followed). For an intervention a the
effect is Delta(X, a) = E[Z | X + a] - E[Z | X], Z the mid change over h events, from paired
rollouts sharing their random numbers.

Subjects: replay (the real events that followed, applied to the perturbed book: no reaction),
the queue-reactive model I (research/qr.py calibration), and the event transformer used as a
generator (research/transformer.py). Relations: stability, side symmetry, size monotonicity,
concavity and level against the empirical response of real market orders of similar size.

Usage: python research/validity.py <py-build> --store <val-store> --train <store>... --symbol INTC
           --transformer <dir> --out <dir>
"""

import argparse
import json
import math
import pathlib
import sys

import numpy as np

ADD, EXECUTE, CANCEL, REPLACE, HIDDEN = 0, 1, 2, 3, 4
SIZES = np.array([50, 100, 200, 400, 800, 1600, 3200, 6400])  # shares per size bucket
QS = (100, 300, 1000, 3000, 10000)


def size_bucket(shares):
    return 0 if shares < 100 else min(7, 1 + int(math.log2(shares / 100)))


class L2:
    def __init__(self, bids, asks):
        self.bids, self.asks, self.invalid = dict(bids), dict(asks), 0

    def copy(self):
        b = L2(self.bids, self.asks)
        b.invalid = self.invalid
        return b

    def best(self, side):
        lv = self.bids if side == 0 else self.asks
        return (max(lv) if side == 0 else min(lv)) if lv else None

    def mid(self):
        b, a = self.best(0), self.best(1)
        return (b + a) / 2 if b is not None and a is not None else float("nan")

    def mirror(self):
        """Prices negated and sides swapped: a buy in the mirror is a sell in the original."""
        return L2({-p: q for p, q in self.asks.items()}, {-p: q for p, q in self.bids.items()})

    def market(self, sign, shares):
        """sign +1 buys (takes asks), -1 sells; walks the book."""
        lv = self.asks if sign > 0 else self.bids
        while shares > 0 and lv:
            p = min(lv) if sign > 0 else max(lv)
            take = min(shares, lv[p])
            lv[p] -= take
            shares -= take
            if not lv[p]:
                del lv[p]

    def price_at(self, side, dist):
        m = self.mid()
        if not np.isfinite(m):
            p = self.best(side)
            return None if p is None else (p - dist if side == 0 else p + dist)
        return int(math.floor(m - dist)) if side == 0 else int(math.ceil(m + dist))

    def apply(self, typ, side, dist, shares):
        if typ == HIDDEN:
            return
        if typ == EXECUTE:
            self.market(+1 if side == 1 else -1, shares)
            return
        p = self.price_at(side, dist)
        if p is None:
            self.invalid += 1
            return
        lv = self.bids if side == 0 else self.asks
        if typ == ADD:
            opp = self.best(1 - side)
            if opp is not None and (p >= opp if side == 0 else p <= opp):
                p = opp - 1 if side == 0 else opp + 1  # post-only
            lv[p] = lv.get(p, 0) + shares
        else:  # cancel; a replace is its cancel half
            if p not in lv:
                self.invalid += 1
                return
            lv[p] -= min(lv[p], shares)
            if not lv[p]:
                del lv[p]


class State:
    def __init__(self, book, history, future):
        self.book, self.history, self.future = book, history, future

    def mirror(self):
        flip = lambda t: None if t is None else {**t, "side": 1 - t["side"]}
        return State(self.book.mirror(), flip(self.history), flip(self.future))


def outcome(book, start_mid, mirror):
    m = book.mid()
    return (m - start_mid) if np.isfinite(m) else float("nan")


class ReplaySubject:
    name = "replay"

    def rollout(self, st, q, h, seed):
        b = st.book.copy()
        m0 = b.mid()
        if q:
            b.market(np.sign(q), abs(q))
        f = st.future
        for i in range(min(h, len(f["type"]))):
            b.apply(int(f["type"][i]), int(f["side"][i]), int(f["dist"][i]), int(SIZES[f["size"][i]]))
        return outcome(b, m0, False)


def toy_qr():
    K, N = 3, 30
    n = np.arange(N + 1)
    return {"K": K, "N": N, "aes": 100, "theta": 0.5, "L": np.full((K, N + 1), 1.0), "C": np.vstack([0.3 * n] * K),
            "M": np.full((K, N + 1), 0.3), "init": np.vstack([np.where((n >= 2) & (n <= 5), 0.25, 0)] * K),
            "size_L": np.array([1.0]), "size_M": np.array([1.0])}


class QRSubject:
    """Model I on K levels per side around p_ref, in AES units: p_ref is the mid for odd spreads
    (side-symmetric), else best bid + 1/2."""
    name = "queue_reactive"

    def __init__(self, fit):
        self.f = fit

    def rollout(self, st, q, h, seed):
        f, K = self.f, self.f["K"]
        rng = np.random.default_rng(seed)
        b = st.book
        m0 = b.mid()
        bb = b.best(0)
        pref = m0 if abs(m0 % 1 - 0.5) < 1e-9 else bb + 0.5
        lv = np.zeros((2, K), int)
        for i in range(K):
            lv[0, i] = round(b.bids.get(int(pref - 0.5 - i), 0) / f["aes"])
            lv[1, i] = round(b.asks.get(int(pref + 0.5 + i), 0) / f["aes"])
        lv = np.minimum(lv, f["N"])

        def deplete(s):
            nonlocal pref, lv
            if rng.random() >= f["theta"]:
                return
            o = 1 - s
            lv[o, 1:] = lv[o, :-1].copy()
            lv[s, :-1] = lv[s, 1:].copy()
            lv[o, 0] = max(1, rng.choice(f["N"] + 1, p=f["init"][0]))
            lv[s, -1] = rng.choice(f["N"] + 1, p=f["init"][K - 1])
            pref += -1 if s == 0 else 1

        if q:
            s = 1 if q > 0 else 0
            units = int(math.ceil(abs(q) / f["aes"]))
            for i in range(K):
                take = min(units, lv[s, i])
                lv[s, i] -= take
                units -= take
                if i == 0 and lv[s, 0] == 0 and take:
                    deplete(s)
                    break
        sl, sm = f.get("size_L", np.array([1.0])), f.get("size_M", np.array([1.0]))
        for _ in range(h):
            rates, keys = [], []
            for s in range(2):
                for i in range(K):
                    n = min(lv[s, i], f["N"])
                    best = all(lv[s, j] == 0 for j in range(i))
                    for t, r in ((0, f["L"][i, n] if n < f["N"] else 0), (1, f["C"][i, n] if n else 0),
                                 (2, f["M"][i, n] if n and best else 0)):
                        rates.append(r)
                        keys.append((s, i, t))
            rates = np.array(rates)
            if rates.sum() <= 0:
                break
            s, i, t = keys[rng.choice(len(keys), p=rates / rates.sum())]
            if t == 0:
                lv[s, i] = min(f["N"], lv[s, i] + 1 + rng.choice(len(sl), p=sl))
            else:
                k = 1 if t == 1 else 1 + rng.choice(len(sm), p=sm)
                lv[s, i] = max(0, lv[s, i] - k)
                if i == 0 and lv[s, 0] == 0:
                    deplete(s)
        bid = next((pref - 0.5 - i for i in range(K) if lv[0, i]), pref - 0.5 - K)
        ask = next((pref + 0.5 + i for i in range(K) if lv[1, i]), pref + 0.5 + K)
        return (bid + ask) / 2 - m0


class TransformerSubject:
    """Samples (type, side, distance) from the generator head; size from the empirical size
    distribution of the event type; the time gap token is the history's median."""
    name = "event_transformer"

    def __init__(self, model, size_dist, device="cpu"):
        import torch

        self.m, self.size_dist, self.dev, self.torch = model.to(device).eval(), size_dist, device, torch

    def rollout_batch(self, states, qs, h, seeds):
        torch = self.torch
        W = self.m.layers * (self.m.window - 1) + 1
        B = len(states)
        hist = {k: np.stack([s.history[k][-W:] for s in states]) for k in ("type", "side", "dist", "size", "log_dt")}
        books = [s.book.copy() for s in states]
        m0 = np.array([b.mid() for b in books])
        rngs = [np.random.default_rng(sd) for sd in seeds]
        for j, q in enumerate(qs):
            if q:
                books[j].market(np.sign(q), abs(q))
                tok = {"type": EXECUTE, "side": 1 if q > 0 else 0, "dist": 0, "size": size_bucket(abs(q)), "log_dt": 0.0}
                for k in hist:
                    hist[k][j] = np.r_[hist[k][j][1:], tok[k]]
        dt = np.median(hist["log_dt"], axis=1)
        for _ in range(h):
            with torch.no_grad():
                x = {k: torch.as_tensor(v, device=self.dev) for k, v in hist.items()}
                _, g = self.m(x)
                p = torch.softmax(g[:, -1].float(), -1).cpu().numpy()
            for j in range(B):
                u = rngs[j].random(2)
                c = min(int(np.searchsorted(np.cumsum(p[j]), u[0] * p[j].sum())), len(p[j]) - 1)
                typ, rest = divmod(c, 20)
                side, dist = divmod(rest, 10)
                cdf = np.cumsum(self.size_dist[typ])
                sz = min(int(np.searchsorted(cdf, u[1] * cdf[-1])), 7)
                books[j].apply(typ, side, dist, int(SIZES[sz]))
                tok = {"type": typ, "side": side, "dist": dist, "size": sz, "log_dt": dt[j]}
                for k in hist:
                    hist[k][j] = np.r_[hist[k][j][1:], tok[k]]
        return np.array([b.mid() for b in books]) - m0, [b.invalid for b in books]


def effects(subject, states, h, seeds, mirror=False):
    """Per state and size: mean over seeds of Z(X + a) - Z(X); buys of q, or with `mirror` sells
    of q on the mirrored state (in its own coordinates)."""
    out = np.full((len(states), len(QS)), np.nan)
    sts = [s.mirror() if mirror else s for s in states]
    sgn = -1 if mirror else 1
    if isinstance(subject, TransformerSubject):
        jobs = [(i, sgn * q, sd) for i in range(len(sts)) for q in (0,) + QS for sd in seeds]
        z = {}
        for c in range(0, len(jobs), 2048):
            part = jobs[c:c + 2048]
            res, _ = subject.rollout_batch([sts[i] for i, _, _ in part], [q for _, q, _ in part], h,
                                           [sd for _, _, sd in part])
            z.update({j: r for j, r in zip(part, res)})
    else:
        z = {(i, sgn * q, sd): subject.rollout(sts[i], sgn * q, h, sd)
             for i in range(len(sts)) for q in (0,) + QS for sd in seeds}
    for i in range(len(sts)):
        for k, q in enumerate(QS):
            d = [z[(i, sgn * q, sd)] - z[(i, 0, sd)] for sd in seeds]
            out[i, k] = np.nanmean(d)
    return out


def sample_states(hftpy, store, sym, n, h, seed=0, W=127):
    t = hftpy.tokens(store, hftpy.symbols(store)[sym], 34_500_000_000_000, 57_300_000_000_000)
    rng = np.random.default_rng(seed)
    one_tick = np.abs(t["mid_after"] - np.floor(t["mid_after"]) - 0.5) < 1e-9
    nxt = np.r_[t["ts"][1:] > t["ts"][:-1], False]
    ok = np.flatnonzero(one_tick & nxt)
    ok = ok[(ok > W + 1) & (ok < len(t["ts"]) - h - 1)]
    idx = np.sort(rng.choice(ok, n, replace=False))
    snap = hftpy.snapshots(store, hftpy.symbols(store)[sym], t["ts"][idx].tolist(), 10)
    states = []
    for k, i in enumerate(idx):
        bp, ap = snap["bid_px"][k] // 100, snap["ask_px"][k] // 100
        bids = {int(bp - j): int(snap["bid_qty"][k, j]) for j in range(10) if snap["bid_qty"][k, j]}
        asks = {int(ap + j): int(snap["ask_qty"][k, j]) for j in range(10) if snap["ask_qty"][k, j]}
        sl = lambda a, b: {key: t[key][a:b] for key in ("type", "side", "dist", "size", "log_dt")}
        states.append(State(L2(bids, asks), sl(i - W, i + 1), sl(i + 1, i + 1 + h)))
    return states, t


def empirical(hftpy, store, sym, tok, h):
    """Mid change h events after real market orders (sweeps merged), by size, one-tick spread."""
    tr = hftpy.trades(store, hftpy.symbols(store)[sym], 34_500_000_000_000, 57_300_000_000_000)
    k = np.searchsorted(tok["ts"], tr["ts"], side="right") - 1
    ok = (k + h < len(tok["ts"])) & np.isfinite(tr["mid_before"]) & (np.abs((tr["mid_before"] * 100) % 1 - 0.5) < 1e-6)
    d = tr["sign"][ok] * (tok["mid_after"][k[ok] + h] - tr["mid_before"][ok] * 100)
    sh = tr["shares"][ok]
    edges = [0] + [int(np.sqrt(a * b)) for a, b in zip(QS[:-1], QS[1:])] + [10 ** 9]
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = (sh >= lo) & (sh < hi)
        x = d[sel]
        out.append({"n": int(sel.sum()), "mean": float(x.mean()) if len(x) else None,
                    "se": float(x.std(ddof=1) / np.sqrt(len(x))) if len(x) > 1 else None})
    return out


def relations(eff_buy, eff_sell_mirror, emp):
    """Pass/fail of the structural relations from per-state effects (rows states, columns QS).
    Side symmetry: Delta(X, buy q) = -Delta(mirror X, sell q)."""
    n = eff_buy.shape[0]
    mb = np.nanmean(eff_buy, 0)
    se_b = np.nanstd(eff_buy, 0, ddof=1) / np.sqrt(n)
    diff = eff_buy + eff_sell_mirror
    sym_z = np.nanmean(diff, 0) / (np.nanstd(diff, 0, ddof=1) / np.sqrt(n) + 1e-12)
    mono_steps = [(mb[k + 1] - mb[k]) / np.sqrt(se_b[k] ** 2 + se_b[k + 1] ** 2 + 1e-24) for k in range(len(QS) - 1)]
    pos = mb > 0
    psi = float(np.polyfit(np.log(np.array(QS)[pos]), np.log(mb[pos]), 1)[0]) if pos.sum() >= 3 else float("nan")
    em = np.array([e["mean"] if e["mean"] is not None else np.nan for e in emp])
    epos = em > 0
    psi_emp = float(np.polyfit(np.log(np.array(QS)[epos]), np.log(em[epos]), 1)[0]) if epos.sum() >= 3 else float("nan")
    # Band of the empirical exponent: bucket means redrawn from their standard errors.
    es = np.array([e["se"] if e["se"] is not None else np.nan for e in emp])
    rng = np.random.default_rng(0)
    draws = []
    for _ in range(2000):
        y = em + rng.normal(size=len(em)) * es
        okd = np.isfinite(y) & (y > 0)
        if okd.sum() >= 3:
            draws.append(np.polyfit(np.log(np.array(QS)[okd]), np.log(y[okd]), 1)[0])
    lo, hi = np.quantile(draws, [0.025, 0.975]) if draws else (np.nan, np.nan)
    level_ratio = mb / em
    return {
        "mean_effect": mb.tolist(), "se": se_b.tolist(),
        "side_symmetry_z": sym_z.tolist(), "side_symmetry_pass": bool(np.all(np.abs(sym_z) < 3)),
        "monotonic_z": mono_steps, "monotonic_pass": bool(np.all(np.array(mono_steps) > -2)),
        "smallest_q_failing_monotonicity": next((QS[k + 1] for k, z in enumerate(mono_steps) if z <= -2), None),
        "psi": psi, "psi_empirical": psi_emp, "psi_empirical_band": [float(lo), float(hi)],
        "concavity_pass": bool(np.isfinite(psi) and 0 < psi < 1),
        "psi_within_empirical_band": bool(np.isfinite(psi) and lo <= psi <= hi),
        "effects_buy": eff_buy.tolist(), "effects_sell_mirror": eff_sell_mirror.tolist(),
        "level_ratio_to_empirical": level_ratio.tolist(),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("--store", required=True)
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--symbol", default="INTC")
    ap.add_argument("--transformer", required=True)
    ap.add_argument("--states", type=int, default=100)
    ap.add_argument("--seeds", type=int, default=16)
    ap.add_argument("--h", type=int, default=50)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sys.path[:0] = [a.py_build, str(pathlib.Path(__file__).resolve().parent)]
    import hftpy
    import qr
    import torch
    import transformer as tr

    states, tok = sample_states(hftpy, a.store, a.symbol, a.states, a.h)
    emp = empirical(hftpy, a.store, a.symbol, tok, a.h)
    fits, aes = [], None
    for s in a.train:
        ev = hftpy.qr_events(s, hftpy.symbols(s)[a.symbol], 3, 100, 34_500_000_000_000, 57_300_000_000_000)
        f = qr.calibrate(ev, 3, 50, aes=aes)
        aes = f["aes"]
        fits.append(f)
        del ev
    fit = qr.pool(fits)
    fit["init"] = np.where(fit["init"].sum(1, keepdims=True) > 0, fit["init"], 1.0 / (fit["N"] + 1))
    m = tr.EventTransformer()
    m.load_state_dict(torch.load(pathlib.Path(a.transformer) / "transformer.pt"))
    size_dist = np.stack([np.bincount(tok["size"][tok["type"] == ty], minlength=8) + 1.0 for ty in range(5)])
    seeds = list(range(a.seeds))
    subjects = [ReplaySubject(), QRSubject(fit), TransformerSubject(m, size_dist, "cuda" if torch.cuda.is_available() else "cpu")]
    res = {"symbol": a.symbol, "states": a.states, "seeds": a.seeds, "h": a.h, "QS": list(QS), "empirical": emp,
           "subjects": {}}
    for subj in subjects:
        buy = effects(subj, states, a.h, seeds)
        sell_m = effects(subj, states, a.h, seeds, mirror=True)
        r = relations(buy, sell_m, emp)
        # Stability: the null rollouts' outcome distribution under two disjoint seed sets.
        if isinstance(subj, TransformerSubject):
            z1, inv = subj.rollout_batch(states, [0] * len(states), a.h, [1000 + i for i in range(len(states))])
            z2, _ = subj.rollout_batch(states, [0] * len(states), a.h, [5000 + i for i in range(len(states))])
            r["invalid_ops_per_rollout"] = float(np.mean(inv))
        else:
            z1 = np.array([subj.rollout(s, 0, a.h, 1000 + i) for i, s in enumerate(states)])
            z2 = np.array([subj.rollout(s, 0, a.h, 5000 + i) for i, s in enumerate(states)])
        from scipy.stats import ks_2samp

        r["stability_ks_p"] = float(ks_2samp(z1, z2).pvalue) if subj.name != "replay" else 1.0
        r["stability_pass"] = r["stability_ks_p"] > 0.01
        res["subjects"][subj.name] = r
        print(subj.name, json.dumps({k: r[k] for k in ("side_symmetry_pass", "monotonic_pass", "psi", "stability_pass")}), flush=True)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "validity.json").write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
