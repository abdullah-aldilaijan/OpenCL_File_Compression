// Port of gpu_zip.py: standard ZIP/ZIP64 with OpenCL fixed-Huffman DEFLATE encoding.
//
// Work runs as a three-stage pipeline so the GPU never waits on the disk:
//   reader thread  -> fills a batch with 32 KiB chunks, packing chunks from MANY files together
//   encoder thread -> runs one OpenCL dispatch per batch (one work-item per chunk, thousands at once)
//   main thread    -> writes ZIP headers + compressed chunks in order, patches CRC/sizes
// Three batches circulate, so reading, GPU encoding and writing all happen at the same time.
// Each file is still split into chunks from its own start, so the ZIP bytes are unchanged.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "archive_io.h"
#include "common.h"
#include "kernels.h"
#include "ocl.h"

namespace {

using namespace archive_io;

// Input bytes per independent DEFLATE chunk: 32 KiB as in the original program (its output is
// byte-identical), or OCLZIP_CHUNK_KB = 1, 2, 4, 8, 16, 64, 128, 256, 512 or 1024. Larger chunks restart the match
// search less often (slightly smaller ZIPs) but give a GPU fewer, longer work-items.
uint32_t chunk_size_setting() {
    uint32_t kb = env_chunks("OCLZIP_CHUNK_KB", 32);
    if (kb != 1 && kb != 2 && kb != 4 && kb != 8 && kb != 16 && kb != 64 && kb != 128 && kb != 256 && kb != 512 && kb != 1024)
        kb = 32;  // anything else: the original size
    return kb << 10;
}
const uint32_t BLOCK = chunk_size_setting();

// Bytes of a chunk sent as stored blocks: one 5-byte header per 65,535 bytes.
inline uint32_t stored_size(uint32_t n) { return n + 5 * std::max(1u, (n + 65534) / 65535); }


uint32_t store_chunk(const uint8_t* src, uint32_t n, uint8_t* out) {
    uint32_t j = 0, o = 0;
    do {
        uint32_t m = std::min<uint32_t>(n - j, 65535), inverse = (~m) & 65535;
        out[o] = 0;
        out[o + 1] = uint8_t(m);
        out[o + 2] = uint8_t(m >> 8);
        out[o + 3] = uint8_t(inverse);
        out[o + 4] = uint8_t(inverse >> 8);
        std::memcpy(out + o + 5, src + j, m);
        o += 5 + m;
        j += m;
    } while (j < n);
    return o;
}
constexpr uint32_t TABLE = 4096;
constexpr uint32_t DEFAULT_BATCH_CHUNKS = 1024;  // 32 MiB of input per GPU dispatch (smaller = better overlap)
constexpr int PIPELINE_DEPTH = 3;               // batches in flight: reading, encoding, writing

// A run of consecutive chunks in a batch that belong to one archive entry.
struct Segment {
    uint32_t entry;      // index into the entry list
    uint32_t first;      // first chunk slot in the batch
    uint32_t chunks;     // number of chunk slots (0 for folders / empty files / end marker)
    bool begins, ends;   // first / last segment of this entry
    uint64_t file_size;  // bytes read so far (final value when ends)
    uint32_t crc;        // CRC-32 so far (final value when ends)
};

struct Batch {
    std::vector<uint8_t> input, output;  // chunk i: input+i*BLOCK in, output+offsets[i] (sizes[i] bytes) out
    std::vector<uint32_t> lengths, sizes;
    std::vector<size_t> offsets;
    std::vector<Segment> segments;
    uint32_t chunks = 0;
    uint64_t seq = 0;  // order in the archive (batches can finish out of order with several devices)
    bool last = false;
    explicit Batch(uint32_t capacity)
        : input(size_t(capacity) * BLOCK), output(size_t(capacity) * BLOCK * 2), lengths(capacity), sizes(capacity),
          offsets(capacity) {}
    void slot_offsets() {  // output laid out in fixed 64 KiB slots
        for (uint32_t i = 0; i < chunks; i++) offsets[i] = size_t(i) * BLOCK * 2;
    }
    void reset() {
        chunks = 0;
        segments.clear();
        last = false;
    }
};


// Blocking hand-off between pipeline threads. close() makes every wait return false.
class Channel {
public:
    bool push(Batch* b) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            queue_.push_back(b);
        }
        ready_.notify_one();
        return true;
    }
    bool pop(Batch*& b) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || finished_ || !queue_.empty(); });
        if (closed_ || queue_.empty()) return false;
        b = queue_.front();
        queue_.pop_front();
        return true;
    }
    void close() {  // error: wake everyone, drop the queue
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }
    void finish() {  // no more batches: pop() returns false once the queue is empty
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        ready_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Batch*> queue_;
    bool closed_ = false, finished_ = false;
};

class Encoder {
public:
    virtual ~Encoder() = default;
    // Compresses b.chunks chunks: reads b.input/b.lengths, fills b.output/b.sizes.
    virtual void encode(Batch& b) = 0;
    std::string name;
    uint32_t capacity = DEFAULT_BATCH_CHUNKS;  // chunks per batch
    size_t dispatches = 0;
    double busy_seconds = 0;  // time spent inside encode() (upload + kernel + download for the GPU)
    uint64_t chunks_done = 0;
};


uint32_t deflate_chunk(const uint8_t* src, uint32_t n, uint8_t* out, int32_t* table);  // CPU version, below

uint32_t reverse_bits(uint32_t v, uint32_t n) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++, v >>= 1) r = (r << 1) | (v & 1);
    return r;
}

