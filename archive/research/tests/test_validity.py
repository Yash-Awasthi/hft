"""Counterfactual harness mechanics: the shared L2 book, mirroring, and paired rollouts."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import validity as v  # noqa: E402


def book():
    return v.L2({1000: 300, 999: 500}, {1001: 200, 1002: 400})


def test_market_order_walks_the_book_and_moves_the_mid():
    b = book()
    assert b.mid() == 1000.5
    b.market(+1, 300)  # takes 200 at 1001 and 100 at 1002
    assert b.asks == {1002: 300} and b.mid() == 1001.0
    b.market(-1, 900)  # more than the bid side holds: empties it
    assert b.bids == {} and np.isnan(b.mid())


def test_tokens_add_cancel_execute_by_distance():
    b = book()
    b.apply(v.ADD, 0, 0, 100)  # bid at distance 0 from 1000.5: 1000
    assert b.bids[1000] == 400
    b.apply(v.ADD, 1, 2, 50)  # ask at ceil(1000.5 + 2) = 1003
    assert b.asks[1003] == 50
    b.apply(v.CANCEL, 0, 1, 1000)  # cancel at 999, capped at the level
    assert 999 not in b.bids
    b.apply(v.EXECUTE, 1, 0, 150)  # resting ask executed at the best
    assert b.asks[1001] == 50
    assert b.invalid == 0
    b.apply(v.CANCEL, 0, 5, 10)  # nothing there
    assert b.invalid == 1


def test_mirror_is_an_involution_and_reflects_the_mid():
    b = book()
    m = b.mirror()
    assert m.mid() == -b.mid() and m.mirror().bids == b.bids and m.mirror().asks == b.asks


def test_paired_rollouts_with_the_null_intervention_have_zero_effect():
    rng = np.random.default_rng(0)
    subj = v.QRSubject(v.toy_qr())
    st = v.State(book(), None, None)
    for seed in range(5):
        z0 = subj.rollout(st, None, 30, seed)
        z1 = subj.rollout(st, None, 30, seed)
        assert z0 == z1
    assert rng is not None


def test_replay_is_side_symmetric_on_a_symmetric_book():
    st = v.State(v.L2({1000: 300, 999: 500}, {1001: 300, 1002: 500}), None,
                 {"type": np.array([0]), "side": np.array([0]), "dist": np.array([3]), "size": np.array([1])})
    buy = v.effects(v.ReplaySubject(), [st], 1, [0])
    sell_m = v.effects(v.ReplaySubject(), [st], 1, [0], mirror=True)
    # 100 shares leave the 300-share best; 300 clear it; more empty the side (no mid).
    assert buy[0, 0] == 0 and buy[0, 1] == 0.5 and np.isnan(buy[0, 2])
    assert np.allclose((buy + sell_m)[:, :2], 0)


def test_queue_reactive_subject_is_side_symmetric_at_a_wide_spread():
    subj = v.QRSubject(v.toy_qr())
    st = v.State(v.L2({1000: 300, 999: 500}, {1003: 200, 1004: 400}), None, None)
    seeds = list(range(300))
    buy, sell_m = v.effects(subj, [st], 20, seeds), v.effects(subj, [st], 20, seeds, mirror=True)
    assert np.all(np.abs(buy + sell_m)[:, :2] < 0.15), (buy, sell_m)


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
