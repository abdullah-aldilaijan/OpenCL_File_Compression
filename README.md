# OpenCL ZIP prototype (C++)

A native C++ port of the Python prototype [OpenCL_Test](https://github.com/abdullah-aldilaijan/OpenCL_Test). The original Python files are unchanged. This version needs no Python, numpy or pyopencl.

## Build

Requires Visual Studio 2022 or newer with the **Desktop development with C++** workload. Then run:

```bat
build.cmd
```

The build makes two programs:

- `build\oclzip-gui.exe` is the desktop window. It replaces `desktop_launcher.py`.
- `build\oclzip.exe` is the command-line tool. It replaces `gpu_zip.py`, `hardware.py` and the decompress side of `opencl_lz.py`, `opencl_compress.py` and `gpu_folder.py`.

Without Visual Studio, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) clang works too: put it under `.tools\` (or set `CXX`) and run `sh build-clang.sh` from Git Bash. It builds `build\oclzip.exe` and `build\oclzip-gui.exe`.

Both are statically linked, so they run on other Windows PCs without the Visual C++ runtime. OpenCL is loaded at runtime from `OpenCL.dll`, which GPU drivers install. No OpenCL SDK is needed. If no OpenCL runtime is found, CPU mode still works.

## Use

`Run.cmd` opens the window. It builds first if needed. `Run.cmd <args>` passes the arguments to `oclzip.exe`:

```bat
oclzip devices
oclzip compress "C:\path\My Folder" "C:\elsewhere\My Folder.zip" --device NVIDIA
oclzip compress file.bin file.zip --device CPU
oclzip compress "C:\path\My Folder" "C:\elsewhere\all.zip" --device ALL
oclzip decompress archive.zip "C:\path\new-folder"
oclzip decompress old.folder.oclr "C:\path\new-folder" --device Intel
oclzip decompress old.oclr restored.bin --device NVIDIA
oclzip pack "C:\path\My Folder" "C:\elsewhere\My Folder.ocpk"
oclzip pack "C:\path\My Folder" "C:\elsewhere\small.ocpk" --level max
oclzip unpack "C:\elsewhere\My Folder.ocpk" "C:\path
ew-folder"
oclzip test "C:\elsewhere\My Folder.ocpk"
```

`--device` matches part of a GPU name, or takes `CPU`. If you leave it out, the best GPU is used (a discrete GPU first, then the one with the most memory).

`oclzip devices` also explains GPUs that exist but can't be used. For example, an Intel Core CPU that normally has integrated graphics, but where Windows shows no Intel display adapter because the firmware switched it off when a discrete GPU was installed. The same notice appears in the window's status bar.

`oclzip selftest [files...]` checks the fast CPU encoder against the loop-based reference encoder, byte for byte, on generated edge cases and on the 32 KiB chunks of any files given.

`oclzip benchgpu <files...>` loads one batch of 8,192 chunks (256 MiB) from the files into memory. It then times the CPU encoder and every GPU kernel version on it, with no disk involved, and checks that all outputs are identical. `BENCH_CHUNKS` and `BENCH_REPS` change the batch size and the number of runs.

`--device ALL`, or **"All — CPU + all GPUs together"** in the window, makes the CPU and every GPU compress the same archive at the same time. It applies to compression only. Restoring an old `.oclr` with `ALL` uses the single best device.

## Solid archives (`.ocpk`)

Besides ZIP, `oclzip pack` writes a solid archive: all files are combined and compressed together. It is opened only by this tool (`unpack`, `decompress`, `test`, or "Restore archive" in the window). How it works:

- **One stream.** Every file's bytes are concatenated in folder order, then cut into 4 MiB blocks. Small files share blocks, so the compressor sees across file boundaries. A ZIP compresses each file separately, in 32 KiB pieces.
- **Stronger codec, on all CPU threads.** Blocks are compressed in parallel with the compressor built into Windows (`cabinet.dll`): XPRESS+Huffman for `--level fast` (the default) or LZMS for `--level max`. Nothing extra is installed.
- **Incompressible data is detected.** Three 32 KiB samples per block are tried first. If none shrinks by 3%, the block is stored as it is (video, JPEG, RAR and similar). A block is also stored if compressing it saves under 1%.
- **Repeated data is stored once, inside and across files.** The read threads cut every file into content-defined chunks of about 80 KiB, while they read it. A cut is made where a rolling hash of the last 64 bytes hits a fixed pattern, so the same data gives the same chunks wherever it appears. Each chunk is hashed with SHA-256. A chunk seen before is not stored again: the file's entry refers to the earlier copy. An identical file becomes a single reference; a file that shares only parts with others (a newer installer, a disk image, an archive with the same contents) refers to just those parts.
- **Index at the end.** A block table (offset, sizes, method, CRC-32 of each block) and a compressed file table (names, sizes, the stream extents each file is made of, CRC-32, times, attributes) follow the blocks, then a footer.
- **Parallel extraction.** Blocks are decompressed on all threads and written straight into files that were created at full size. Every block CRC and every file CRC is checked. Each file stays open until its last piece is written, then gets its time set and is closed. The same path checks as for ZIP apply: no `..`, no drive letters, no reserved names, no duplicate paths.
- `oclzip test archive.ocpk` decompresses everything and checks every CRC without writing any files.

Results on the 49 GB test folder (47,555 files), same drive for source and archive:

| | ZIP (CPU mode) | `.ocpk` fast |
|---|---|---|
| Archive size | 46.78 GB (10.5% saved) | **44.09 GB (15.7% saved)** |
| Repeated data | stored every time | 2.25 GB stored once (1,953 files entirely, the rest parts of files) |
| Time to create | ≈ 60 s | 60–90 s (see below) |
| Check all data (`test`) | – | 33 s |
| Extract (writes 52 GB) | 577 s (one thread) | **157–215 s** (all threads) |

Chunk-level dedup saves 0.86 GB more than the first version of the format, which only found whole identical files (44.95 GB, 14.0%). It costs no time: with and without it, creating took 88.8 s and 89.4 s in back-to-back runs. The ranges come from the drive. The QLC drive slows down for minutes after a 45 GB file is deleted and as it fills up, and single runs varied from 60 s to 297 s with no other program using it. Archives made by the first version (format 1) still open.

On a compressible 0.69 GB folder (MATLAB installer files, warm cache):

| Method | Space saved | Time |
|---|---|---|
| ZIP, CPU | 44.2% | 0.46 s |
| `.ocpk` fast (XPRESS+Huffman) | **54.7%** | 0.76 s |
| `.ocpk` max (LZMS) | **65.6%** | 23.1 s |
| `.ocpk` store (only the repeated-data saving) | 2.7% | 0.64 s |

`--level max` (LZMS, 16 MiB blocks) compresses best but runs at only about 30 MB/s on this CPU. Use it for small, compressible folders, not for 50 GB of mostly video.

**On the GPU: `--level gpu`, `all` and `deflate`.** These levels compress each block with the ZIP encoder instead of XPRESS: the block is cut into chunks (`OCLZIP_CHUNK_KB`, default 32 KiB), the chunks of many blocks are sent to the GPU in one batch, and each block is stored as one raw DEFLATE stream (method 3; archives that use it are format 3). `gpu` runs it on the best GPU, `all` on the GPUs and the CPU together, and `deflate` on the CPU only. All three write exactly the same archive. Everything else (one stream, repeated data stored once, block CRCs, parallel extraction) is unchanged. In the window, choose "OCPK DEFLATE" and the processing device. The fixed-Huffman DEFLATE is weaker than XPRESS, so these levels save less space than `fast`.

Settings: `OCLZIP_PACK_BLOCK_MB` (block size, default 4; 16 for max), `OCLZIP_PACK_THREADS` (compression threads, default all), `OCLZIP_DEDUPE=0` (don't look for repeated data), `OCLZIP_PACK_ORDER=type` (put files of the same type next to each other; it gained 0.14% on the MATLAB folder and lost 6 MB on the full folder, so it is off).

Also tried and dropped: a level that compressed every block with XPRESS+Huffman and again with LZMS where XPRESS saved over 30%. On compressible data nearly every block qualifies, so it was as slow as `max` (19.7 s against 21.6 s) for the same size.

## What runs where

| Step | Device |
|---|---|
| DEFLATE match search and Huffman bit encoding | Selected GPU (optimised `deflate_blocks` kernel; same output as `deflate.cl` in the `OpenCL Test` folder) |
| Packing compressed chunks end to end before download | Selected GPU (`compact` kernel) |
| The same encoder when **CPU** is selected | Table-driven C++ version of the kernel on a persistent pool of all CPU threads (same output bytes; `OCLZIP_CPU_ENCODER=reference` runs the original loop-based port) |
| Writing the ZIP | A separate I/O thread, unbuffered 8 MiB writes |
| File I/O, CRC-32, ZIP/ZIP64 headers | CPU |
| ZIP extraction (stored + DEFLATE, including dynamic Huffman ZIPs from Windows, 7-Zip and Python) | CPU (built-in inflater) |
| Legacy `.oclr` / `.folder.oclr` decoding | Selected device (OpenCL `lz_decode` / `rle_decode`), validated on CPU first |

**Change from the Python version:** in Python, CPU mode used zlib (dynamic Huffman, level 6). Here, CPU mode runs the same fixed-Huffman encoder as the GPU. So CPU and GPU ZIPs are byte-for-byte identical, but CPU-mode ZIPs may be somewhat larger than Python's zlib ZIPs were.

## Verification done

- ZIPs from the RTX 5060, the Iris Xe and CPU mode are byte-identical to each other and to the original `gpu_zip.py` output.
- Test data:
  - random, repeated and all-byte-value data
  - empty files and files of 1–4 bytes
  - Unicode (Arabic) names, nested and empty folders
  - 3,000 small files, and files of 235 MB and 244 MB
  - an edge-case set that hits every match length (3–258) and every distance code
- The optimised GPU kernel is checked against the CPU encoder. The original loop-based CPU encoder is kept as an independent reference (`OCLZIP_CPU_ENCODER=reference`, and `oclzip selftest`).
- Python's `zipfile`/zlib and Windows `Expand-Archive` extract the ZIPs correctly.
- `oclzip decompress` restores ZIPs made by this tool, by Python (`ZIP_DEFLATED` level 9) and by Windows `Compress-Archive`, all identical to the source.
- Legacy `OCLLZ001`, `OCLRLE01` and `.folder.oclr` archives made by the Python scripts restore correctly on NVIDIA, Intel and CPU.
- These are refused, and leave no temporary folders behind: an existing output, an archive saved inside its source folder, `../` paths, reserved Windows names (`CON` etc.), and corrupted DEFLATE data.

Files larger than 4 GiB have not been tested yet. ZIP64 headers are always written.

## How compression runs in parallel

Compression is a three-stage pipeline. Three batches circulate between the stages, so all three run at the same time:

1. **Reader thread.** It fills batches of 32 KiB chunk slots with file data. Chunks from **many files are packed into the same batch**: 8,192 chunks (256 MiB) on a discrete GPU, 1,024 (32 MiB) on an integrated GPU, 256 on the CPU. Each file still starts at a fresh chunk, so the ZIP bytes don't change.
2. **Encoder thread.** It uploads the batch and runs the kernel with **one work-item per chunk**. A second kernel then packs the compressed chunks end to end, so only the real bytes are downloaded rather than a 64 KiB slot per chunk.
3. **Main thread.** It assembles the ZIP headers and compressed chunks in order into 8 MiB blocks.
4. **I/O thread.** It writes those blocks with `FILE_FLAG_NO_BUFFERING`, so tens of GB of output don't pass through, and flush out, the Windows file cache. CRC and size patches for files that span batches are written at the end with positioned writes.

**Reading in parallel.** Every file, large or small, is split into pieces of up to 8 MiB:
- 8 read threads read and checksum pieces at the same time, up to 512 MiB ahead. The disk gets many requests at once, and the CRC work is spread over several cores.
- The reader takes the pieces strictly in archive order and only copies them into batches.
- Each file's CRC-32 is assembled from its piece CRCs with zlib's `crc32_combine` method.
- Pieces are multiples of 32 KiB, so the chunks, and therefore the ZIP bytes, are unchanged.
- Read buffers are page-aligned and recycled through a small pool. Before, every 8 MiB piece went into a new zero-filled `std::vector`: about 52 GB of needless memset and page faults for the test folder below.
- The whole ZIP's disk space is reserved up front (`FileAllocationInfo`), so NTFS allocates once instead of growing the file 8 MiB at a time. The real size is set at the end.

**Small files.** For a file that fits entirely in one batch (most files), the writer already knows the CRC and sizes. It writes the complete header straight away, instead of seeking back afterwards to patch it.

Smaller batches turned out faster than large ones. With large batches, a set of small files fits into a single batch, so the read, compress and write stages can't overlap.

**Safeguard against dropped launches.** The Intel driver once silently dropped a very long kernel launch and left stale data in the buffers. So every launch is checked:
- Before each run, the size slots are filled with a marker value.
- A slot the kernel did not overwrite shows that the launch was dropped.
- Dropped chunks are retried once with smaller launches. If that still fails, the rest of the archive is encoded on the CPU, with the same output bytes.

An archive can never be silently corrupt.

## Using all devices at once (`ALL`)

How it works:
- **One encoder thread per device.** The CPU and every usable GPU each get their own thread.
- **Shared batches.** The reader still packs files into 512-chunk batches. Each device takes the next batch as soon as it finishes its previous one, so faster devices automatically do more of the work.
- **Same output.** Batches are numbered, and the writer puts them back in order before writing. The ZIP is byte-identical to single-device mode no matter which device compressed which batch.
- **Fewer CPU threads.** The CPU encoder leaves a few threads free for reading, writing and feeding the GPUs: 16 of 20 on this machine.
- **Work split report.** At the end, each device's share of the work is shown:

```
Elapsed after setup: 0.83s. Work split:
  NVIDIA GeForce RTX 5060 Laptop GPU     19 batches,  24.9% of chunks
  Intel(R) Iris(R) Xe Graphics           12 batches,  16.4% of chunks
  CPU (16 threads)                       43 batches,  58.7% of chunks