// Lookup tables shared by the optimised kernel and the CPU encoder.
struct DeflateTables {
    uint32_t lit[288];    // bit-reversed fixed Huffman code | (code length << 16)
    uint8_t lcode[259];   // match length 3..258 -> length code 0..28
    uint8_t dcode[512];   // distance -> distance code (zlib's two-level scheme)
    uint32_t drev[30];    // bit-reversed 5-bit distance codes
    DeflateTables() {
        static const uint32_t lb[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint32_t db[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                        33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        for (uint32_t s = 0; s < 288; s++) {
            uint32_t c, n;
            if (s <= 143) { c = 48 + s; n = 8; }
            else if (s <= 255) { c = 400 + s - 144; n = 9; }
            else if (s <= 279) { c = s - 256; n = 7; }
            else { c = 192 + s - 280; n = 8; }
            lit[s] = reverse_bits(c, n) | (n << 16);
        }
        std::memset(lcode, 0, sizeof lcode);
        for (uint32_t len = 3; len <= 258; len++) {
            uint32_t lc = 0;
            while (lc < 28 && lb[lc + 1] <= len) lc++;
            lcode[len] = uint8_t(lc);
        }
        for (uint32_t d = 1; d <= 32768; d++) {
            uint32_t dc = 0;
            while (dc < 29 && db[dc + 1] <= d) dc++;
            dcode[d <= 256 ? d - 1 : 256 + ((d - 1) >> 7)] = uint8_t(dc);
        }
        for (uint32_t dc = 0; dc < 30; dc++) drev[dc] = reverse_bits(dc, 5);
    }
};
const DeflateTables kTables;

// OCLZIP_KERNEL picks the GPU kernel: "original" (0), "v1" (1), "v2" (2) or the default v3 (3).
int kernel_version() {
    char v[32];
    DWORD len = GetEnvironmentVariableA("OCLZIP_KERNEL", v, sizeof v);
    if (len > 0 && len < sizeof v) {
        if (_stricmp(v, "original") == 0) return 0;
        if (_stricmp(v, "v1") == 0) return 1;
        if (_stricmp(v, "v2") == 0) return 2;
    }
    return 3;
}

// The optimised kernels with their lookup tables prepended as OpenCL constants.
std::string kernel_source() {
    int version = kernel_version();
    if (version == 0) return std::string(kDeflateKernel) + kCompactKernel;
    auto list = [](const char* type, const char* name, const auto* values, size_t count) {
        std::string s = std::string("__constant ") + type + " " + name + "[" + std::to_string(count) + "]={";
        for (size_t i = 0; i < count; i++) s += (i ? "," : "") + std::to_string(uint32_t(values[i]));
        return s + "};\n";
    };
    std::string tables = list("uint", "LIT", kTables.lit, 288) + list("uchar", "LCODE", kTables.lcode, 259) +
                         list("uchar", "DCODE", kTables.dcode, 512) + list("uint", "DREV", kTables.drev, 30);
    if (version == 1) return tables + kDeflateKernelFast + kCompactKernel;
    // LENC[len]: length symbol code followed by its extra bits, as one string: bits | (count << 24).
    static const uint32_t lb[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint32_t le[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    uint32_t lenc[259] = {};
    for (uint32_t len = 3; len <= 258; len++) {
        uint32_t lc = kTables.lcode[len], e = kTables.lit[257 + lc], n = e >> 16;
        lenc[len] = ((e & 0xFFFFu) | ((len - lb[lc]) << n)) | ((n + le[lc]) << 24);
    }
    return tables + list("uint", "LENC", lenc, 259) + (version == 2 ? kDeflateKernelV2 : kDeflateKernelV3) +
           kCompactKernel;
}

class GpuEncoder : public Encoder {
public:
    explicit GpuEncoder(const ocl::Device& device)
        : runtime_(device, kernel_source().c_str()), version_(kernel_version()) {
        if (version_ == 0 && BLOCK > 32768) throw Failure("The original kernel supports chunks up to 32 KiB");
        if ((version_ == 1 || version_ == 2) && BLOCK > 65536)
            throw Failure("The v1 and v2 kernels support chunks up to 64 KiB");
        // Hash table entry: 2 bytes (v1, v2), 4 bytes (original kernel; v3 with its 16-bit tag).
        table_entry_ = (version_ == 0 || version_ == 3) ? 4 : 2;
        static const char* const labels[] = {" [original kernel]", " [v1 kernel]", " [v2 kernel]", ""};
        name = device.name + labels[version_];
        group_ = env_chunks("OCLZIP_GROUP_SIZE", 32);
        // One work-item per chunk: a discrete GPU needs thousands of chunks per launch to be busy.
        // 1,024 chunks left an RTX 2070 SUPER (40 SMs) mostly idle; 8,192 cut its busy time ~28%.
        capacity = env_chunks("OCLZIP_BATCH_CHUNKS", device.unified ? DEFAULT_BATCH_CHUNKS : 8192u);
        // Host side: the pipeline keeps PIPELINE_DEPTH batches of 96 KiB per chunk; use at most 1/8 of RAM.
        MEMORYSTATUSEX memory{sizeof memory};
        if (GlobalMemoryStatusEx(&memory))
            capacity = uint32_t(std::min<uint64_t>(
                capacity, std::max<uint64_t>(DEFAULT_BATCH_CHUNKS,
                                             memory.ullTotalPhys / 8 / PIPELINE_DEPTH / (uint64_t(BLOCK) * 3))));
        // Stay within the device's single-allocation limit and a quarter of its memory.
        uint64_t per_chunk_out = uint64_t(BLOCK) * 2;
        uint64_t per_chunk_total = uint64_t(BLOCK) * 3 + uint64_t(TABLE) * table_entry_ + 8;
        if (device.max_alloc)
            capacity = uint32_t(std::min<uint64_t>(capacity, device.max_alloc / per_chunk_out));
        if (device.global_mem)
            capacity = uint32_t(std::min<uint64_t>(capacity, device.global_mem / 4 / per_chunk_total));
        capacity = std::max(capacity, 16u);
        src_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_READ_ONLY, size_t(BLOCK) * capacity);
        dst_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_WRITE_ONLY, size_t(BLOCK) * 2 * capacity);
        lengths_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_READ_ONLY, sizeof(uint32_t) * capacity);
        sizes_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_READ_WRITE, sizeof(uint32_t) * capacity);
        // Integrated GPUs get shorter launches so the driver's timeout is never reached.
        // Discrete GPUs: one launch per batch (an 8,192-chunk launch took ~0.4 s on an RTX 2070 SUPER).
        launch_ = std::min(capacity, env_chunks("OCLZIP_LAUNCH_CHUNKS", device.unified ? 512u : 8192u));
        tables_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_READ_WRITE, size_t(table_entry_) * TABLE * capacity);
        kernel_ = runtime_.kernel("deflate_blocks");
        runtime_.set_arg(kernel_, 0, *src_);
        runtime_.set_arg(kernel_, 1, *dst_);
        runtime_.set_arg(kernel_, 2, *lengths_);
        runtime_.set_arg(kernel_, 3, *sizes_);
        runtime_.set_arg(kernel_, 4, *tables_);
        runtime_.set_arg(kernel_, 5, BLOCK);
        // Packing stage: compressed chunks are moved end to end on the GPU before download.
        offsets_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_READ_ONLY, sizeof(uint32_t) * capacity);
        packed_ = std::make_unique<ocl::Buffer>(runtime_, ocl::CL_MEM_WRITE_ONLY, size_t(stored_size(BLOCK) + 16) * capacity);
        compact_ = runtime_.kernel("compact");
        runtime_.set_arg(compact_, 0, *dst_);
        runtime_.set_arg(compact_, 1, *sizes_);
        runtime_.set_arg(compact_, 2, *offsets_);
        runtime_.set_arg(compact_, 3, *packed_);
        runtime_.set_arg(compact_, 4, BLOCK * 2);
        packed_offsets_.resize(capacity);
    }

    void encode(Batch& b) override {
        uint32_t n = b.chunks;
        if (gpu_failed_) {  // the driver already lost a launch in this run; don't trust the GPU again
            encode_on_cpu(b, n, false);
            return;
        }
        runtime_.write(*src_, b.input.data(), size_t(BLOCK) * n);
        runtime_.write(*lengths_, b.lengths.data(), sizeof(uint32_t) * n);
        // Some drivers (seen on Intel Iris Xe) silently drop a long-running kernel launch and
        // leave the previous results in the buffers. So: launch in slices, pre-fill the sizes
        // with a marker the kernel always overwrites, and check every chunk afterwards.
        for (uint32_t slice = launch_, attempt = 0;; attempt++) {
            std::fill(b.sizes.begin(), b.sizes.begin() + n, kNotWritten);
            runtime_.write(*sizes_, b.sizes.data(), sizeof(uint32_t) * n);
            for (uint32_t first = 0; first < n; first += slice) {  // one work-item per chunk
                uint32_t count = std::min(slice, n - first);
                if (version_ < 2) {
                    runtime_.run(kernel_, count, first);
                } else {  // whole work-groups; work-items at or past `end` do no chunk
                    runtime_.set_arg(kernel_, 6, first + count);
                    runtime_.run(kernel_, (count + group_ - 1) / group_ * group_, first, group_);
                }
            }
            runtime_.read(*sizes_, b.sizes.data(), sizeof(uint32_t) * n);
            uint32_t missing = 0;
            for (uint32_t i = 0; i < n; i++)
                if (!valid(b.sizes[i])) missing++;
            if (!missing) break;
            if (attempt == 0 && slice > 64) {
                slice = std::max(64u, slice / 8);
                launch_ = slice;  // keep the smaller launch size for the rest of the run
                say("Warning: the GPU driver dropped " + commas(missing) + " chunks; retrying with " +
                    commas(slice) + "-chunk launches.");
                continue;
            }
            runtime_.read(*dst_, b.output.data(), size_t(BLOCK) * 2 * n);
            b.slot_offsets();
            gpu_failed_ = true;
            say("Warning: the GPU did not finish " + commas(missing) +
                " chunks; using the CPU for the rest of this archive (same output bytes).");
            encode_on_cpu(b, n, true);
            return;
        }
        // Download only the real compressed bytes: pack the chunks end to end on the GPU first.
        uint32_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            packed_offsets_[i] = total;
            b.offsets[i] = total;
            total += b.sizes[i];
        }
        runtime_.write(*offsets_, packed_offsets_.data(), sizeof(uint32_t) * n);
        runtime_.run(compact_, size_t(n) * kCompactGroup, 0, kCompactGroup);  // one work-group per chunk
        runtime_.read(*packed_, b.output.data(), total);
        dispatches++;
    }

