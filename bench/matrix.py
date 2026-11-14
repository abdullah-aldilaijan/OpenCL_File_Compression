"""File-size / device / chunk-size test matrix.

python -I matrix.py <oclzip.exe> <sizes-folder> <rep> [--only SUBSTR]

Sources: for every size set, one file ("one") and the whole folder ("all"), plus every set
together ("everything"). Modes: ZIP on CPU, GPU and ALL with DEFLATE chunks of 4 to 256 KiB,
and .ocpk fast with 1/4/16 MiB blocks. Each result is one JSON line in
<OCLZIP_BENCH_OUT>\\matrix.jsonl; finished (rep, source, mode) runs are skipped on restart.

Verification (rep 1 only): every ZIP gets a CRC-32 of the whole archive file, so ZIPs from CPU,
GPU and ALL at one chunk size can be compared byte for byte, and the CPU one is fully inflated
with zipfile (every member's CRC checked). Every .ocpk is checked with `oclzip test`.
"""
import json, os, re, subprocess, sys, threading, time, zipfile, zlib
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from others import Meter  # noqa: E402

OUT = os.environ.get("OCLZIP_BENCH_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench_out"))  # results and temporary archives
RESULTS = os.path.join(OUT, "matrix.jsonl")
SETS = ["s010M", "s050M", "s100M", "s500M", "s001G", "s010G"]
# MATRIX_CHUNKS=2,512 runs only those chunk sizes (finished runs are skipped either way).
CHUNKS = [int(x) for x in os.environ.get("MATRIX_CHUNKS", "4,8,16,32,64,128,256").split(",")]
# MATRIX_GPU: part of the GPU name as `oclzip devices` shows it (NVIDIA, AMD, Intel, ...).
DEVICES = [("CPU", "CPU"), ("GPU", os.environ.get("MATRIX_GPU", "NVIDIA")), ("ALL", "ALL")]
BLOCKS = [int(x) for x in os.environ.get("MATRIX_BLOCKS", "1,4,16").split(",")]
# .ocpk levels: Windows XPRESS on the CPU, and the ZIP encoder's DEFLATE on GPU, GPU + CPU and CPU.
PACK_LEVELS = [("fast", "fast"), ("gpu", "gpu"), ("all", "all"), ("cpu", "deflate")]


def sources(root):
    out = []
    for s in SETS:
        out.append((f"{s}/one", os.path.join(root, s, "f01.bin")))
        out.append((f"{s}/all", os.path.join(root, s)))
    out.append(("everything", root))
    # MATRIX_EXTRA=name=path;... adds sources outside the sizes folder (e.g. s010G4/all).
    for item in filter(None, os.environ.get("MATRIX_EXTRA", "").split(";")):
        name, path = item.split("=", 1)
        out.append((name, path))
    return out


def modes():
    out = []
    for kb in CHUNKS:
        for label, dev in DEVICES:
            out.append((f"zip-{label}-{kb}k", "zip", dev, {"OCLZIP_CHUNK_KB": str(kb)}))
    for label, level in PACK_LEVELS:
        for mb in BLOCKS:
            out.append((f"ocpk-{label}-{mb}m", "pack", level, {"OCLZIP_PACK_BLOCK_MB": str(mb)}))
    return out


def size_of(path):
    if os.path.isfile(path):
        return os.path.getsize(path), 1
    total = files = 0
    for d, _, names in os.walk(path):
        for n in names:
            total += os.path.getsize(os.path.join(d, n))
            files += 1
    return total, files


def file_crc(path):
    crc = 0
    with open(path, "rb", buffering=0) as f:
        buf = bytearray(16 << 20)
        view = memoryview(buf)
        while True:
            n = f.readinto(buf)
            if not n:
                return crc
            crc = zlib.crc32(view[:n], crc)


def warm(path):
    files = [path] if os.path.isfile(path) else [os.path.join(d, n) for d, _, ns in os.walk(path) for n in ns]
    buf = bytearray(16 << 20)
    for f in files:
        with open(f, "rb", buffering=0) as h:
            while h.readinto(buf):
                pass


def check_zip(path):
    """Inflates every member (zlib releases the GIL, so members run in parallel)."""
    with zipfile.ZipFile(path) as z:
        names = [i.filename for i in z.infolist() if not i.is_dir()]

    def one(name):
        with zipfile.ZipFile(path) as z, z.open(name) as f:
            while f.read(16 << 20):  # CRC is checked at the end of the member
                pass
        return True

    with ThreadPoolExecutor(8) as pool:
        return all(pool.map(one, names))


