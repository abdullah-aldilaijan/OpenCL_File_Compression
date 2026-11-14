"""Builds the 100 GiB mixed set: python -I make_100g.py <object-folder> <out-folder>

5 x 1 GiB + 5 x 5 GiB + 5 x 10 GiB (80 GiB) plus 20 GiB of MiB-sized files:
1,120 x 1 MiB, 256 x 10 MiB, 128 x 50 MiB, 64 x 100 MiB and 8 x 500 MiB (20,480 MiB).
1,591 files, exactly 100 GiB. Files are built from 4 MiB slices like make_sizes.py.
The Object folder is smaller than 100 GiB, so most slices appear twice in this set: ZIP is not
affected (chunks are independent), but .ocpk stores the repeats once.
"""
import bisect, os, sys

MIB = 1 << 20
SLICE = 4 * MIB
STRIDE = 7919
PLAN = [("big", 1024, 5), ("big", 5120, 5), ("big", 10240, 5),
        ("mib", 1, 1120), ("mib", 10, 256), ("mib", 50, 128), ("mib", 100, 64), ("mib", 500, 8)]
assert sum(mb * n for _, mb, n in PLAN) == 100 * 1024

src, out = sys.argv[1], sys.argv[2]
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
handles = {}


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
        f = handles.get(path)
        if f is None:
            if len(handles) > 64:
                for h in handles.values():
                    h.close()
                handles.clear()
            f = handles[path] = open(path, "rb")
        f.seek(inner)
        parts.append(f.read(take))
        offset += take
        n -= take
    return b"".join(parts)


k = 6 * 1009  # start somewhere other than the other sets
for group, mb, count in PLAN:
    folder = os.path.join(out, f"{group}_{mb}MiB")
    os.makedirs(folder, exist_ok=True)
    for i in range(count):
        path = os.path.join(folder, f"f{i + 1:04d}.bin")
        size = mb * MIB
        if os.path.exists(path) and os.path.getsize(path) == size:
            k += (size + SLICE - 1) // SLICE
            continue
        with open(path, "wb") as w:
            left = size
            while left:
                n = min(left, SLICE)
                w.write(read_stream((k * STRIDE % slices) * SLICE, n))
                k += 1
                left -= n
    print(f"{count} x {mb} MiB done", flush=True)
print("100 GiB set ready in", out)
