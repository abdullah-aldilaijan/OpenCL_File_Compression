"""Builds the file-size test sets: python -I make_sizes.py <object-folder> <out-folder>

Each set is a folder of equal-size files. Every file is assembled from 4 MiB slices taken from all
over the source folder (seen as one long stream, stepping by a large stride), so every file gets a
similar mix of video, installers, documents and so on, and sizes can be compared fairly. Slices are
not reused inside a set, so ZIP and .ocpk results are not inflated by repeated data.
"""
import os, sys, time

SLICE = 4 << 20
MIB = 1 << 20
SETS = [("s010M", 10 * MIB, 50), ("s050M", 50 * MIB, 20), ("s100M", 100 * MIB, 20),
        ("s500M", 500 * MIB, 8), ("s001G", 1024 * MIB, 6), ("s010G", 10240 * MIB, 3)]

src, out = sys.argv[1], sys.argv[2]
files = []
for d, dirs, names in os.walk(src):
    dirs.sort()
    for n in sorted(names):
        p = os.path.join(d, n)
        files.append((p, os.path.getsize(p)))
total = sum(s for _, s in files)
starts, at = [], 0
for _, s in files:
    starts.append(at)
    at += s
slices = total // SLICE
print(f"source: {len(files)} files, {total/1e9:.2f} GB, {slices} slices")

import bisect
handles = {}


def read_stream(offset, n):
    """n bytes of the concatenated source stream from offset."""
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


STRIDE = 7919  # prime, so stepping by it visits every slice once before repeating
for set_index, (name, size, count) in enumerate(SETS):
    folder = os.path.join(out, name)
    os.makedirs(folder, exist_ok=True)
    t = time.time()
    k = set_index * 1009  # each set starts somewhere else
    for i in range(count):
        path = os.path.join(folder, f"f{i+1:02d}.bin")
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
    print(f"{name}: {count} x {size // MIB} MiB in {time.time() - t:.1f}s", flush=True)
