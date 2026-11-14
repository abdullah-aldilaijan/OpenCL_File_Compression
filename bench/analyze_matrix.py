"""Summarises matrix.jsonl: python -I analyze_matrix.py [results.jsonl] [--json out.json]"""
import json, os, statistics, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(os.environ.get("OCLZIP_BENCH_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench_out")), "matrix.jsonl")
rows = [json.loads(l) for l in open(path, encoding="utf-8")]
ok = [r for r in rows if r["exit"] == 0 and "bytes" in r]
bad = [r for r in rows if r not in ok]

runs = defaultdict(list)
for r in ok:
    runs[(r["source"], r["mode"])].append(r)

SOURCES = []
for r in rows:
    if r["source"] not in SOURCES:
        SOURCES.append(r["source"])
MODES = []
for r in rows:
    if r["mode"] not in MODES:
        MODES.append(r["mode"])


def med(key, src, mode):
    v = [r[key] for r in runs.get((src, mode), []) if key in r]
    return statistics.median(v) if v else None


summary = {"sources": {}, "problems": []}
print(f"{len(rows)} runs, {len(bad)} failed")
for r in bad:
    print("  FAILED", r["rep"], r["source"], r["mode"], r.get("error"))
    summary["problems"].append(f"failed: {r['source']} {r['mode']} {r.get('error')}")

# Verification
for r in ok:
    if r.get("inflate_ok") is False or r.get("test_ok") is False:
        summary["problems"].append(f"verify failed: {r['source']} {r['mode']}")
crc = defaultdict(dict)
for r in ok:
    if "archive_crc" in r:
        kind, dev, kb = r["mode"].split("-")
        crc[(r["source"], kb)][dev] = (r["archive_crc"], r["bytes"])
mism = [(k, v) for k, v in crc.items() if len(set(v.values())) > 1]
print(f"identity groups checked: {len(crc)}, mismatched: {len(mism)}")
for k, v in mism:
    summary["problems"].append(f"CPU/GPU/ALL differ: {k} {v}")
verified = sum(1 for r in ok if r.get("inflate_ok") or r.get("test_ok"))
print(f"fully verified archives: {verified}; reps per cell: "
      f"{sorted(set(len(v) for v in runs.values()))}")
summary["verified"] = verified
summary["identity_groups"] = len(crc)
summary["identity_mismatches"] = len(mism)
summary["runs"] = len(rows)
summary["failed"] = len(bad)

for src in SOURCES:
    s = {"modes": {}}
    for mode in MODES:
        w = med("wall", src, mode)
        if w is None:
            continue
        rs = runs[(src, mode)]
        s["src_bytes"] = rs[0]["src_bytes"]
        s["src_files"] = rs[0]["src_files"]
        s["modes"][mode] = {"wall": round(w, 3), "mb_s": round(med("mb_s", src, mode), 1),
                            "saved_pct": rs[0]["saved_pct"], "n": len(rs),
                            "walls": [r["wall"] for r in rs],
                            "limited_by": rs[-1].get("limited_by", ""),
                            "split": rs[-1].get("split")}
    summary["sources"][src] = s
    best = min(s["modes"].items(), key=lambda kv: kv[1]["wall"]) if s["modes"] else None
    print(f"\n== {src}: {s.get('src_files')} files, {s.get('src_bytes', 0)/2**30:.2f} GiB"
          + (f"  fastest: {best[0]} {best[1]['wall']}s" if best else ""))
    print(f"{'chunk':>6} | " + " | ".join(f"{d:>18}" for d in ["CPU", "GPU", "ALL"]) + " | saved")
    for kb in ["1k", "2k", "4k", "8k", "16k", "32k", "64k", "128k", "256k", "512k", "1024k"]:
        cells = []
        saved = ""
        for d in ["CPU", "GPU", "ALL"]:
            m = s["modes"].get(f"zip-{d}-{kb}")
            cells.append(f"{m['wall']:7.2f}s {m['mb_s']:7.0f}MB/s" if m else " " * 18)
            if m:
                saved = f"{m['saved_pct']:.2f}%"
        print(f"{kb:>6} | " + " | ".join(cells) + f" | {saved}")
    for lvl in ["fast", "gpu", "all", "cpu"]:
        for mb in ["1m", "2m", "4m", "8m", "16m", "32m", "64m"]:
            m = s["modes"].get(f"ocpk-{lvl}-{mb}")
            if m:
                print(f"  ocpk {lvl:>4} {mb:>3}: {m['wall']:7.2f}s {m['mb_s']:7.0f} MB/s  saved {m['saved_pct']:.2f}%")

if "--json" in sys.argv:
    json.dump(summary, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
