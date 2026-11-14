# oclzip Test Matrix — how to generate it

"The oclzip Test Matrix" is one HTML page (title **oclzip Test Matrix**) that shows, for many source sizes,
how fast `oclzip` compresses and how much space it saves, on CPU, GPU and `ALL`, for:

- **ZIP** at chunk sizes 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 KiB and 1 MiB (`OCLZIP_CHUNK_KB`)
- **.ocpk** at block sizes 1, 2, 4, 8, 16, 32, 64 MiB (`OCLZIP_PACK_BLOCK_MB`), as XPRESS (`--level fast`, CPU only)
  and as DEFLATE on GPU (`gpu`), GPU + CPU (`all`) and CPU (`deflate`)
- **Scaling with processing units**: CPU threads, GPU batch size, CPU threads beside the GPU, .ocpk threads

The page is built from JSON results by `build_report.py` and `report_template.html`. When asked for the
oclzip Test Matrix, run the steps below on their machine and publish the page (or give the user
`bench/report.html`). **Every table cell must have a value**: run every
option on every source unless the user says otherwise.

## Rules that keep the numbers honest

- Run **one compression at a time**. Never start two test jobs together, and do not build, copy or write large files
  while tests run: the drive and GPU are shared, and timings would be wrong.
- Close other programs that use the disk or GPU heavily. If some must keep running, set `OCLZIP_BENCH_OTHERS` to a
  part of their names: `others.py` then records their disk I/O (`other_io_mb` in each result) so a slow outlier can
  be explained. Re-run an outlier cell if needed.
- Repetitions: 3 runs per cell (reps 1, 2, 3; the page shows the median). If the user wants it fast, 1 run is
  acceptable, but say so on the page.
- Rep 1 verifies every archive: CPU ZIPs are fully inflated, every .ocpk passes `oclzip test`, and each archive's
  CRC is compared across CPU, GPU and ALL (they must be byte-identical). Sources over 16 GiB skip the inflate and
  `oclzip test` steps unless `MATRIX_FULL_VERIFY=1`; their CRC comparison always runs.
- Use exact numbers from the results. Do not invent or estimate values for the page.

## 0. Settings

All scripts are Python 3 (standard library only) and run on Windows. Run them with `python -I`.

| Variable | Meaning | Default |
|---|---|---|
| `OCLZIP_BENCH_OUT` | folder for results (`*.jsonl`) and temporary archives; needs free space ≥ the largest source | `bench_out` next to `bench/` |
| `MATRIX_GPU` | part of the GPU's name as `oclzip devices` prints it (`NVIDIA`, `AMD`, `Intel`, ...) | `NVIDIA` |
| `MATRIX_CHUNKS` | ZIP chunk sizes in KiB to run | `4,8,16,32,64,128,256` — set it to the full list below |
| `MATRIX_BLOCKS` | .ocpk block sizes in MiB to run | `1,4,16` — set it to the full list below |
| `MATRIX_EXTRA` | extra sources outside the sizes folder: `name=path;name=path` | none |

Full lists: `MATRIX_CHUNKS=1,2,4,8,16,32,64,128,256,512,1024` and `MATRIX_BLOCKS=1,2,4,8,16,32,64`.

## 1. Build

```
sh build-clang.sh          # llvm-mingw clang in .tools/, or run build.cmd with MSVC
build\oclzip.exe devices   # find the GPU name for MATRIX_GPU
build\oclzip.exe selftest  # must end with "mismatches: 0"
```

## 2. Test data

Pick a source folder with varied real data (video, installers, documents...). Its size limits how much unique data
the sets can have; a set larger than the folder repeats data, which does not change ZIP results but lets .ocpk store
the repeats once (say so on the page).