private:
    // Encodes chunks on all CPU threads; with only_missing, keeps the chunks the GPU did finish.
    static bool valid(uint32_t size) { return size != kNotWritten && size != 0 && size <= stored_size(BLOCK); }

    void encode_on_cpu(Batch& b, uint32_t n, bool only_missing) {
        b.slot_offsets();
        std::atomic<uint32_t> next{0};
        auto work = [&] {
            std::vector<int32_t> table(TABLE);
            for (uint32_t i; (i = next.fetch_add(1)) < n;) {
                if (only_missing && valid(b.sizes[i])) continue;
                b.sizes[i] = deflate_chunk(b.input.data() + size_t(i) * BLOCK, b.lengths[i],
                                           b.output.data() + size_t(i) * BLOCK * 2, table.data());
            }
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < std::max(1u, std::thread::hardware_concurrency()); t++) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        dispatches++;
    }

    bool gpu_failed_ = false;
    static constexpr uint32_t kNotWritten = 0xFFFFFFFFu;
    uint32_t launch_ = 0;  // chunks per kernel launch
    ocl::Runtime runtime_;
    int version_;               // kernel version (see kernel_version)
    uint32_t table_entry_ = 2;  // hash table entry size in bytes
    uint32_t group_ = 32;       // work-group size for the v2/v3 kernels
    static constexpr size_t kCompactGroup = 128;
    std::unique_ptr<ocl::Buffer> src_, dst_, lengths_, sizes_, tables_, offsets_, packed_;
    ocl::cl_kernel kernel_ = nullptr, compact_ = nullptr;
    std::vector<uint32_t> packed_offsets_;
};

// ---- CPU version of deflate_blocks: same algorithm, byte-identical output ----

