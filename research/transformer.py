"""Event transformer (DESIGN.md section 11): one small causal model over book-event tokens with a
forecast head (down / flat / up mid at two event horizons, the signal) and a generator head
(the next event's type, side and distance, the simulator).

Input per event: type, side, distance bucket, size bucket (embeddings, summed) and log1p of
the microseconds since the previous event (a linear map). Pre-norm blocks with RMSNorm,
two-head attention over a sliding window of the last `window` events with an ALiBi distance
bias (so cached keys and values equal a full recompute), and a tanh-GELU MLP.

Training: train days, universe large-tick stocks, chunks of CHUNK events; validation: the
validation day. Export: a flat float32 file read by src/strategy/event_transformer.hpp.

Usage: python research/transformer.py <py-build> --train <store>... --val <store> --out <dir>
"""

import argparse
import json
import math
import pathlib
import struct
import sys
import time
import tomllib

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

ROOT = pathlib.Path(__file__).resolve().parents[1]
N_TYPE, N_SIDE, N_DIST, N_SIZE = 5, 2, 10, 8
N_GEN = N_TYPE * N_SIDE * N_DIST
HORIZONS = (10, 100)
FLAT = 0.25  # ticks: |mid change| below this is flat
CHUNK = 1024
MAGIC = 0x45564E54  # "EVNT"


class RMSNorm(nn.Module):
    def __init__(self, d, eps=1e-5):
        super().__init__()
        self.w, self.eps = nn.Parameter(torch.ones(d)), eps

    def forward(self, x):
        return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + self.eps) * self.w


