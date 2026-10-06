"""Independent message counter for a BinaryFILE ITCH stream on stdin.

Reads only the length prefix and type byte, so it shares no decoding code with the
C++ decoder; used to cross-check `itch_count`.
"""

import sys
from collections import Counter

data = sys.stdin.buffer.read()
counts = Counter()
off = 0
frames = 0
while off + 2 <= len(data):
    n = int.from_bytes(data[off : off + 2], "big")
    if off + 2 + n > len(data):
        break
    if n:
        counts[chr(data[off + 2])] += 1
    off += 2 + n
    frames += 1

for t in sorted(counts):
    print(t, counts[t])
print("frames", frames)
print("bad", 0)
print("trailing_bytes", len(data) - off)