```
python -I bench/make_sizes.py <source-folder> <OUT>/sizes
    # 50 x 10 MiB, 20 x 50 MiB, 20 x 100 MiB, 8 x 500 MiB, 6 x 1 GiB, 3 x 10 GiB (43.3 GiB)
python -I bench/make_4x10g.py <source-folder> <OUT>/sizes <OUT>/big/s010G4
    # 4 x 10 GiB: files 1-3 are hard links to the 3 x 10 GiB set, file 4 is new (10 GiB)
python -I bench/make_100g.py <source-folder> <OUT>/big/s100G
    # 100 GiB mix: 5 x 1 GiB, 5 x 5 GiB, 5 x 10 GiB and 1,576 files of 1-500 MiB (1,591 files)
```

If the disk is too small or the user wants it quicker, leave out the 100 GiB and 4 x 10 GiB sets (and drop them
from `MATRIX_EXTRA`). The 13 standard sources are: each size set as one file (`/one`) and as the whole folder
(`/all`), plus `everything` (the whole sizes folder).

## 3. Run the matrix

```
set MATRIX_CHUNKS=1,2,4,8,16,32,64,128,256,512,1024
set MATRIX_BLOCKS=1,2,4,8,16,32,64
set MATRIX_EXTRA=s010G4/all=<OUT>\big\s010G4;s100G/all=<OUT>\big\s100G
python -I bench/matrix.py build\oclzip.exe <OUT>\sizes 1
python -I bench/matrix.py build\oclzip.exe <OUT>\sizes 2
python -I bench/matrix.py build\oclzip.exe <OUT>\sizes 3
python -I bench/units.py  build\oclzip.exe <OUT>\sizes 1
python -I bench/units.py  build\oclzip.exe <OUT>\sizes 2
```

- Each run appends one JSON line to `<OUT>/matrix.jsonl` (or `units.jsonl`). Finished
  (rep, source, option) runs are skipped, so a stopped run can simply be started again.
- `--only <text>` runs only sources whose name contains the text (for example `--only s100G`).
- Run long jobs in the background and report progress as "done / total runs" and a time estimate.
- Rough time on an 8-core CPU + RTX 2070 SUPER with one SSD, 1 run of all 61 options: about 1 h for the 13
  standard sources, 1 h for 4 x 10 GiB and 3–4 h for the 100 GiB set (plus checking). Big sources dominate.
- `units.py` tests 1–16 CPU threads; edit the lists in `configs()` to match the machine's core count.

## 4. Build the page

```
python -I bench/analyze_matrix.py --json <scratch>/matrix_summary.json
python -I bench/build_report.py <scratch>/matrix_summary.json bench/findings.html
```

(set `PYTHONIOENCODING=utf-8` if the console cannot print × or –)

1. `analyze_matrix.py` prints every source's table plus `runs`, `failed`, `identity groups checked` and
   `mismatched`. **Failed must be 0 and mismatched must be 0**; if not, investigate before publishing.
2. Rewrite `bench/findings.html` for this machine: it is a list of `<li>` items shown under
   "What the runs show". Base every sentence on the printed numbers (fastest device per size, GPU vs CPU,
   when ALL helps, chunk size vs saving, .ocpk block size vs saving, outliers). Remove "In progress" items when done.
3. Update the "How it was tested" paragraph in `report_template.html` (sources, options, runs, machine).
4. Open `bench/report.html` and check every table for every button choice: no empty cells and no
   "not run" unless the user agreed to skip those runs.
5. Publish `bench/report.html`. When updating an existing page, publish to the same URL.

## Files

| File | Role |
|---|---|
| `make_sizes.py`, `make_4x10g.py`, `make_100g.py` | build the test sets from 4 MiB slices of a source folder |
| `matrix.py` | runs every (source, option), times it, verifies rep 1 |
| `units.py` | scaling with CPU threads, GPU batch size, .ocpk threads |
| `others.py` | measures disk I/O of other programs during a run |
| `analyze_matrix.py` | medians, verification summary, `--json` for the page |
| `build_report.py` | fills `report_template.html` with the data and findings → `report.html` |
| `findings.html` | the hand-written findings list for the page |