def parse(text):
    r = {}
    m = re.search(r"Total elapsed: ([\d.]+)s", text)
    if m:
        r["tool_seconds"] = float(m.group(1))
    m = re.search(r"Limited by: (.*)", text)
    if m:
        r["limited_by"] = m.group(1).strip()
    shares = re.findall(r"^\s+(.+?)\s{2,}(\d+) batches,\s+([\d.]+)% of chunks", text, re.M)
    if shares:
        r["split"] = {name.strip(): float(p) for name, _, p in shares}
    m = re.search(r"Repeated data stored once: ([\d,]+) bytes", text)
    if m:
        r["repeated_bytes"] = int(m.group(1).replace(",", ""))
    m = re.search(r"(\d[\d,]*) blocks, (\d[\d,]*) stored", text)
    if m:
        r["blocks"] = int(m.group(1).replace(",", ""))
        r["stored_blocks"] = int(m.group(2).replace(",", ""))
    return r


def main():
    exe, root, rep = sys.argv[1], sys.argv[2], int(sys.argv[3])
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    done = set()
    if os.path.exists(RESULTS):
        for line in open(RESULTS, encoding="utf-8"):
            d = json.loads(line)
            done.add((d["rep"], d["source"], d["mode"]))
    for source, path in sources(root):
        if only and only not in source:
            continue
        src_bytes, src_files = size_of(path)
        order = modes()
        if rep >= 2:
            # Later repetitions: start each source at a different mode, and read sources that fit
            # in the file cache once first, so no mode is the only one paying for a cold read.
            shift = (rep * 7) % len(order)
            order = order[shift:] + order[:shift]
            if src_bytes < (16 << 30) and any((rep, source, m[0]) not in done for m in order):
                warm(path)
        for mode, kind, arg, env_extra in order:
            if (rep, source, mode) in done:
                continue
            ext = ".zip" if kind == "zip" else ".ocpk"
            target = os.path.join(OUT, "m_run" + ext)
            if os.path.exists(target):
                os.remove(target)
            env = dict(os.environ, **env_extra)
            cmd = [exe, "compress", path, target, "--device", arg] if kind == "zip" else \
                  [exe, "pack", path, target, "--level", arg]
            meter = Meter()
            stop = threading.Event()

            def watch():
                while not stop.wait(3.0):
                    meter.sample()

            w = threading.Thread(target=watch, daemon=True)
            w.start()
            t0 = time.perf_counter()
            p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
            wall = time.perf_counter() - t0
            stop.set()
            w.join()
            meter.sample()
            text = p.stdout + p.stderr
            rec = {"rep": rep, "source": source, "mode": mode, "exit": p.returncode, "wall": round(wall, 3),
                   "src_bytes": src_bytes, "src_files": src_files, "other_io_mb": meter.bytes >> 20,
                   "time": time.strftime("%Y-%m-%d %H:%M:%S")}
            rec.update(parse(text))
            if p.returncode == 0 and os.path.exists(target):
                rec["bytes"] = os.path.getsize(target)
                rec["saved_pct"] = round(100 * (1 - rec["bytes"] / src_bytes), 3)
                rec["mb_s"] = round(src_bytes / wall / 1e6, 1)
                if rep == 1:
                    full = src_bytes <= (16 << 30) or os.environ.get("MATRIX_FULL_VERIFY") == "1"
                    t1 = time.perf_counter()
                    if kind == "zip":
                        rec["archive_crc"] = f"{file_crc(target):08x}"
                        if arg == "CPU" and full:
                            rec["inflate_ok"] = check_zip(target)
                    else:
                        if arg != "fast":  # DEFLATE levels: GPU, ALL and CPU must give the same archive
                            rec["archive_crc"] = f"{file_crc(target):08x}"
                        if full:
                            q = subprocess.run([exe, "test", target], capture_output=True, text=True,
                                               encoding="utf-8", errors="replace")
                            rec["test_ok"] = q.returncode == 0 and "PASS" in q.stdout
                    rec["verify_seconds"] = round(time.perf_counter() - t1, 1)
            else:
                rec["error"] = text.strip().splitlines()[-1:] if text.strip() else []
            if os.path.exists(target):
                os.remove(target)
            with open(RESULTS, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
            ok = rec.get("inflate_ok", rec.get("test_ok", ""))
            print(f"{source:12} {mode:16} {wall:8.2f}s {rec.get('mb_s', 0):8.1f} MB/s "
                  f"{rec.get('saved_pct', 0):6.2f}% {rec.get('limited_by', '')[:28]:28} {ok}", flush=True)


main()