constexpr uint32_t LB[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint32_t LE[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint32_t DB[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint32_t DE[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

uint32_t rev(uint32_t v, uint32_t n) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

struct BitWriter {
    uint8_t* out;
    size_t bytes = 0;
    uint64_t acc = 0;
    int count = 0;
    void put(uint32_t value, uint32_t n) {
        acc |= uint64_t(value & ((1ull << n) - 1)) << count;
        count += int(n);
        while (count >= 8) {
            out[bytes++] = uint8_t(acc);
            acc >>= 8;
            count -= 8;
        }
    }
    void align() {
        if (count) {
            out[bytes++] = uint8_t(acc);
            acc = 0;
            count = 0;
        }
    }
    void symbol(uint32_t s) {
        uint32_t c, n;
        if (s <= 143) { c = 48 + s; n = 8; }
        else if (s <= 255) { c = 400 + s - 144; n = 9; }
        else if (s <= 279) { c = s - 256; n = 7; }
        else { c = 192 + s - 280; n = 8; }
        put(rev(c, n), n);
    }
};

uint32_t deflate_chunk(const uint8_t* src, uint32_t n, uint8_t* out, int32_t* table) {
    std::fill(table, table + TABLE, -1);
    BitWriter w{out};
    w.put(2, 3);  // BFINAL=0, fixed Huffman
    uint32_t i = 0;
    while (i < n) {
        uint32_t len = 0, distance = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t(src[i]) * 251u + src[i + 1]) * 251u + src[i + 2]) & 4095u;
            int32_t prev = table[h];
            table[h] = int32_t(i);
            if (prev >= 0 && i - uint32_t(prev) <= 32768) {
                uint32_t limit = std::min(258u, n - i);
                while (len < limit && src[uint32_t(prev) + len] == src[i + len]) len++;
                distance = i - uint32_t(prev);
            }
        }
        if (len >= 3) {
            uint32_t lc = 0;
            while (lc < 28 && LB[lc + 1] <= len) lc++;
            w.symbol(257 + lc);
            w.put(len - LB[lc], LE[lc]);
            uint32_t dc = 0;
            while (dc < 29 && DB[dc + 1] <= distance) dc++;
            w.put(rev(dc, 5), 5);
            w.put(distance - DB[dc], DE[dc]);
            i += len;
        } else {
            w.symbol(src[i]);
            i++;
        }
    }
    w.symbol(256);
    w.put(0, 3);  // non-final empty stored block aligns the next chunk
    w.align();
    w.put(0, 16);
    w.put(65535, 16);
    uint32_t size = uint32_t(w.bytes);
    if (size >= stored_size(n)) size = store_chunk(src, n, out);  // unhelpful chunk: uncompressed
    return size;
}

// ---- Fast CPU encoder: the same algorithm as deflate_chunk(), producing identical bytes ----
// deflate_chunk() above stays as the loop-based reference (OCLZIP_CPU_ENCODER=reference).
// This version uses the precomputed tables, a 64-bit bit buffer with 32-bit stores, 16-bit hash
// entries, 8-byte match comparison, and stops encoding as soon as the output can no longer beat
// a stored block (the result is then the stored block, exactly as the reference decides).

// Length symbol + extra bits for match lengths 3..258, as one bit string: bits | (count << 24).
struct LengthCodes {
    uint32_t code[259];
    LengthCodes() {
        for (uint32_t len = 3; len <= 258; len++) {
            uint32_t lc = kTables.lcode[len], e = kTables.lit[257 + lc];
            uint32_t bits = e & 0xFFFFu, n = e >> 16;
            bits |= (len - LB[lc]) << n;
            n += LE[lc];
            code[len] = bits | (n << 24);
        }
    }
};
const LengthCodes kLengths;

inline uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline uint32_t trailing_zeros(uint64_t v) {  // v != 0
#ifdef _MSC_VER
    unsigned long index;
    _BitScanForward64(&index, v);
    return uint32_t(index);
#else
    return uint32_t(__builtin_ctzll(v));
#endif
}

// Pos: uint16_t hash entries for chunks up to 64 KiB, uint32_t for larger chunks (all ones = empty).
template <class Pos>
uint32_t deflate_chunk_fast(const uint8_t* src, uint32_t n, uint8_t* out, Pos* table) {
    std::memset(table, 0xFF, TABLE * sizeof(Pos));
    uint64_t acc = 0;
    uint32_t cnt = 0, pos = 0;
    // Once this many whole bytes are written, the final size is too big: use stored blocks.
    const uint32_t give_up = stored_size(n);
    auto put = [&](uint32_t value, uint32_t bits) {
        acc |= uint64_t(value) << cnt;
        cnt += bits;
        if (cnt >= 32) {
            uint32_t word = uint32_t(acc);
            std::memcpy(out + pos, &word, 4);
            acc >>= 32;
            cnt -= 32;
            pos += 4;
        }
    };
    put(2, 3);  // BFINAL=0, fixed Huffman
    uint32_t i = 0;
    bool stored = false;
    while (i < n) {
        if (pos >= give_up) {
            stored = true;
            break;
        }
        uint32_t len = 0, distance = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t(src[i]) * 251u + src[i + 1]) * 251u + src[i + 2]) & 4095u;
            uint32_t prev = table[h];
            table[h] = Pos(i);
            if (prev != Pos(~0u) && i - prev <= 32768u) {  // DEFLATE window
                uint32_t d = i - prev, limit = std::min(258u, n - i);
                const uint8_t *a = src + prev, *b = src + i;
                while (len + 8 <= limit) {
                    uint64_t diff = load64(a + len) ^ load64(b + len);
                    if (diff) {
                        len += trailing_zeros(diff) >> 3;
                        goto counted;
                    }
                    len += 8;
                }
                while (len < limit && a[len] == b[len]) len++;
            counted:
                distance = d;
            }
        }
        if (len >= 3) {
            uint32_t lcode = kLengths.code[len];
            put(lcode & 0xFFFFFFu, lcode >> 24);
            uint32_t dc = distance <= 256u ? kTables.dcode[distance - 1] : kTables.dcode[256 + ((distance - 1) >> 7)];
            put(kTables.drev[dc] | ((distance - DB[dc]) << 5), 5 + DE[dc]);
            i += len;
        } else {
            uint32_t e = kTables.lit[src[i]];
            put(e & 0xFFFFu, e >> 16);
            i++;
        }
    }
    if (!stored) {
        uint32_t e = kTables.lit[256];
        put(e & 0xFFFFu, e >> 16);
        put(0, 3);  // non-final empty stored block aligns the next chunk
        cnt = (cnt + 7u) & ~7u;
        if (cnt >= 32) {
            uint32_t word = uint32_t(acc);
            std::memcpy(out + pos, &word, 4);
            acc >>= 32;
            cnt -= 32;
            pos += 4;
        }
        put(0, 16);
        put(65535, 16);
        for (uint32_t k = 0; k < cnt; k += 8) out[pos++] = uint8_t(acc >> k);
        if (pos < give_up) return pos;
    }
    return store_chunk(src, n, out);  // unhelpful chunk: uncompressed
}

bool use_reference_cpu_encoder() {
    char v[32];
    DWORD len = GetEnvironmentVariableA("OCLZIP_CPU_ENCODER", v, sizeof v);
    return len > 0 && len < sizeof v && _stricmp(v, "reference") == 0;
}


class CpuEncoder : public Encoder {
public:
    explicit CpuEncoder(unsigned threads = 0)
        // OCLZIP_CPU_THREADS caps the encoder threads (default: all logical processors).
        : threads_(std::min(threads ? threads : std::max(1u, std::thread::hardware_concurrency()),
                            env_chunks("OCLZIP_CPU_THREADS", 65536))),
          pool_(threads_),
          reference_(use_reference_cpu_encoder()) {
        name = "CPU (" + std::to_string(threads_) + " threads" + (reference_ ? ", reference encoder" : "") + ")";
        capacity = env_chunks("OCLZIP_BATCH_CHUNKS", 256);  // small batches keep the CPU pipeline overlapped
        tables_.resize(size_t(TABLE) * threads_);
    }

    void encode(Batch& b) override {
        uint32_t n = b.chunks;
        std::atomic<uint32_t> next{0};
        pool_.run([&](unsigned t) {
            int32_t* table = tables_.data() + size_t(t) * TABLE;
            for (uint32_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
                const uint8_t* in = b.input.data() + size_t(i) * BLOCK;
                uint8_t* out = b.output.data() + size_t(i) * BLOCK * 2;
                b.sizes[i] = reference_ ? deflate_chunk(in, b.lengths[i], out, table)
                         : BLOCK <= 65536 ? deflate_chunk_fast(in, b.lengths[i], out, reinterpret_cast<uint16_t*>(table))
                                          : deflate_chunk_fast(in, b.lengths[i], out, reinterpret_cast<uint32_t*>(table));
            }
        });
        b.slot_offsets();
        dispatches++;
    }

private:
    unsigned threads_;
    WorkerPool pool_;
    bool reference_;
    std::vector<int32_t> tables_;
};

// Compares the fast CPU encoder with the reference encoder on generated and real data.
// Returns the number of mismatching chunks.
uint64_t compare_encoders(const std::vector<std::vector<uint8_t>>& samples) {
    std::vector<uint8_t> a(BLOCK * 2 + 64), b(BLOCK * 2 + 64);
    std::vector<int32_t> t32(TABLE);
    std::vector<uint32_t> t16(TABLE);  // room for 16- or 32-bit entries
    uint64_t bad = 0;
    for (auto& s : samples) {
        uint32_t n = uint32_t(std::min<size_t>(s.size(), BLOCK));
        if (!n) continue;
        // The batch buffers are always a full 32 KiB slot; mimic that so 8-byte loads stay inside.
        std::vector<uint8_t> in(BLOCK, 0xCD);
        std::memcpy(in.data(), s.data(), n);
        uint32_t sa = deflate_chunk(in.data(), n, a.data(), t32.data());
        uint32_t sb = BLOCK <= 65536 ? deflate_chunk_fast(in.data(), n, b.data(), reinterpret_cast<uint16_t*>(t16.data()))
                                     : deflate_chunk_fast(in.data(), n, b.data(), t16.data());
        if (sa != sb || std::memcmp(a.data(), b.data(), sa) != 0) bad++;
    }
    return bad;
}