class Block(nn.Module):
    def __init__(self, d, heads, hidden):
        super().__init__()
        self.n1, self.n2 = RMSNorm(d), RMSNorm(d)
        self.qkv, self.o = nn.Linear(d, 3 * d), nn.Linear(d, d)
        self.fc1, self.fc2 = nn.Linear(d, hidden), nn.Linear(hidden, d)
        self.heads = heads

    def forward(self, x, bias):
        B, T, d = x.shape
        h = self.heads
        q, k, v = self.qkv(self.n1(x)).view(B, T, 3, h, d // h).permute(2, 0, 3, 1, 4)
        a = q @ k.transpose(-1, -2) / math.sqrt(d // h) + bias
        y = (a.softmax(-1) @ v).transpose(1, 2).reshape(B, T, d)
        x = x + self.o(y)
        return x + self.fc2(F.gelu(self.fc1(self.n2(x)), approximate="tanh"))


class EventTransformer(nn.Module):
    def __init__(self, d=32, layers=2, heads=2, hidden=64, window=64):
        super().__init__()
        self.d, self.layers, self.heads, self.hidden, self.window = d, layers, heads, hidden, window
        self.e_type, self.e_side = nn.Embedding(N_TYPE, d), nn.Embedding(N_SIDE, d)
        self.e_dist, self.e_size = nn.Embedding(N_DIST, d), nn.Embedding(N_SIZE, d)
        self.e_dt = nn.Linear(1, d)
        self.blocks = nn.ModuleList([Block(d, heads, hidden) for _ in range(layers)])
        self.norm = RMSNorm(d)
        self.forecast = nn.Linear(d, 3 * len(HORIZONS))
        self.gen = nn.Linear(d, N_GEN)
        self.register_buffer("slopes", torch.tensor([2.0 ** (-8 * (i + 1) / heads) for i in range(heads)]))

    def bias(self, T, device):
        i = torch.arange(T, device=device)
        dist = (i[:, None] - i[None, :]).float()
        ok = (dist >= 0) & (dist < self.window)
        b = -self.slopes[:, None, None] * dist
        return torch.where(ok, b, torch.tensor(float("-inf"), device=device))

    def forward(self, x):
        h = (self.e_type(x["type"].long()) + self.e_side(x["side"].long()) + self.e_dist(x["dist"].long())
             + self.e_size(x["size"].long()) + self.e_dt(x["log_dt"].float().unsqueeze(-1)))
        bias = self.bias(h.shape[1], h.device)
        for b in self.blocks:
            h = b(h, bias)
        h = self.norm(h)
        return self.forecast(h), self.gen(h)


def ordered_params(m):
    """Export order, matched by the C++ loader."""
    out = [("e_type", m.e_type.weight), ("e_side", m.e_side.weight), ("e_dist", m.e_dist.weight),
           ("e_size", m.e_size.weight), ("e_dt.w", m.e_dt.weight.view(-1)), ("e_dt.b", m.e_dt.bias)]
    for i, b in enumerate(m.blocks):
        out += [(f"b{i}.n1", b.n1.w), (f"b{i}.qkv.w", b.qkv.weight), (f"b{i}.qkv.b", b.qkv.bias),
                (f"b{i}.o.w", b.o.weight), (f"b{i}.o.b", b.o.bias), (f"b{i}.n2", b.n2.w),
                (f"b{i}.fc1.w", b.fc1.weight), (f"b{i}.fc1.b", b.fc1.bias),
                (f"b{i}.fc2.w", b.fc2.weight), (f"b{i}.fc2.b", b.fc2.bias)]
    out += [("norm", m.norm.w), ("forecast.w", m.forecast.weight), ("forecast.b", m.forecast.bias),
            ("gen.w", m.gen.weight), ("gen.b", m.gen.bias), ("slopes", m.slopes)]
    return out


HEADER = "<8I"


def export(m, path):
    flat = np.concatenate([p.detach().cpu().numpy().astype(np.float32).ravel() for _, p in ordered_params(m)])
    with open(path, "wb") as f:
        f.write(struct.pack(HEADER, MAGIC, m.d, m.layers, m.heads, m.hidden, m.window, len(flat), 3 * len(HORIZONS)))
        f.write(flat.tobytes())


def read_export(path):
    raw = pathlib.Path(path).read_bytes()
    k = struct.calcsize(HEADER)
    magic, d, layers, heads, hidden, window, n, nf = struct.unpack(HEADER, raw[:k])
    assert magic == MAGIC
    return ({"d_model": d, "layers": layers, "heads": heads, "hidden": hidden, "window": window, "forecast": nf},
            np.frombuffer(raw[k:], dtype=np.float32, count=n))


def golden(m, x, path):
    """Recorded inputs and full-sequence PyTorch outputs (float32, CPU) for the C++ kernel test:
    n, then per event type, side, dist, size (int32) and log_dt (float32), then per event the
    forecast and generator logits."""
    m = m.cpu().eval()
    with torch.no_grad():
        f, g = m({k: v.unsqueeze(0) for k, v in x.items()})
    n = len(x["type"])
    with open(path, "wb") as fh:
        fh.write(struct.pack("<I", n))
        ints = torch.stack([x["type"], x["side"], x["dist"], x["size"]], -1).int().numpy()
        for i in range(n):
            fh.write(ints[i].tobytes())
            fh.write(np.float32(x["log_dt"][i]).tobytes())
        fh.write(torch.cat([f[0], g[0]], -1).numpy().astype(np.float32).tobytes())


def full_outputs(m, x, chunk=4096):
    """Full-sequence outputs computed in chunks: each chunk is preceded by the receptive field
    (layers x (window - 1) events), so the result equals one pass over the whole sequence."""
    rf = m.layers * (m.window - 1)
    n = len(x["type"])
    fs, gs = [], []
    with torch.no_grad():
        for s in range(0, n, chunk):
            a = max(0, s - rf)
            f, g = m({k: torch.as_tensor(v[a:s + chunk]).unsqueeze(0) for k, v in x.items()})
            fs.append(f[0, s - a:].numpy())
            gs.append(g[0, s - a:].numpy())
    return np.concatenate(fs), np.concatenate(gs)


def agreement(hftpy, weights_bin, m, x):
    """Kernel against PyTorch on one token sequence: maximum absolute logit error and the share
    of events with the same arg-max forecast and next-event class."""
    f, g = full_outputs(m.cpu().eval(), x)
    out = {}
    for avx in (False, True):
        r = hftpy.transformer_run(str(weights_bin), *[np.ascontiguousarray(x[k]) for k in ("type", "side", "dist", "size")],
                                  np.ascontiguousarray(x["log_dt"], dtype=np.float32), avx2=avx)
        out["avx2" if avx else "scalar"] = {
            "max_abs_err": float(max(np.abs(r["forecast"] - f).max(), np.abs(r["gen"] - g).max())),
            **{f"agree_h{h}": float((r["forecast"][:, 3 * k:3 * k + 3].argmax(1) == f[:, 3 * k:3 * k + 3].argmax(1)).mean())
               for k, h in enumerate(HORIZONS)},
            "agree_gen": float((r["gen"].argmax(1) == g.argmax(1)).mean())}
    return out


# Data ------------------------------------------------------------------------------------------
def labels(mid):
    out = []
    for h in HORIZONS:
        dm = np.full(len(mid), np.nan)
        dm[:-h] = mid[h:] - mid[:-h]
        y = np.where(np.isnan(dm), -100, np.where(dm > FLAT, 2, np.where(dm < -FLAT, 0, 1)))
        out.append(y)
    gen = np.full(len(mid), -100)
    return np.stack(out, -1), dm, gen


def load_tokens(hftpy, stores, symbols, start=34_500_000_000_000, end=57_300_000_000_000):
    seqs = []
    for store in stores:
        loc = hftpy.symbols(store)
        for s in symbols:
            t = hftpy.tokens(store, loc[s], start, end)
            if len(t["ts"]) < CHUNK + max(HORIZONS):
                continue
            y, _, _ = labels(t["mid_after"])
            nxt = (t["type"].astype(np.int64) * N_SIDE + t["side"]) * N_DIST + t["dist"]
            g = np.r_[nxt[1:], -100]
            seqs.append({"type": t["type"], "side": t["side"], "dist": t["dist"], "size": t["size"],
                         "log_dt": t["log_dt"], "y": y, "g": g, "mid": t["mid_after"], "symbol": s})
    return seqs


def chunks(seqs, rng=None):
    out = []
    for s in seqs:
        n = len(s["type"]) // CHUNK
        for c in range(n):
            out.append((s, c * CHUNK))
    if rng is not None:
        rng.shuffle(out)
    return out


def to_batch(items, device):
    keys = ("type", "side", "dist", "size", "log_dt", "y", "g")
    b = {k: torch.as_tensor(np.stack([s[k][o:o + CHUNK] for s, o in items])).to(device) for k in keys}
    return b


def evaluate(m, seqs, device, max_chunks=400):
    m.eval()
    ce_f, ce_g, n_f, n_g, ic_x, ic_y = 0.0, 0.0, 0, 0, [], []
    base_counts = np.zeros(N_GEN)
    with torch.no_grad():
        items = chunks(seqs)[:max_chunks]
        for i in range(0, len(items), 16):
            b = to_batch(items[i:i + 16], device)
            f, g = m(b)
            for k in range(len(HORIZONS)):
                lf = f[..., 3 * k:3 * k + 3].reshape(-1, 3)
                yy = b["y"][..., k].reshape(-1)
                ok = yy >= 0
                ce_f += F.cross_entropy(lf[ok], yy[ok], reduction="sum").item()
                n_f += int(ok.sum())
                if k == 0:
                    p = lf.softmax(-1)
                    ic_x.append((p[:, 2] - p[:, 0])[ok].cpu().numpy())
                    ic_y.append((yy[ok] - 1).cpu().numpy())
            gg = b["g"].reshape(-1)
            ok = gg >= 0
            ce_g += F.cross_entropy(g.reshape(-1, N_GEN)[ok], gg[ok], reduction="sum").item()
            n_g += int(ok.sum())
            base_counts += np.bincount(gg[ok].cpu().numpy(), minlength=N_GEN)
    x, y = np.concatenate(ic_x), np.concatenate(ic_y)
    p = base_counts / base_counts.sum()
    unigram = float(-(base_counts * np.log(np.maximum(p, 1e-12))).sum() / base_counts.sum())
    m.train()
    return {"forecast_ce": ce_f / n_f, "gen_ce": ce_g / n_g, "gen_unigram_ce": unigram,
            "ic_h10": float(np.corrcoef(x, y)[0, 1]),
            "acc_h10_nonflat": float(np.mean(np.sign(x[y != 0]) == y[y != 0]))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--val", required=True)
    ap.add_argument("--symbols", type=int, default=10)
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sys.path.insert(0, a.py_build)
    import hftpy

    torch.manual_seed(0)
    rng = np.random.default_rng(0)
    uni = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    syms = uni["large_tick"]["symbols"][:a.symbols]
    t0 = time.time()
    train, val = load_tokens(hftpy, a.train, syms), load_tokens(hftpy, [a.val], syms)
    print(f"tokens: train {sum(len(s['type']) for s in train):,}, val {sum(len(s['type']) for s in val):,}, "
          f"{time.time() - t0:.0f}s", flush=True)
    device = "cuda" if torch.cuda.is_available() else "cpu"
    m = EventTransformer().to(device)
    opt = torch.optim.AdamW(m.parameters(), lr=3e-3, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=3e-3, total_steps=a.steps)
    items = chunks(train, rng)
    log = []
    for step in range(a.steps):
        batch = [items[(step * 32 + j) % len(items)] for j in range(32)]
        b = to_batch(batch, device)
        f, g = m(b)
        loss = sum(F.cross_entropy(f[..., 3 * k:3 * k + 3].reshape(-1, 3), b["y"][..., k].reshape(-1),
                                   ignore_index=-100) for k in range(len(HORIZONS)))
        loss = loss + F.cross_entropy(g.reshape(-1, N_GEN), b["g"].reshape(-1), ignore_index=-100)
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(m.parameters(), 1.0)
        opt.step()
        sched.step()
        if (step + 1) % 500 == 0:
            ev = evaluate(m, val, device)
            log.append({"step": step + 1, "loss": float(loss), **ev})
            print(json.dumps(log[-1]), flush=True)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    m = m.cpu().eval()
    torch.save(m.state_dict(), out / "transformer.pt")
    export(m, out / "transformer.bin")
    params = sum(p.numel() for p in m.parameters())
    (out / "transformer.json").write_text(json.dumps({"params": params, "symbols": syms, "log": log}, indent=1))
    print(f"params {params}, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    if sys.argv[1] == "golden":
        # python research/transformer.py golden <out-dir>: random weights and tokens, fixed seeds
        torch.manual_seed(7)
        mdl = EventTransformer(window=32)
        gen = torch.Generator().manual_seed(8)
        n = 300
        x = {"type": torch.randint(0, N_TYPE, (n,), generator=gen), "side": torch.randint(0, N_SIDE, (n,), generator=gen),
             "dist": torch.randint(0, N_DIST, (n,), generator=gen), "size": torch.randint(0, N_SIZE, (n,), generator=gen),
             "log_dt": torch.rand((n,), generator=gen) * 6}
        d = pathlib.Path(sys.argv[2])
        export(mdl, d / "evt_weights.bin")
        golden(mdl, x, d / "evt_golden.bin")
    else:
        main()
