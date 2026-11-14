"""Builds the 4 x 10 GiB set: python -I make_4x10g.py <object-folder> <sizes-folder> <out-folder>

Files 1-3 are hard links to sizes/s010G (no extra disk space). File 4 is new: the next 2,560 slices of
the same slice sequence make_sizes.py used for s010G, so it has the same kind of mix.
The set lives outside the sizes folder so the "everything" source is unchanged.
"""
import bisect, os, sys

SLICE = 4 << 20
SIZE = 10240 << 20
STRIDE = 7919
src, sizes, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
for i in range(1, 4):
    link, target = os.path.join(out, f"f{i:02d}.bin"), os.path.join(sizes, "s010G", f"f{i:02d}.bin")
    if not os.path.exists(link):
        os.link(target, link)

files = []
for d, dirs, names in os.walk(src):
    dirs.sort()
    for n in sorted(names):
        p = os.path.join(d, n)
        files.append((p, os.path.getsize(p)))
starts, at = [], 0
for _, s in files:
    starts.append(at)
    at += s
slices = at // SLICE


def read_stream(offset, n):
    parts = []
    while n:
        k = bisect.bisect_right(starts, offset) - 1
        path, size = files[k]
        inner = offset - starts[k]
        take = min(n, size - inner)
        if take <= 0:
            offset = starts[k] + size
            continue
        with open(path, "rb") as f:
            f.seek(inner)
            parts.append(f.read(take))
        offset += take
        n -= take
    return b"".join(parts)


path = os.path.join(out, "f04.bin")
if not (os.path.exists(path) and os.path.getsize(path) == SIZE):
    k = 5 * 1009 + 3 * (SIZE // SLICE)  # s010G is set 5; its three files used 3 x 2,560 slices
    with open(path, "wb") as w:
        for _ in range(SIZE // SLICE):
            w.write(read_stream((k * STRIDE % slices) * SLICE, SLICE))
            k += 1
print("4 x 10 GiB ready in", out)