// "ALL": the CPU plus every usable GPU work on the same archive at once.
std::vector<std::unique_ptr<Encoder>> make_all_encoders() {
    std::vector<std::unique_ptr<Encoder>> engines;
    for (auto& d : ocl::devices(ocl::CL_DEVICE_TYPE_GPU)) {
        if (!d.available || !d.compiler) continue;
        try {
            engines.push_back(std::make_unique<GpuEncoder>(d));
        } catch (const std::exception& e) {
            say("Warning: skipping " + d.name + ": " + e.what());
        }
    }
    // Leave threads free for reading, writing and feeding each GPU.
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned reserved = 2 + unsigned(engines.size());
    engines.push_back(std::make_unique<CpuEncoder>(hw > reserved + 1 ? hw - reserved : 1));
    return engines;
}

std::unique_ptr<Encoder> make_encoder(const std::string& device) {
    if (device == "CPU") return std::make_unique<CpuEncoder>();
    std::string want = utf8(fold_case(wide(device)));
    const ocl::Device* best = nullptr;
    auto all = ocl::devices(ocl::CL_DEVICE_TYPE_GPU);
    for (auto& d : all) {
        if (!d.available || !d.compiler) continue;
        if (utf8(fold_case(wide(d.name))).find(want) == std::string::npos) continue;
        // Prefer discrete GPUs, then the one with the most memory.
        if (!best || (best->unified && !d.unified) || (best->unified == d.unified && d.global_mem > best->global_mem))
            best = &d;
    }
    if (!best) throw Failure("Selected OpenCL GPU is not available");
    return std::make_unique<GpuEncoder>(*best);
}

class ChunkCompressorImpl : public ChunkCompressor {
public:
    ChunkCompressorImpl(std::unique_ptr<Encoder> e, uint32_t min_chunks, bool gpu) : engine_(std::move(e)), gpu_(gpu) {
        if (!gpu_) engine_->capacity = std::max(engine_->capacity, min_chunks);  // CPU: no device buffers
        if (engine_->capacity < min_chunks)
            throw Failure(engine_->name + " takes " + commas(engine_->capacity) + " chunks per batch; a block needs " +
                          commas(min_chunks) + " (use smaller blocks or bigger chunks)");
        batch_ = std::make_unique<Batch>(engine_->capacity);
    }
    std::string name() const override { return engine_->name; }
    uint32_t capacity() const override { return engine_->capacity; }
    uint8_t* input() override { return batch_->input.data(); }
    uint32_t* lengths() override { return batch_->lengths.data(); }
    void run(uint32_t count) override {
        batch_->chunks = count;
        engine_->encode(*batch_);
    }
    const uint8_t* output(uint32_t k, uint32_t& size) const override {
        size = batch_->sizes[k];
        return batch_->output.data() + batch_->offsets[k];
    }
    bool is_gpu() const override { return gpu_; }

private:
    std::unique_ptr<Encoder> engine_;
    std::unique_ptr<Batch> batch_;
    bool gpu_;
};

}  // namespace

uint32_t deflate_chunk_size() { return BLOCK; }

std::vector<std::unique_ptr<ChunkCompressor>> make_chunk_compressors(const std::string& device, uint32_t min_chunks) {
    std::vector<std::unique_ptr<Encoder>> engines;
    if (device == "ALL") engines = make_all_encoders();
    else engines.push_back(make_encoder(device == "GPU" ? std::string() : device));
    std::vector<std::unique_ptr<ChunkCompressor>> out;
    for (auto& e : engines) {
        bool gpu = dynamic_cast<GpuEncoder*>(e.get()) != nullptr;
        out.push_back(std::make_unique<ChunkCompressorImpl>(std::move(e), min_chunks, gpu));
    }
    return out;
}

namespace {

void dos_time(FILETIME ft, uint16_t& clock, uint16_t& date) {
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    int year = std::clamp(int(local.wYear), 1980, 2107);
    date = uint16_t(((year - 1980) << 9) | (local.wMonth << 5) | local.wDay);
    clock = uint16_t((local.wHour << 11) | (local.wMinute << 5) | (local.wSecond / 2));
}

struct Record {
    std::string name;
    uint16_t method, clock, date;
    uint32_t crc;
    uint64_t size, compressed, offset;
    bool directory;
};


// Shared state of one compression run; the first error stops every stage.
class Pipeline {
public:
    Pipeline(uint32_t capacity, int depth) {
        for (int i = 0; i < depth; i++) {
            pool_.push_back(std::make_unique<Batch>(capacity));
            free.push(pool_.back().get());
        }
    }
    void fail(std::exception_ptr e) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = e;
        }
        free.close();
        filled.close();
        encoded.close();
    }
    void rethrow() {
        if (error_) std::rethrow_exception(error_);
    }
    Channel free, filled, encoded;

private:
    std::vector<std::unique_ptr<Batch>> pool_;
    std::mutex mutex_;
    std::exception_ptr error_;
};


// Time spent by the stages that are not encoders, to show where a run is limited.
struct StageTimes {
    double read_busy = 0;          // reader: copying file data into batches
    double read_wait_files = 0;    // reader: waiting for the read threads (disk)
    double read_wait_buffers = 0;  // reader: waiting for a free batch (devices or writer are behind)
    double write_busy = 0;         // writer: writing the ZIP
    double write_wait = 0;         // writer: waiting for the next compressed batch
    double write_disk_wait = 0;    // writer: part of write_busy spent waiting for the disk
};


// Stage 1: put every file's data into chunk slots, many files per batch.
void read_stage(Pipeline& p, const std::vector<Entry>& entries, uint32_t capacity, ReadAhead& ahead,
                StageTimes& times) {
    double begin = seconds_now();
    Batch* b = nullptr;
    uint64_t seq = 0;
    auto next_batch = [&](bool first) {
        double t = seconds_now();
        if (!first && !p.filled.push(b)) throw Cancelled{};
        if (!p.free.pop(b)) throw Cancelled{};
        b->seq = seq++;
        times.read_wait_buffers += seconds_now() - t;
    };
    next_batch(true);
    for (uint32_t index = 0; index < entries.size(); index++) {
        if (entries[index].directory) {
            b->segments.push_back({index, b->chunks, 0, true, true, 0, 0});
            continue;
        }
        uint32_t crc = 0;
        uint64_t size = 0;
        bool started = false;
        uint32_t first = ahead.first_piece(index), count = ahead.piece_count(index);
        for (uint32_t k = 0; k < count; k++) {
            double t = seconds_now();
            ReadAhead::Loaded piece = ahead.take(first + k);
            times.read_wait_files += seconds_now() - t;
            if (piece.error) std::rethrow_exception(piece.error);
            crc = k ? crc32_combine(crc, piece.crc, piece.data.size) : piece.crc;
            size_t total = piece.data.size, done = 0;
            bool last_piece = k + 1 == count;
            // A piece is 8 MiB; a chunk boundary every 32 KiB from the file start is kept because
            // pieces are multiples of 32 KiB, so copying pieces back to back gives the same chunks.
            do {
                if (b->chunks == capacity) next_batch(false);
                size_t space = size_t(capacity - b->chunks) * BLOCK, got = std::min(space, total - done);
                std::memcpy(b->input.data() + size_t(b->chunks) * BLOCK, piece.data.data() + done, got);
                done += got;
                size += got;
                bool eof = last_piece && done == total;
                uint32_t n = uint32_t((got + BLOCK - 1) / BLOCK);
                for (uint32_t j = 0; j < n; j++)
                    b->lengths[b->chunks + j] = uint32_t(std::min<size_t>(BLOCK, got - size_t(j) * BLOCK));
                if (n || eof) b->segments.push_back({index, b->chunks, n, !started, eof, size, eof ? crc : 0});
                if (n || eof) started = true;
                b->chunks += n;
            } while (done < total);
        }
    }
    b->last = true;
    double t = seconds_now();
    if (!p.filled.push(b)) throw Cancelled{};
    times.read_wait_buffers += seconds_now() - t;
    p.filled.finish();  // lets every encoder thread stop once the queue is drained
    times.read_busy = seconds_now() - begin - times.read_wait_files - times.read_wait_buffers;
}

