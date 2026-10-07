"""Features exported in batch by the Python module equal, bit for bit, those computed in
streaming mode through the backtest scheduler (apps/feature_stream), on the committed
fixture. Exits non-zero on any difference.

Usage: python research/tests/check_batch_streaming.py <build-dir-with-hftpy> <release-build-dir>
"""

import gzip
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    py_build, bin_build = map(pathlib.Path, sys.argv[1:3])
    sys.path.insert(0, str(py_build))
    import hftpy

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        raw = subprocess.run(["zstd", "-dcq", ROOT / "tests/data/flow-7-4-20k.itch.zst"],
                             check=True, capture_output=True).stdout
        (tmp / "day.gz").write_bytes(gzip.compress(raw, compresslevel=1))
        store = tmp / "store"
        store.mkdir()
        subprocess.run([bin_build / "apps/ingest", store, tmp / "day.gz"], check=True,
                       capture_output=True)

        targets, index = [0, 1, 2], [3]
        batch = hftpy.features(str(store), targets, index, [1] * len(targets))
        k = len(batch["names"])
        seq = np.concatenate([batch[t]["seq"] for t in targets])
        x = np.concatenate([batch[t]["X"] for t in targets])
        order = np.argsort(seq, kind="stable")
        seq, x = seq[order], x[order]

        out = tmp / "stream.bin"
        subprocess.run([bin_build / "apps/feature_stream", store, "50000", out,
                        ",".join(map(str, index))] + [str(t) for t in targets], check=True)
        rows = np.fromfile(out, dtype=np.dtype([("seq", "<u8"), ("x", "<f8", (k,))]))

    same = (len(rows) == len(seq) and np.array_equal(rows["seq"], seq)
            and np.array_equal(rows["x"].view(np.uint64), x.view(np.uint64)))
    print(f"{len(seq)} rows, {k} features, batch == streaming: {same}")
    sys.exit(0 if same and len(seq) > 1000 else 1)


if __name__ == "__main__":
    main()
