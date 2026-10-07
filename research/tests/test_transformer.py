"""Event transformer checks: causal, with a receptive field of layers x (window - 1) events, and
the flat weight export reads back exactly."""

import pathlib
import sys
import tempfile

import numpy as np
import torch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import transformer as tr  # noqa: E402


def batch(n, seed):
    g = torch.Generator().manual_seed(seed)
    return {"type": torch.randint(0, 5, (1, n), generator=g), "side": torch.randint(0, 2, (1, n), generator=g),
            "dist": torch.randint(0, 10, (1, n), generator=g), "size": torch.randint(0, 8, (1, n), generator=g),
            "log_dt": torch.rand((1, n), generator=g) * 5}


def test_outputs_ignore_the_future_and_events_beyond_the_receptive_field():
    torch.manual_seed(0)
    m = tr.EventTransformer(window=16).eval()
    x = batch(200, 1)
    t = 150
    with torch.no_grad():
        f0, g0 = m(x)
        y = {k: v.clone() for k, v in x.items()}
        y["type"][0, t + 1:] = (y["type"][0, t + 1:] + 1) % 5  # future
        cut = t - m.layers * (m.window - 1)
        y["dist"][0, :cut] = (y["dist"][0, :cut] + 3) % 10  # beyond the receptive field
        f1, g1 = m(y)
    assert torch.equal(f0[0, t], f1[0, t]) and torch.equal(g0[0, t], g1[0, t])
    y["dist"][0, cut] = (y["dist"][0, cut] + 3) % 10  # inside: must matter
    with torch.no_grad():
        f2, _ = m(y)
    assert not torch.equal(f0[0, t], f2[0, t])


def test_export_reads_back():
    torch.manual_seed(1)
    m = tr.EventTransformer()
    with tempfile.TemporaryDirectory() as d:
        path = pathlib.Path(d) / "w.bin"
        tr.export(m, path)
        hdr, flat = tr.read_export(path)
    assert hdr["d_model"] == m.d and hdr["layers"] == m.layers and hdr["window"] == m.window
    want = np.concatenate([p.detach().numpy().ravel() for _, p in tr.ordered_params(m)])
    assert np.array_equal(flat, want.astype(np.float32))


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