// Stage 2: one OpenCL dispatch (or CPU thread pool run) per batch.
// With several devices, each runs this loop on its own thread and takes the next batch as
// soon as it is free, so faster devices automatically do more of the work.
void encode_stage(Pipeline& p, Encoder& engine) {
    Batch* b = nullptr;
    while (p.filled.pop(b)) {
        if (b->chunks) {
            double t0 = seconds_now();
            engine.encode(*b);
            engine.busy_seconds += seconds_now() - t0;
            engine.chunks_done += b->chunks;
        }
        if (!p.encoded.push(b)) throw Cancelled{};
    }
}

template <class Fn>
std::thread stage_thread(Pipeline& p, Fn fn) {
    return std::thread([&p, fn] {
        try {
            fn();
        } catch (const Cancelled&) {
        } catch (...) {
            p.fail(std::current_exception());
        }
    });
}

}  // namespace

// Hidden "selftest" command: fast CPU encoder vs reference encoder, plus optional real files.
int selftest_encoders(const std::vector<std::wstring>& files) {
    std::vector<std::vector<uint8_t>> samples;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    // Every length 1..300, then assorted lengths up to a full chunk, over several data shapes.
    std::vector<uint32_t> lengths;
    for (uint32_t n = 1; n <= 300; n++) lengths.push_back(n);
    for (uint32_t n : {511u, 4095u, 4096u, 8191u, 8192u, 16384u, 32763u, 32764u, 32765u, 32767u, 32768u, 32769u,
                       65530u, 65534u, 65535u, 65536u})
        if (n <= BLOCK) lengths.push_back(n);
    for (uint32_t n : {BLOCK - 5, BLOCK - 1, BLOCK}) lengths.push_back(n);
    say("Chunk size: " + std::to_string(BLOCK >> 10) + " KiB");
    for (int k = 0; k < 200; k++) lengths.push_back(1 + uint32_t(rnd() % BLOCK));
    for (uint32_t n : lengths) {
        for (int shape = 0; shape < 7; shape++) {
            std::vector<uint8_t> s(n);
            uint32_t period = 1 + uint32_t(rnd() % 300);
            for (uint32_t i = 0; i < n; i++) {
                switch (shape) {
                    case 0: s[i] = uint8_t(rnd()); break;                              // incompressible
                    case 1: s[i] = 0; break;                                           // all zero
                    case 2: s[i] = uint8_t(i % period); break;                         // repeating
                    case 3: s[i] = uint8_t("abcdefghij klmno"[rnd() % 16]); break;     // text-like
                    case 4: s[i] = uint8_t(i < period ? rnd() : s[i - period] ^ (rnd() % 97 == 0)); break;
                    case 5: s[i] = uint8_t(rnd() % 4); break;                          // low entropy
                    default: s[i] = uint8_t(i * 7 + (rnd() % 1000 == 0 ? 3 : 0)); break;
                }
            }
            samples.push_back(std::move(s));
        }
    }
    uint64_t generated = samples.size();
    uint64_t bad = compare_encoders(samples);
    say("Generated chunks: " + commas(generated) + ", mismatches: " + commas(bad));
    uint64_t real = 0, real_bad = 0;
    for (auto& path : files) {
        File f = File::open_read(absolute_path(path));
        std::vector<std::vector<uint8_t>> chunks;
        for (;;) {
            std::vector<uint8_t> c(BLOCK);
            size_t got = f.read(c.data(), BLOCK);
            if (!got) break;
            c.resize(got);
            chunks.push_back(std::move(c));
            if (chunks.size() == 512) {
                real += chunks.size();
                real_bad += compare_encoders(chunks);
                chunks.clear();
            }
        }
        real += chunks.size();
        real_bad += compare_encoders(chunks);
    }
    if (!files.empty()) say("File chunks: " + commas(real) + ", mismatches: " + commas(real_bad));
    return (bad || real_bad) ? 1 : 0;
}