```

Testing: 27 `ALL` runs, including 16-chunk batches that force heavy out-of-order completion. Every ZIP was byte-identical to the single-device reference, and Python's `zipfile` validated them.

## Performance on a desktop (i7-10700K, RTX 2070 SUPER, one NVMe drive)

The test was a 49 GB folder: 47,555 files, mostly media and installers, 10.5% space saved. It was read from and written to the same Crucial P1 NVMe drive. All runs produced the identical ZIP (same SHA-256 as the original build).

Times are full runs. "Round 1" is the first improvement pass (fast CPU encoder, background writer, bigger GPU batches). "Round 2" adds the v3 GPU kernel, recycled read buffers and preallocation. Round 2 CPU times are clean runs, with no other program using the drive: median 60.3 s of 59.6, 58.9, 73.4, 61.8 s.

| Device | Original build | Round 1 | **Round 2** |
|---|---|---|---|
| CPU (16 threads) | 106.1 s | 81.0 s (85.6–107.9 s in later clean runs) | **≈ 60 s** (58.9–73.4 s) |
| RTX 2070 SUPER | 243.3 s (GPU busy 226.4 s) | 171.2 s (GPU busy 159.1 s) | **104.4 s** (GPU busy 102.2 s) |
| ALL | – | 96.3 s (GPU did 19% of the chunks) | **68.3 s** (GPU did 26%) |

- **CPU mode is limited by the drive.** In round 2 the reader hardly ever waits (about 1 s), and the writer spends about 48 of the 60 s waiting for the disk. That's about 1.6 GB/s of reads and writes together on one QLC drive. Writing the ZIP to a different drive than the source is the next big win.
- **Recycled read buffers did most of round 2's CPU gain.** The old reader waited 25–50 s for data in most runs. Preallocation saves about 2 s, which is within noise. Unbuffered reads (`OCLZIP_UNBUFFERED_READ=1`) were slower (71.5 s), so they stay off.
- **The drive makes timings noisy.** After a few hundred GB of writes, the P1 drops to roughly 235 MB/s sustained (its SLC cache is full), and the same run can take 270 s whatever the encoder. Another program writing to the drive at the same time (here, a second benchmark) also doubled some run times. Compare runs only on a rested, otherwise idle drive.
- **The GPU stays slower than the CPU** on this machine (about 0.5 against 2.1 GB/s). The output must stay byte-identical, so each 32 KiB chunk is one serial work-item.
- **ALL is slower than CPU-only.** Both are disk-limited, and the CPU gives up 3 threads to feed the GPU.

## Where the time goes

Every compression run ends with a timing report for the stages. Because each stage has its own thread, the times overlap:

```
Reader: working 0.21s, waiting for disk 0.81s, waiting for free buffers 0.00s
Writer: writing 0.04s, waiting for compressed data 0.95s
Limited by: reading the source files from disk (the devices were waiting for data)
```

How to read it:
- **"Waiting for disk"** is high: the source drive is the limit. More GPUs won't help. Try faster storage, or write the ZIP to a different drive than the one you are reading from.
- **"Waiting for free buffers"** is high: the encoders, or the writer, are the limit.
- In `ALL` mode, devices that are "busy" for much less time than the elapsed time spent the rest of it waiting for data.

## GPU kernel optimisations

The optimised `deflate_blocks` produces exactly the same bits as the original kernel. It changes how they are produced:

| Original | Optimised |
|---|---|
| Zeroes the whole 64 KiB output slot first | No zeroing needed |
| Writes every single bit with a global-memory read-modify-write | Collects bits in a 64-bit register and stores 32 bits at a time |
| Loops to find Huffman, length and distance codes | Uses lookup tables (generated on the host and prepended to the kernel as `__constant` arrays) |
| 32-bit hash table, filled one entry at a time | 16-bit hash table, filled with 16-byte vector stores |
| Compares matches one byte at a time | Compares matches 4 bytes at a time (`vload4`) |

GPU busy time (upload + compress + download), same pipeline, best of 3:

| Test | Original kernel | Optimised kernel |
|---|---|---|
| 235 MB file, RTX 5060 | 0.48 s | **0.22 s** |
| 235 MB file, Iris Xe | 1.88 s | **0.65 s** |
| 3,000 small files, RTX 5060 | 0.30 s | **0.14 s** |
| 3,000 small files, Iris Xe | 1.07 s | **0.42 s** |

**Kernel v3** (the default now) adds two more changes, again with the same output bits:
- **Lookup tables in `__local` memory.** Each work-group copies them in once. In `__constant` memory, a lookup is only fast when every work-item of a warp reads the same address. `LIT[byte]` is a different address for each work-item, so those reads were serialised up to 32 ways.
- **A tag in the hash table.** Each 32-bit entry holds the position and a 16-bit tag of the 3 bytes stored there. On incompressible data, almost every lookup finds a different 3-byte string. A different tag proves the bytes differ, so the step is a literal without reading the old position from global memory. Equal tags still do the full comparison.
- The 3 bytes for the hash slide along in registers, so a literal step reads 1 new byte instead of 3.

Measured with `oclzip benchgpu <files>` on an RTX 2070 SUPER: one 256 MiB batch in memory, best of 7 runs, all outputs identical:

| Kernel | Video (incompressible) | Game archive | MATLAB files (44% smaller) |
|---|---|---|---|
| v1 (`OCLZIP_KERNEL=v1`) | 307 MB/s | 319 MB/s | 548 MB/s |
| v2 (local tables, `OCLZIP_KERNEL=v2`) | 344 MB/s | 342 MB/s | 580 MB/s |
| **v3 (local tables + tagged hash table)** | **492 MB/s** | **449 MB/s** | **663 MB/s** |
| CPU, 16 threads, for comparison | 2,073 MB/s | 2,109 MB/s | 2,773 MB/s |

Two more changes keep the same bits:
- **Early stored-block exit.** The output of a chunk only grows. Once n + 5 bytes are written, the chunk will end up as a stored block anyway, so the kernel (and the CPU encoder) stop encoding there.
- **The stored-block copy** moves 16 bytes at a time (`vload16`/`vstore16`) instead of 1.

Optional tuning through environment variables:
- `OCLZIP_BATCH_CHUNKS`: chunks per batch (default 8192 on discrete GPUs, 1024 on integrated, 256 on the CPU, 512 for `ALL`)
- `OCLZIP_LAUNCH_CHUNKS`: chunks per kernel launch (default 8192 on discrete GPUs, 512 on integrated)
- `OCLZIP_READ_THREADS`: number of parallel read threads (default 8)
- `OCLZIP_WRITE_THREADS`: number of ZIP write threads (default 1; more did not help on one NVMe drive)
- `OCLZIP_UNBUFFERED_WRITE=0`: write the ZIP through the Windows file cache instead
- `OCLZIP_CPU_ENCODER=reference`: use the original loop-based CPU encoder, for comparison
- `OCLZIP_KERNEL=original`, `v1` or `v2`: use an older kernel, for comparison
- `OCLZIP_GROUP_SIZE`: work-group size for the v2/v3 kernels (default 32)
- `OCLZIP_UNBUFFERED_READ=1`: read the source files with `FILE_FLAG_NO_BUFFERING`
- `OCLZIP_PREALLOCATE=0`: don't reserve disk space for the whole ZIP up front
- `OCLZIP_CHUNK_KB`: DEFLATE chunk size in KiB: 1, 2, 4, 8, 16, 32 (default), 64, 128, 256, 512 or 1024 (1 MiB). Every size gives the same bytes on CPU, GPU and `ALL`; 32 gives exactly the original program's output. The original kernel supports up to 32, v1 and v2 up to 64. For chunks over 64 KiB the v3 kernel keeps 20-bit positions in its hash table (with a 12-bit tag instead of 16), and the CPU encoder uses 32-bit entries; output for 64 KiB and smaller is unchanged.
- `OCLZIP_CPU_THREADS`: CPU compression threads (default all), also the CPU share in `ALL`

## File size, chunk size and device tests (i7-10700K, RTX 2070 SUPER)

These results come from the **oclzip Test Matrix**: an HTML page with every source size, chunk and block size, and device. To generate it on another machine, follow [bench/TEST_MATRIX.md](bench/TEST_MATRIX.md).

Test sets were built from 4 MiB slices of the Object folder (video, installers, documents and so on, mixed the same way in every file): 50 × 10 MiB, 20 × 50 MiB, 20 × 100 MiB, 8 × 500 MiB, 6 × 1 GiB and 3 × 10 GiB, 43.3 GiB in all. Each set was compressed as one file and as the whole folder, plus all sets together. Each of the 13 sources ran 39 options: ZIP on CPU, GPU and `ALL` at 2/4/8/16/32/64/128/256/512 KiB chunks, and `.ocpk` with 1/4/16 MiB blocks as XPRESS (`fast`) and as DEFLATE on GPU (`gpu`), `ALL` (`all`) and CPU (`deflate`). That is 507 combinations with 3 runs each (1,521 runs, none failed). The first run of each was verified: every CPU ZIP fully inflated, every `.ocpk` passed `oclzip test`, and in all 156 groups the CPU, GPU and `ALL` outputs (ZIP, and `.ocpk` DEFLATE) were byte-identical. The full results, with every option per source, are on the test matrix page built by `bench/build_report.py`.

Median speed (MB/s) at the default 32 KiB chunk:

| Source | Size | CPU | GPU | ALL | .ocpk 4 MiB | ZIP saved | .ocpk saved |
|---|---|---|---|---|---|---|---|
| 1 × 10 MiB | 10 MiB | 164 | 13 | 33 | 81 | 3.95% | 6.41% |
| 50 × 10 MiB | 0.49 GiB | 1248 | 264 | 785 | 1026 | 10.11% | 13.06% |
| 1 × 100 MiB | 0.10 GiB | 787 | 104 | 266 | 524 | 3.54% | 7.91% |
| 20 × 100 MiB | 1.95 GiB | 1546 | 399 | 1324 | 1368 | 9.79% | 12.67% |
| 8 × 500 MiB | 3.91 GiB | 1606 | 439 | 1523 | 1395 | 10.46% | 13.52% |
| 1 × 1 GiB | 1.00 GiB | 1436 | 343 | 1134 | 1245 | 9.95% | 12.43% |
| 6 × 1 GiB | 6.00 GiB | 1514 | 451 | 1579 | 1383 | 10.63% | 13.60% |
| 1 × 10 GiB | 10.0 GiB | 1022 | 461 | 1002 | 1346 | 10.77% | 13.97% |
| 3 × 10 GiB | 30.0 GiB | 908 | 474 | 931 | 949 | 10.65% | 14.80% |
| all sets | 43.3 GiB | 904 | 472 | 943 | 993 | 10.59% | 19.84% |

Chunk size on all sets together (43.3 GiB, median MB/s):

| Chunk | CPU | GPU | ALL | ZIP saved |
|---|---|---|---|---|
| 2 KiB | 869 | 464 | 848 | 9.46% |
| 4 KiB | 968 | 469 | 1015 | 9.86% |
| 8 KiB | 909 | 470 | 866 | 10.15% |
| 16 KiB | 908 | 471 | 983 | 10.42% |
| 32 KiB | 904 | 472 | 943 | 10.59% |
| 64 KiB | 892 | 443 | 911 | 10.65% |
| 128 KiB | 784 | 393 | 854 | 10.65% |
| 256 KiB | 904 | 341 | 885 | 10.65% |
| 512 KiB | 788 | 333 | 868 | 10.65% |

`.ocpk` on all sets together (4 MiB blocks): XPRESS 993 MB/s, saved 19.84%; DEFLATE on GPU 464 MB/s, `ALL` 915 MB/s, CPU 836 MB/s, all saving 18.09% with the same archive.

What the tests show:
- **CPU is the fastest single device at every size.** Below about 16 GiB the source sits in the file cache and the CPU reaches 1.3–1.6 GB/s; above that, reading and writing on the one drive hold everything to about 0.9–1.0 GB/s.
- **The GPU is slow for small jobs.** OpenCL start-up costs about 0.25 s and a 10 MiB file has too few chunks to fill the GPU, so it reaches only 8–33 MB/s there. From 1 GiB up it settles at 430–475 MB/s.
- **`ALL` only pays off on large jobs**, where it matches or slightly beats CPU (6 × 1 GiB, 3 × 10 GiB, all sets). On small jobs it pays the GPU start-up and is slower than CPU.
- **Chunk size trades space for speed, up to 64 KiB.** Going from 4 to 64 KiB saves about 0.8 points more (for example 10.03% → 10.82% on the 10 GiB file). 2 KiB is often the CPU's fastest setting on cached data but saves about 1 point less than 32 KiB. Above 64 KiB nothing more is saved, because DEFLATE only looks back 32 KiB, and the GPU gets slower with fewer, longer chunks (333 MB/s at 512 KiB, and only 7–65 MB/s on files of 100 MiB or less). 16 KiB is a good middle: within 0.2 points of 32 KiB and often faster.
- **`.ocpk` DEFLATE runs on the GPU** and writes the same archive on GPU, `ALL` and CPU. It saves 7–8 points more than ZIP on all sets, but the GPU reaches only about half the CPU's speed with it on this machine.
- **`.ocpk` XPRESS saves 3–4 points more than ZIP at about the same speed** on large data, and much more on overlapping data (19.8% on all sets, because the sets share slices). 1 and 4 MiB blocks are close; 16 MiB blocks save less (its sampling misjudges mixed blocks) and are slower on small sources.

Scaling with processing units (median MB/s of 2 runs, `units.py`; ZIPs were byte-identical for every setting):

| Setting | 1 | 2 | 4 | 8 | 12 | 16 |
|---|---|---|---|---|---|---|
| CPU threads, 10 GiB file | 254 | 493 | 896 | 1213 | 1417 | 1572 |
| `.ocpk` threads, 10 GiB file | 362 | 589 | 1131 | 1388 | – | 1392 |

- CPU speed grows almost linearly to 8 threads (the 8 cores), then hyper-threading adds a little more until writing becomes the limit.
- GPU batch size (`OCLZIP_BATCH_CHUNKS` = `OCLZIP_LAUNCH_CHUNKS`): 512 → 323 MB/s, 16384 → 504 MB/s on the 10 GiB file; on smaller sources the gain stops at 4096–8192.
- `ALL` with 1/4/8/13 CPU threads: 557/1053/1292/1270 MB/s, so the GPU adds about as much as 2 CPU threads.

## Performance

All runs used an i9-13900H with an RTX 5060 Laptop GPU and Iris Xe. Times are total seconds, including OpenCL start-up.

| Test | First C++ port (one file at a time) | Now |
|---|---|---|
| 3,000 small files (92 MB), RTX 5060 | 12.0 s | **0.55 s** |
| 3,000 small files, Iris Xe | – | **0.67 s** |
| 235 MB file, RTX 5060 | 2.2 s | **0.49 s** |
| 235 MB file, Iris Xe | – | **0.83 s** |
| 3,000 small files, CPU (20 threads) | 1.3 s | **0.18 s** |
| 235 MB file, CPU (20 threads) | 0.58 s | **0.17 s** |

`ALL` compared with single devices (best of 3, total seconds):

| Test | CPU | RTX 5060 | Iris Xe | **ALL** |
|---|---|---|---|---|
| 3,000 small files (92 MB) | 0.18 | 0.57 | 0.66 | 0.48 |
| 235 MB file | 0.16 | 0.47 | 0.84 | 0.43 |
| 1.2 GB file | 0.72 | 1.30 | 3.41 | 1.01 |

With the parallel reader (best of 2, total seconds; "cold" means the files were dropped from the Windows cache first):

| Test | CPU | RTX 5060 | **ALL** |
|---|---|---|---|
| bench folder (3,014 files, 400 MB), warm | 0.21 | 0.76 | 0.52 |
| bench folder, cold | 0.30 | 0.77 | 0.64 |
| 1.2 GB file, warm | 0.46 | 1.36 | 0.75 |
| 1.2 GB file, cold | 0.63 | 1.40 | 0.88 |

`ALL` is faster than either GPU alone, but **slower than CPU-only on this laptop**. Three reasons:
- Starting OpenCL on two GPUs adds about 0.2 s.
- The Iris Xe shares the CPU's power budget and memory, so it slows the CPU down.
- Every GPU batch has to be copied to the GPU and back, while the CPU alone already uses most of the memory bandwidth.

`ALL` should help more on a desktop with a strong discrete GPU and fewer CPU cores, or with a heavier compressor.

**The CPU is still faster on this machine.** Two reasons:
- About 0.19 s of every GPU run is OpenCL start-up: loading the GPU drivers and compiling the kernel.
- Fixed-Huffman DEFLATE with this simple match finder is very cheap work: 20 CPU threads can compress the 235 MB file in about 0.05 s. That leaves little for the GPU to win back after copying the data to it and back.

The GPU would pay off with a heavier compressor, such as dynamic Huffman or deeper match search, where the work per byte is much higher.

## Files

- `src/ocl.*` – loads OpenCL dynamically; handles devices, context, program, buffers
- `src/kernels.h` – embedded OpenCL kernels
- `src/pack.cpp` – solid `.ocpk` archives (pack, unpack, test)
- `src/archive_io.h` – shared by ZIP and `.ocpk`: folder listing, parallel read-ahead, background writer, worker pool, CRC-32 combine
- `src/zip_create.cpp` – ZIP/ZIP64 writer with GPU and CPU encoders (`gpu_zip.py`)
- `src/zip_restore.cpp` – safe ZIP extraction and the inflater (`folder_archive.restore`)
- `src/legacy.cpp` – `.oclr` decoders (`opencl_lz.py`, `opencl_compress.py`, `gpu_folder.py`)
- `src/hardware.cpp` – device discovery (`hardware.py`), plus notices for GPUs that Windows or OpenCL cannot use
- `src/cli.cpp`, `src/gui.cpp` – command line and Win32 window

## License

MIT. See [LICENSE](LICENSE). You may use, copy, modify and share this code, including in commercial projects, as long as the copyright and license notice is kept.
