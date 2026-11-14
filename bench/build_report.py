"""Builds report.html: python -I build_report.py <matrix-summary.json> <findings.html>

The summary comes from analyze_matrix.py --json; findings.html holds the <li> items for
"What the runs show". Unit-scaling medians are read from <OCLZIP_BENCH_OUT>\\units.jsonl.
"""
import json, os, statistics, sys
from collections import defaultdict

here = os.path.dirname(os.path.abspath(__file__))
S = json.load(open(sys.argv[1], encoding="utf-8"))
findings = open(sys.argv[2], encoding="utf-8").read()
names = {"s010M": "10 MiB", "s050M": "50 MiB", "s100M": "100 MiB", "s500M": "500 MiB", "s001G": "1 GiB", "s010G": "10 GiB", "s010G4": "10 GiB"}
src = []
for k, v in S["sources"].items():
    if k == "everything":
        label = "All sets together"
    elif k == "s100G/all":
        label = f"100 GiB mix ({v['src_files']:,} files)"
    else:
        a, b = k.split("/")
        label = f"1 × {names[a]}" if b == "one" else f"{v['src_files']} × {names[a]}"
    src.append({"key": k, "label": label, "bytes": v["src_bytes"], "files": v["src_files"],
                "m": {m: [x["wall"], x["mb_s"], x["saved_pct"]] for m, x in v["modes"].items()}})
# The extra 4 x 10 GiB set goes before "All sets together".
src.sort(key=lambda x: x["key"] == "everything")
rows = [json.loads(l) for l in open(os.path.join(os.environ.get("OCLZIP_BENCH_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench_out")), "units.jsonl"), encoding="utf-8")]
d = defaultdict(list)
for r in rows:
    if r["exit"] == 0:
        d[(r["source"], r["config"])].append(r["mb_s"])
units = {f"{a}|{b}": round(statistics.median(v)) for (a, b), v in d.items()}
data = {"sources": src, "units": units, "runs": S["runs"], "failed": S["failed"], "verified": S["verified"],
        "groups": S["identity_groups"], "mism": S["identity_mismatches"], "unit_runs": len(rows)}
page = open(os.path.join(here, "report_template.html"), encoding="utf-8").read()
page = page.replace("/*DATA*/null", json.dumps(data, separators=(",", ":"))).replace("<!--FINDINGS-->", findings)
open(os.path.join(here, "report.html"), "w", encoding="utf-8").write(page)
print("wrote report.html", len(page), "bytes")