// Hidden "benchgpu" command: the same in-memory batch through each encoder, repeatedly, so kernel
// versions can be compared without disk or pipeline noise. Chunks come from the files given.
int bench_encoders(const std::vector<std::wstring>& files, uint32_t chunks, int reps) {
    Batch b(chunks);
    for (auto& path : files) {
        File f = File::open_read(absolute_path(path));
        while (b.chunks < chunks) {
            size_t got = f.read(b.input.data() + size_t(b.chunks) * BLOCK, BLOCK);
            if (!got) break;
            b.lengths[b.chunks++] = uint32_t(got);
        }
        if (b.chunks == chunks) break;
    }
    say("Batch: " + commas(b.chunks) + " chunks (" + commas(uint64_t(b.chunks) * BLOCK >> 20) + " MiB)");
    struct Variant {
        const char* label;
        const char* kernel;  // OCLZIP_KERNEL value, or nullptr for the CPU encoder
        const char* group;
    };
    std::vector<Variant> variants = {{"CPU fast encoder", nullptr, nullptr},
                                     {"GPU v1 kernel", "v1", nullptr},
                                     {"GPU v2 kernel, group 32", "v2", "32"},
                                     {"GPU v3 kernel, group 32", "v3", "32"},
                                     {"GPU v3 kernel, group 64", "v3", "64"}};
    std::vector<ocl::Device> gpus;
    try {
        for (auto& d : ocl::devices(ocl::CL_DEVICE_TYPE_GPU))
            if (d.available && d.compiler && !d.unified) gpus.push_back(d);
        if (gpus.empty())
            for (auto& d : ocl::devices(ocl::CL_DEVICE_TYPE_GPU))
                if (d.available && d.compiler) gpus.push_back(d);
    } catch (const std::exception&) {
    }
    std::vector<std::unique_ptr<Encoder>> engines;
    for (auto& v : variants) {
        if (!v.kernel) {
            engines.push_back(std::make_unique<CpuEncoder>());
            continue;
        }
        if (gpus.empty()) {
            engines.push_back(nullptr);
            continue;
        }
        SetEnvironmentVariableA("OCLZIP_KERNEL", v.kernel);
        SetEnvironmentVariableA("OCLZIP_GROUP_SIZE", v.group);
        SetEnvironmentVariableA("OCLZIP_BATCH_CHUNKS", std::to_string(chunks).c_str());
        SetEnvironmentVariableA("OCLZIP_LAUNCH_CHUNKS", std::to_string(chunks).c_str());
        try {
            engines.push_back(std::make_unique<GpuEncoder>(gpus[0]));
        } catch (const Failure& e) {  // e.g. an older kernel and a chunk size it does not support
            say(std::string(v.label) + ": skipped (" + e.what() + ")");
            engines.push_back(nullptr);
        }
    }
    std::vector<std::vector<double>> times(variants.size());
    std::vector<uint64_t> output_hash(variants.size());
    for (int r = 0; r < reps + 1; r++) {  // round 0 warms up
        for (size_t k = 0; k < variants.size(); k++) {
            if (!engines[k]) continue;
            double t = seconds_now();
            engines[k]->encode(b);
            double dt = seconds_now() - t;
            if (r) times[k].push_back(dt);
            if (r != reps) continue;
            uint64_t h = 1469598103934665603ull;  // FNV-1a over the compressed chunks
            for (uint32_t i = 0; i < b.chunks; i++)
                for (uint32_t j = 0; j < b.sizes[i]; j++) h = (h ^ b.output[b.offsets[i] + j]) * 1099511628211ull;
            output_hash[k] = h;
        }
    }
    bool same = true;
    for (size_t k = 0; k < variants.size(); k++) {
        if (!engines[k]) continue;
        auto t = times[k];
        std::sort(t.begin(), t.end());
        double mb = double(b.chunks) * BLOCK / 1e6;
        char line[200];
        std::snprintf(line, sizeof line, "%-28s best %.3fs (%5.0f MB/s)  median %.3fs  output %016llx", variants[k].label,
                      t.front(), mb / t.front(), t[t.size() / 2], (unsigned long long)output_hash[k]);
        say(line);
        if (output_hash[k] != output_hash[0]) same = false;
    }
    say(same ? "All outputs identical." : "OUTPUTS DIFFER.");
    return same ? 0 : 1;
}

