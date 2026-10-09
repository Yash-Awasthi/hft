"""Independent slow L3 book used to cross-check the C++ books.

Reads one symbol file of a store with the zstd command-line tool, decodes ITCH by hand and
prints the same per-symbol summary line as apps/book_replay: BBO change count and an FNV-1a
hash over (seq, bid_px, ask_px, bid_qty, ask_qty) for every BBO change.

Usage: python research/ref_book.py <store-dir> <locate> [--crossed]
"""

import heapq
import struct
import subprocess
import sys

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1


def records(path):
    raw = subprocess.run(["zstd", "-dcq", path], check=True, capture_output=True).stdout
    pos = 0
    while pos < len(raw):
        seq, n = struct.unpack_from("<QH", raw, pos)
        pos += 10
        yield seq, raw[pos : pos + n]
        pos += n


class Side:
    def __init__(self, sign):
        self.sign = sign  # +1 bids (max first), -1 asks (min first)
        self.qty = {}
        self.heap = []

    def change(self, price, delta):
        q = self.qty.get(price, 0) + delta
        assert q >= 0
        if q == 0:
            del self.qty[price]
        else:
            if price not in self.qty:
                heapq.heappush(self.heap, -self.sign * price)
            self.qty[price] = q

    def best(self):
        while self.heap:
            price = -self.sign * self.heap[0]
            if price in self.qty:
                return price, self.qty[price]
            heapq.heappop(self.heap)
        return 0, 0


def main():
    store, locate = sys.argv[1], int(sys.argv[2])
    show_crossed = "--crossed" in sys.argv
    sides = {b"B": Side(1), b"S": Side(-1)}
    orders = {}
    h = FNV_OFFSET
    changes = errors = executed = 0
    last = (0, 0, 0, 0)
    state = b"?"
    for seq, m in records(f"{store}/{locate:05d}.zst"):
        t = m[0:1]
        ts = int.from_bytes(m[5:11], "big")
        if t in (b"A", b"F"):
            ref, side, shares, price = struct.unpack_from(">QcI8xI", m, 11)
            if ref in orders or shares == 0:
                errors += 1
                continue
            orders[ref] = [side, price, shares]
            sides[side].change(price, shares)
        elif t in (b"E", b"C", b"X"):
            ref, shares = struct.unpack_from(">QI", m, 11)
            if t != b"X":
                executed += shares
            o = orders.get(ref)
            if o is None or shares == 0 or shares > o[2]:
                errors += 1
                continue
            o[2] -= shares
            sides[o[0]].change(o[1], -shares)
            if o[2] == 0:
                del orders[ref]
        elif t == b"D":
            (ref,) = struct.unpack_from(">Q", m, 11)
            o = orders.pop(ref, None)
            if o is None:
                errors += 1
                continue
            sides[o[0]].change(o[1], -o[2])
        elif t == b"U":
            old, new, shares, price = struct.unpack_from(">QQII", m, 11)
            o = orders.get(old)
            if o is None or shares == 0 or (new != old and new in orders):
                errors += 1
                continue
            del orders[old]
            sides[o[0]].change(o[1], -o[2])
            orders[new] = [o[0], price, shares]
            sides[o[0]].change(price, shares)
        else:
            if t == b"H":
                state = m[19:20]
            continue
        bp, bq = sides[b"B"].best()
        ap, aq = sides[b"S"].best()
        cur = (bp, ap, bq, aq)
        if cur == last:
            continue
        last = cur
        changes += 1
        for b in struct.pack("<QIIQQ", seq, bp, ap, bq, aq):
            h = ((h ^ b) * FNV_PRIME) & MASK
        if show_crossed and bp and ap and bp > ap:
            hh, rem = divmod(ts // 10**9, 3600)
            print(f"crossed {hh:02d}:{rem // 60:02d}:{rem % 60:02d}.{ts % 10**9:09d} "
                  f"seq={seq} {t.decode()} state={state.decode()} bid={bp}x{bq} ask={ap}x{aq}")
    print(f"{locate} changes={changes} errors={errors} executed={executed} "
          f"orders={len(orders)} bbo_hash={h:016x}")


if __name__ == "__main__":
    main()
