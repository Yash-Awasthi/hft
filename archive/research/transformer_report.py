"""MS9 report: event transformer training, kernel agreement with PyTorch and step latency.

Usage: python research/transformer_report.py <transformer-dir> <out.md>
"""

import json
import pathlib
import re
import sys


def main():
    d = pathlib.Path(sys.argv[1])
    meta = json.loads((d / "transformer.json").read_text())
    agr = json.loads((d / "agreement.json").read_text())
    bench = (d / "bench.txt").read_text()
    med = dict(re.findall(r"EventStep/(\w+)_median\s+(\d+) ns", bench))
    last = meta["log"][-1]
    lines = [
        "# MS9: event transformer and in-loop inference kernel", "",
        f"Model: d_model 32, 2 layers, 2 heads, MLP 64, sliding window 64 with an ALiBi bias, "
        f"{meta['params']:,} parameters ({meta['params'] * 4 / 1024:.1f} KB in float32; the design's 10k "
        f"parameters / 40 KB in L1 is missed by about 2x). Trained on the train days for {len(meta['symbols'])} "
        f"large-tick universe stocks ({', '.join(meta['symbols'])}), 3,000 steps of 32 x 1,024 events.", "",
        "## Validation day (first 400 chunks of 1,024 events)", "",
        "| Step | Forecast CE (nats, 2 horizons) | IC, 10 events | Direction accuracy, non-flat, 10 events | "
        "Generator CE | Unigram CE |", "|---|---|---|---|---|---|",
        *[f"| {r['step']} | {r['forecast_ce']:.3f} | {r['ic_h10']:.3f} | {r['acc_h10_nonflat']:.1%} | "
          f"{r['gen_ce']:.3f} | {r['gen_unigram_ce']:.3f} |" for r in meta["log"]], "",
        f"The generator's next-event cross entropy is {last['gen_unigram_ce'] - last['gen_ce']:.3f} nats below the "
        "unigram baseline. The forecast horizon is in events, so the IC is not comparable with the clock-time "
        "ICs of MS4.", "",
        "## Kernel against PyTorch", "",
        f"{agr['events']:,} consecutive validation-day events of {agr['symbol']}, trained weights.", "",
        "| Path | Max abs logit error | Same arg-max, 10 events | Same arg-max, 100 events | Same next-event class |",
        "|---|---|---|---|---|",
        *[f"| {k} | {v['max_abs_err']:.1e} | {v['agree_h10']:.4%} | {v['agree_h100']:.4%} | {v['agree_gen']:.4%} |"
          for k, v in agr.items() if isinstance(v, dict)], "",
        "Golden test in CI (`EventTransformer.MatchesPyTorchAndAgreesOnDecisions`): random weights, 300 events, "
        "window 32 so the ring wraps; error below 2e-5 and identical forecast decisions on both paths.", "",
        "## Step latency at the wall-clock baseline", "",
        "| Path | Median ns per event | Target |", "|---|---|---|",
        f"| Scalar C++ | {int(med.get('scalar', 0)):,} | - |",
        f"| AVX2 float32 | {int(med.get('avx2', 0)):,} | < 2,000 "
        f"({'met' if int(med.get('avx2', 0)) < 2000 else 'miss'}) |", "",
        "`hft_bench --benchmark_filter=EventStep --benchmark_repetitions=10` with `HFT_TRANSFORMER` pointing at "
        "the trained weights, pinned to one virtual CPU; window full. Idle machine on mains, turbo off "
        "(maximum processor state 99% for every core class, boost mode off; about 2.4 GHz).", "",
        "## Not built", "",
        "- int8 and AVX-VNNI paths, ONNX Runtime and LibTorch baselines (Stretch).",
        "- Inference cost in the backtest's processing latency, stale-event policy, PnL against model latency.",
        "- Quantisation loss in IC and PnL (no int8 path).", ""]
    pathlib.Path(sys.argv[2]).write_text("\n".join(lines))


if __name__ == "__main__":
    main()