void create_zip(const std::wstring& source_in, const std::wstring& target_in, const std::string& device) {
    std::wstring source = absolute_path(source_in), target = absolute_path(target_in);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(long_path(source).c_str(), GetFileExInfoStandard, &info))
        throw Failure("Source not found: " + utf8(source));
    bool source_is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (exists(target)) throw Failure("Choose a new archive name");
    if (source_is_dir && is_within(target, source)) throw Failure("Save the archive outside the source folder");

    bool all = device == "ALL", cpu = device == "CPU";
    std::vector<std::unique_ptr<Encoder>> engines;
    if (all) engines = make_all_encoders();
    else engines.push_back(make_encoder(device));
    uint32_t capacity = engines[0]->capacity;
    if (all) {
        // Shared batch size: small enough that every device gets work, within each GPU's buffers.
        capacity = env_chunks("OCLZIP_BATCH_CHUNKS", 512);
        for (auto& e : engines)
            if (e->name.rfind("CPU", 0) != 0) capacity = std::min(capacity, e->capacity);
        std::string names;
        for (auto& e : engines) names += (names.empty() ? "" : " + ") + e->name;
        say("Standard ZIP / ALL DEVICES: " + names);
        say("Every device takes the next batch as soon as it is free; batches are written back in order.");
    } else if (cpu) {
        say("Standard ZIP / CPU DEFLATE: " + engines[0]->name);
        say("CPU: same fixed-Huffman encoder as the GPU kernel, plus file I/O, CRC and ZIP headers.");
    } else {
        say("Standard ZIP / GPU DEFLATE: " + engines[0]->name);
        say("GPU: match search and Huffman encoding. CPU: file I/O, CRC and ZIP headers.");
    }
    say("Batches of up to " + commas(capacity) + " chunks (" + commas(capacity / 32) +
        " MiB) packed across files; reading, encoding and writing run in parallel.");

    check_link(source, info.dwFileAttributes);
    std::vector<Entry> entries;
    std::string root = utf8(name_of(source));
    entries.push_back({source, root + (source_is_dir ? "/" : ""), source_is_dir, info.ftLastWriteTime,
                       (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow});
    if (source_is_dir) walk(source, root + "/", entries);

    std::vector<Record> records;
    StageTimes times;
    uint64_t total = 0, count = 0;
    double start = seconds_now();
    {
        TempDir temp(parent_of(target), L".gpu-zip-");
        std::wstring staged = join(temp.path(), L"archive.zip");
        {
            ZipWriter out(staged);
            if (env_flag("OCLZIP_PREALLOCATE", true)) {
                // Upper bound: every chunk stored (5 bytes per 32 KiB) plus headers and directory.
                uint64_t estimate = 0;
                for (auto& e : entries)
                    estimate += e.size + (e.size / BLOCK + 2) * (stored_size(BLOCK) - BLOCK) + 2 * (e.name.size() + 100);
                out.reserve(estimate);
            }
            // One batch being read, one being written, and up to two queued per encoder.
            int depth = engines.size() == 1 ? PIPELINE_DEPTH : 2 + 2 * int(engines.size());
            Pipeline pipeline(capacity, depth);
            ReadAhead ahead(entries, env_chunks("OCLZIP_READ_THREADS", 8));
            std::thread reader =
                stage_thread(pipeline, [&] { read_stage(pipeline, entries, capacity, ahead, times); });
            std::vector<std::thread> encoders;
            for (auto& e : engines) {
                Encoder* engine = e.get();
                encoders.push_back(stage_thread(pipeline, [&pipeline, engine] { encode_stage(pipeline, *engine); }));
            }

            // Stage 3 (this thread): write entries in order as their compressed chunks arrive.
            try {
                uint64_t offset = 0, payload_start = 0;
                uint16_t clock = 0, date = 0;
                std::map<uint64_t, Batch*> waiting;  // finished early, waiting for their turn
                uint64_t next = 0;
                double write_begin = seconds_now();
                for (bool last = false; !last;) {
                    Batch* b = nullptr;
                    auto ready = waiting.find(next);
                    if (ready != waiting.end()) {
                        b = ready->second;
                        waiting.erase(ready);
                    } else {
                        double t = seconds_now();
                        bool got = pipeline.encoded.pop(b);
                        times.write_wait += seconds_now() - t;
                        if (!got) break;
                        if (b->seq != next) {
                            waiting[b->seq] = b;
                            continue;
                        }
                    }
                    next++;
                    for (const Segment& s : b->segments) {
                        const Entry& e = entries[s.entry];
                        // A whole entry inside this batch (most files): its CRC and sizes are already
                        // known, so the header is written complete and needs no seek back later.
                        bool whole = s.begins && s.ends;
                        uint64_t whole_compressed = 0;
                        if (whole) {
                            for (uint32_t i = s.first; i < s.first + s.chunks; i++) whole_compressed += b->sizes[i];
                            if (!e.directory) whole_compressed += 5;
                        }
                        if (s.begins) {
                            if (e.name.size() > 65535) throw Failure("Archive path is too long");
                            dos_time(e.modified, clock, date);
                            offset = out.tell();
                            std::vector<uint8_t> h;
                            put32(h, 0x04034b50); put16(h, 45); put16(h, 0x800); put16(h, e.directory ? 0 : 8);
                            put16(h, clock); put16(h, date); put32(h, whole ? s.crc : 0); put32(h, 0xffffffff);
                            put32(h, 0xffffffff); put16(h, uint32_t(e.name.size())); put16(h, 20);
                            h.insert(h.end(), e.name.begin(), e.name.end());
                            put16(h, 1); put16(h, 16);  // ZIP64 extra; patched below for multi-batch files
                            put64(h, whole ? s.file_size : 0); put64(h, whole_compressed);
                            out.write(h.data(), h.size());
                            payload_start = out.tell();
                        }
                        for (uint32_t i = s.first; i < s.first + s.chunks; i++)
                            out.write(b->output.data() + b->offsets[i], b->sizes[i]);
                        if (!s.ends) continue;
                        if (!e.directory) {
                            // Final byte-aligned empty stored block terminates this raw DEFLATE stream.
                            static const uint8_t final_block[5] = {1, 0, 0, 0xff, 0xff};
                            out.write(final_block, 5);
                            count++;
                        }
                        uint64_t end = out.tell(), compressed = end - payload_start;
                        if (whole && compressed != whole_compressed) throw Failure("Internal size mismatch");
                        if (!whole) {
                            std::vector<uint8_t> patch;
                            put32(patch, s.crc);
                            out.patch(offset + 14, patch);
                            patch.clear();
                            put64(patch, s.file_size);
                            put64(patch, compressed);
                            out.patch(offset + 30 + e.name.size() + 4, patch);
                        }
                        records.push_back({e.name, uint16_t(e.directory ? 0 : 8), clock, date, s.crc, s.file_size,
                                           compressed, offset, e.directory});
                        total += s.file_size;
                        say("Packed: " + utf8(name_of(e.path)) + " (" + commas(s.file_size) + " bytes)");
                    }
                    last = b->last;
                    b->reset();
                    if (!last && !pipeline.free.push(b)) break;
                }
                times.write_busy = seconds_now() - write_begin - times.write_wait;
                times.write_disk_wait = out.wait_seconds;
            } catch (...) {
                pipeline.fail(std::current_exception());
            }
            reader.join();
            for (auto& t : encoders) t.join();
            pipeline.rethrow();
            if (records.size() != entries.size()) throw Failure("Compression stopped before all files were written");

            uint64_t central_offset = out.tell();
            for (auto& r : records) {
                std::vector<uint8_t> c;
                uint32_t attr = ((r.directory ? 040755u : 0100644u) << 16) | (r.directory ? 16u : 0u);
                put32(c, 0x02014b50); put16(c, 0x032d); put16(c, 45); put16(c, 0x800); put16(c, r.method);
                put16(c, r.clock); put16(c, r.date); put32(c, r.crc); put32(c, 0xffffffff); put32(c, 0xffffffff);
                put16(c, uint32_t(r.name.size())); put16(c, 28); put16(c, 0); put16(c, 0); put16(c, 0);
                put32(c, attr); put32(c, 0xffffffff);
                c.insert(c.end(), r.name.begin(), r.name.end());
                put16(c, 1); put16(c, 24); put64(c, r.size); put64(c, r.compressed); put64(c, r.offset);
                out.write(c.data(), c.size());
            }
            uint64_t end64 = out.tell(), central_size = end64 - central_offset, n = records.size();
            std::vector<uint8_t> t;
            put32(t, 0x06064b50); put64(t, 44); put16(t, 45); put16(t, 45); put32(t, 0); put32(t, 0);
            put64(t, n); put64(t, n); put64(t, central_size); put64(t, central_offset);
            put32(t, 0x07064b50); put32(t, 0); put64(t, end64); put32(t, 1);
            put32(t, 0x06054b50); put16(t, 0); put16(t, 0); put16(t, 0xffff); put16(t, 0xffff);
            put32(t, 0xffffffff); put32(t, 0xffffffff); put16(t, 0);
            out.write(t.data(), t.size());
            out.close();
        }
        move_new(staged, target);
    }
    uint64_t size = File::open_read(target).size();
    say(commas(count) + " files; " + commas(total) + " source bytes -> " + commas(size) + " ZIP bytes");
    if (size >= total)
        say("NO SPACE SAVED: ZIP is " + commas(size - total) + " bytes larger.");
    else
        say("Space saved: " + percent(double(total - size) / double(total)));
    char elapsed[64];
    double wall = seconds_now() - start;
    std::snprintf(elapsed, sizeof elapsed, "%.3fs", wall);
    if (all) {
        say(std::string("Elapsed after setup: ") + elapsed + ". Work split:");
        uint64_t chunks = 0;
        for (auto& e : engines) chunks += e->chunks_done;
        for (auto& e : engines) {
            char line[256];
            std::snprintf(line, sizeof line, "  %-45s %5zu batches, %5.1f%% of chunks, busy %.3fs", e->name.c_str(),
                          e->dispatches, chunks ? 100.0 * double(e->chunks_done) / double(chunks) : 0.0,
                          e->busy_seconds);
            say(line);
        }
    } else {
        Encoder* engine = engines[0].get();
        say(std::string(cpu ? "CPU encoder batches: " : "OpenCL kernel batches: ") +
            std::to_string(engine->dispatches) + "; elapsed after " + (cpu ? "setup" : "GPU setup") + ": " + elapsed);
        char busy[128];
        std::snprintf(busy, sizeof busy, "%s busy (upload + compress + download): %.3fs of %.3fs",
                      cpu ? "Encoder" : "GPU", engine->busy_seconds, wall);
        say(busy);
    }
    // Where did the time go? Each stage runs on its own thread, so these overlap.
    char line[256];
    std::snprintf(line, sizeof line,
                  "Reader: working %.3fs, waiting for disk %.3fs, waiting for free buffers %.3fs",
                  times.read_busy, times.read_wait_files, times.read_wait_buffers);
    say(line);
    std::snprintf(line, sizeof line,
                  "Writer: writing %.3fs (of which waiting for the disk %.3fs), waiting for compressed data %.3fs",
                  times.write_busy, times.write_disk_wait, times.write_wait);
    say(line);
    const char* limit;
    if (times.read_wait_buffers > 0.4 * wall)
        limit = times.write_busy > 0.5 * wall ? "writing the ZIP to disk" : "compression (the devices are the bottleneck)";
    else if (times.read_wait_files > 0.3 * wall)
        limit = "reading the source files from disk (the devices were waiting for data)";
    else if (times.read_busy > 0.5 * wall)
        limit = "the reader thread (copying data into batches); the devices were waiting for data";
    else
        limit = "no single stage (well balanced)";
    say(std::string("Limited by: ") + limit);
}
