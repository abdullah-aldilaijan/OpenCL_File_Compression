"""How speed scales with processing units: python -I units.py <oclzip.exe> <sizes-folder> <rep>

CPU threads (ZIP on CPU), GPU batch size (ZIP on GPU), CPU threads next to the GPU (ALL) and .ocpk
compression threads, on three sources. Results: <OCLZIP_BENCH_OUT>\\units.jsonl.
All ZIPs at one chunk size must be byte-identical whatever the settings, so each ZIP's CRC is kept.
"""
import json, os, subprocess, sys, time, zlib

OUT = os.environ.get("OCLZIP_BENCH_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench_out"))  # results and temporary archives
RESULTS = os.path.join(OUT, "units.jsonl")


def configs():
    out = []
    for t in [1, 2, 4, 8, 12, 16]:
        out.append((f"zip-CPU-threads{t}", "zip", "CPU", {"OCLZIP_CPU_THREADS": str(t)}))
    for c in [512, 1024, 2048, 4096, 8192, 16384]:
        out.append((f"zip-GPU-batch{c}", "zip", os.environ.get("MATRIX_GPU", "NVIDIA"), {"OCLZIP_BATCH_CHUNKS": str(c), "OCLZIP_LAUNCH_CHUNKS": str(c)}))
    for t in [1, 4, 8, 13]:
        out.append((f"zip-ALL-cputhreads{t}", "zip", "ALL", {"OCLZIP_CPU_THREADS": str(t)}))
    for t in [1, 2, 4, 8, 16]:
        out.append((f"ocpk-fast-threads{t}", "pack", "fast", {"OCLZIP_PACK_THREADS": str(t)}))
    return out


def warm(path):
    files = [path] if os.path.isfile(path) else [os.path.join(d, n) for d, _, ns in os.walk(path) for n in ns]
    buf = bytearray(16 << 20)
    total = 0
    for f in files:
        with open(f, "rb", buffering=0) as h:
            while True:
                n = h.readinto(buf)
                if not n:
                    break
                total += n
    return total, len(files)


def crc_of(path):
    crc = 0
    buf = bytearray(16 << 20)
    with open(path, "rb", buffering=0) as f:
        while True:
            n = f.readinto(buf)
            if not n:
                return f"{crc:08x}"
            crc = zlib.crc32(memoryview(buf)[:n], crc)


def main():
    exe, root, rep = sys.argv[1], sys.argv[2], int(sys.argv[3])
    sources = [("s100M/all", os.path.join(root, "s100M")), ("s001G/one", os.path.join(root, "s001G", "f01.bin")),
               ("s010G/one", os.path.join(root, "s010G", "f01.bin"))]
    done = set()
    if os.path.exists(RESULTS):
        for line in open(RESULTS, encoding="utf-8"):
            d = json.loads(line)
            done.add((d["rep"], d["source"], d["config"]))
    for source, path in sources:
        todo = [c for c in configs() if (rep, source, c[0]) not in done]
        if not todo:
            continue
        src_bytes, src_files = warm(path)
        shift = (rep * 5) % len(todo)
        for name, kind, arg, env_extra in todo[shift:] + todo[:shift]:
            target = os.path.join(OUT, "u_run" + (".zip" if kind == "zip" else ".ocpk"))
            if os.path.exists(target):
                os.remove(target)
            cmd = [exe, "compress", path, target, "--device", arg] if kind == "zip" else \
                  [exe, "pack", path, target, "--level", arg]
            t0 = time.perf_counter()
            p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                               env=dict(os.environ, **env_extra))
            wall = time.perf_counter() - t0
            rec = {"rep": rep, "source": source, "config": name, "exit": p.returncode, "wall": round(wall, 3),
                   "src_bytes": src_bytes, "time": time.strftime("%Y-%m-%d %H:%M:%S")}
            text = p.stdout + p.stderr
            for line in text.splitlines():
                if line.startswith("Limited by:"):
                    rec["limited_by"] = line.split(":", 1)[1].strip()
                if "% of chunks" in line:
                    rec.setdefault("split", []).append(" ".join(line.split()))
                if line.startswith("Standard ZIP") or line.startswith("Solid pack"):
                    rec["engine"] = line
            if p.returncode == 0 and os.path.exists(target):
                rec["bytes"] = os.path.getsize(target)
                rec["mb_s"] = round(src_bytes / wall / 1e6, 1)
                if kind == "zip" and rep == 1:
                    rec["archive_crc"] = crc_of(target)
            else:
                rec["error"] = text.strip().splitlines()[-1:]
            if os.path.exists(target):
                os.remove(target)
            with open(RESULTS, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
            print(f"{source:10} {name:22} {wall:8.2f}s {rec.get('mb_s', 0):8.1f} MB/s {rec.get('limited_by', '')}", flush=True)


main()
