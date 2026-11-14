// Solid ".ocpk" archives: every file is combined into one stream, which is cut into blocks and
// compressed in parallel with the Windows Compression API (cabinet.dll: XPRESS+Huffman or LZMS), or
// with the ZIP encoder's DEFLATE on the GPU, the CPU or both (levels gpu, deflate, all).
// Identical files are stored once. Blocks that do not shrink (video, JPEG, RAR...) are stored.
//
// Layout (little-endian):
//   header   "OCPACK01", u32 version, u32 block size, u64 reserved x2              (32 bytes)
//   blocks   payloads back to back
//   blocks table   per block: u64 offset, u32 raw size, u32 packed size, u32 CRC-32 of the raw
//                  bytes, u8 method, 3 bytes zero                                    (24 bytes each)
//   file table     one blob, compressed like a block; per entry: u8 kind (0 file, 1 folder),
//                  u16 name length, UTF-8 name, u64 size, u64 stream offset, u32 CRC-32,
//                  u64 modified time (FILETIME), u32 attributes
//   footer   u64 blocks-table offset, u64 block count, u64 file-table offset, u32 file-table packed
//            size, u32 file-table raw size, u32 file-table CRC, u8 file-table method, 3 zero bytes,
//            "OCPKEND1"                                                              (48 bytes)
#include <atomic>
#include <map>
#include <set>
#include <unordered_map>

#include "archive_io.h"
#include "common.h"

#include <compressapi.h>  // after <windows.h> (via common.h)

using namespace archive_io;

namespace {

constexpr char kMagic[8] = {'O', 'C', 'P', 'A', 'C', 'K', '0', '1'};
constexpr char kEndMagic[8] = {'O', 'C', 'P', 'K', 'E', 'N', 'D', '1'};
// 2: files may be made of several stream extents. 3: blocks may be raw DEFLATE (written only then).
constexpr uint32_t kVersion = 3;
constexpr size_t kHeaderSize = 32, kFooterSize = 48, kBlockRecord = 24;

enum Method : uint8_t { kStored = 0, kXpressHuff = 1, kLzms = 2, kDeflate = 3 };

const char* method_name(uint8_t m) {
    return m == kStored ? "stored" : m == kXpressHuff ? "XPRESS+Huffman" : m == kLzms ? "LZMS" : "DEFLATE";
}

// Raw DEFLATE decoder for the blocks the ZIP encoder writes: stored and fixed-Huffman blocks only.
// Every length and distance is checked against the output, so damaged input fails cleanly.
void inflate_block(const uint8_t* in, size_t n, uint8_t* out, size_t raw) {
    struct Tables {
        uint16_t lit[512];  // 9 bits of input -> symbol << 4 | code length
        uint8_t dist[32];
        Tables() {
            auto rev = [](uint32_t v, int bits) {
                uint32_t r = 0;
                for (int i = 0; i < bits; i++) r |= ((v >> i) & 1) << (bits - 1 - i);
                return r;
            };
            for (uint32_t s = 0; s < 288; s++) {
                uint32_t code, len;
                if (s < 144) code = 0x30 + s, len = 8;
                else if (s < 256) code = 0x190 + (s - 144), len = 9;
                else if (s < 280) code = s - 256, len = 7;
                else code = 0xC0 + (s - 280), len = 8;
                for (uint32_t k = rev(code, int(len)); k < 512; k += 1u << len) lit[k] = uint16_t(s << 4 | len);
            }
            for (uint32_t d = 0; d < 32; d++) dist[rev(d, 5)] = uint8_t(d);
        }
    };
    static const Tables t;
    static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                               33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    uint64_t buf = 0;
    int count = 0;
    size_t at = 0, o = 0;
    auto refill = [&] {
        while (count <= 56 && at < n) {
            buf |= uint64_t(in[at++]) << count;
            count += 8;
        }
    };
    auto take = [&](int bits) {
        if (count < bits) {
            refill();
            if (count < bits) throw Failure("Corrupt DEFLATE block (truncated)");
        }
        uint32_t v = uint32_t(buf & ((1ull << bits) - 1));
        buf >>= bits;
        count -= bits;
        return v;
    };
    for (bool last = false; !last;) {
        last = take(1) != 0;
        uint32_t type = take(2);
        if (type == 0) {
            take(count & 7);  // to a byte boundary
            uint32_t len = take(16), nlen = take(16);
            if (len != (~nlen & 0xFFFFu) || len > raw - o) throw Failure("Corrupt DEFLATE stored block");
            while (len && count) {  // bytes still in the bit buffer
                out[o++] = uint8_t(take(8));
                len--;
            }
            if (len > n - at) throw Failure("Corrupt DEFLATE block (truncated)");
            std::memcpy(out + o, in + at, len);
            o += len;
            at += len;
        } else if (type == 1) {
            for (;;) {
                if (count < 48) refill();
                uint16_t e = t.lit[buf & 511];
                if (count < int(e & 15)) throw Failure("Corrupt DEFLATE block (truncated)");
                buf >>= e & 15;
                count -= e & 15;
                uint32_t s = e >> 4;
                if (s < 256) {
                    if (o >= raw) throw Failure("Corrupt DEFLATE block (too long)");
                    out[o++] = uint8_t(s);
                } else if (s == 256) {
                    break;
                } else {
                    s -= 257;
                    if (s >= 29) throw Failure("Corrupt DEFLATE block (length code)");
                    uint32_t len = kLenBase[s] + take(kLenExtra[s]);
                    uint32_t d = t.dist[take(5)];
                    if (d >= 30) throw Failure("Corrupt DEFLATE block (distance code)");
                    uint32_t distance = kDistBase[d] + take(kDistExtra[d]);
                    if (distance > o || len > raw - o) throw Failure("Corrupt DEFLATE block (distance)");
                    uint8_t* to = out + o;
                    const uint8_t* from = to - distance;
                    if (distance >= len) std::memcpy(to, from, len);
                    else
                        for (uint32_t k = 0; k < len; k++) to[k] = from[k];
                    o += len;
                }
            }
        } else {
            throw Failure("Corrupt DEFLATE block (unsupported block type)");
        }
    }
    if (o != raw) throw Failure("Corrupt DEFLATE block (size)");
}

DWORD algorithm_flags(uint8_t method) {
    // Raw mode (no per-buffer header) for XPRESS; LZMS uses its normal buffer format.
    return method == kXpressHuff ? (COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW) : COMPRESS_ALGORITHM_LZMS;
}

class Codec {
public:
    explicit Codec(uint8_t method) : method_(method) {
        if (method == kStored) return;
        if (!CreateCompressor(algorithm_flags(method), nullptr, &compressor_))
            throw Failure("Cannot start the " + std::string(method_name(method)) + " compressor: " +
                          win_error(GetLastError()));
    }
    ~Codec() {
        if (compressor_) CloseCompressor(compressor_);
    }
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    // Returns the packed size, or 0 if the result would not be smaller than `limit`.
    size_t compress(const uint8_t* in, size_t n, uint8_t* out, size_t limit) {
        if (method_ == kStored) return 0;
        SIZE_T packed = 0;
        if (!Compress(compressor_, const_cast<uint8_t*>(in), n, out, limit, &packed)) return 0;  // did not fit: incompressible
        return packed < limit ? packed : 0;
    }

private:
    uint8_t method_;
    COMPRESSOR_HANDLE compressor_ = nullptr;
};

class Decoder {
public:
    explicit Decoder(uint8_t method) {
        if (!CreateDecompressor(algorithm_flags(method), nullptr, &handle_))
            throw Failure("Cannot start the " + std::string(method_name(method)) + " decompressor");
    }
    ~Decoder() {
        if (handle_) CloseDecompressor(handle_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void decompress(const uint8_t* in, size_t n, uint8_t* out, size_t raw) {
        SIZE_T got = 0;
        if (!Decompress(handle_, const_cast<uint8_t*>(in), n, out, raw, &got) || got != raw) throw Failure("Corrupt compressed block");
    }

private:
    DECOMPRESSOR_HANDLE handle_ = nullptr;
};

// Blocking FIFO between threads; close() wakes everyone.
template <class T>
class Queue {
public:
    bool push(T v) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (closed_) return false;
            q_.push_back(std::move(v));
        }
        cv_.notify_one();
        return true;
    }
    // Takes the front item without waiting, only if there is one and ok(item) is true.
    template <class Ok>
    bool try_pop_if(T& v, Ok ok) {
        std::lock_guard<std::mutex> lock(m_);
        if (closed_ || q_.empty() || !ok(q_.front())) return false;
        v = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    bool pop(T& v) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return closed_ || done_ || !q_.empty(); });
        if (closed_ || q_.empty()) return false;
        v = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(m_);
            done_ = true;
        }
        cv_.notify_all();
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
    bool closed_ = false, done_ = false;
};

struct PackBlock {
    uint64_t seq = 0;
    std::vector<uint8_t> raw, packed;
    size_t raw_size = 0, packed_size = 0;
    uint8_t method = kStored;
    uint32_t crc = 0;
};

struct BlockRecord {
    uint64_t offset;
    uint32_t raw, packed, crc;
    uint8_t method;
};

struct PackEntry {
    std::string name;
    bool directory = false;
    uint64_t size = 0, offset = 0;
    uint32_t crc = 0;
    FILETIME modified{};
    DWORD attributes = 0;
    // Where the file's bytes are in the stream (offset, length), in file order. Empty: one extent
    // at `offset`. Several when parts of the file repeat data stored earlier.
    std::vector<std::pair<uint64_t, uint64_t>> extents;
};

// True when three 32 KiB samples of the block all fail to shrink by at least 3%: the block is
// stored without compressing the rest (already-compressed data such as video, JPEG or RAR).
bool looks_incompressible(Codec& codec, const uint8_t* data, size_t n, std::vector<uint8_t>& scratch) {
    constexpr size_t kSample = 32 << 10;
    if (n < 4 * kSample) return false;
    scratch.resize(kSample);
    for (size_t at : {size_t(0), n / 2 - kSample / 2, n - kSample}) {
        size_t packed = codec.compress(data + at, kSample, scratch.data(), kSample * 97 / 100);
        if (packed) return false;
    }
    return true;
}

// Repeated data: chunks of at least this size that occurred before are stored as references.
constexpr uint32_t kMinReference = 512;

struct ShaKey {
    size_t operator()(const std::array<uint8_t, 32>& h) const {
        size_t v;
        std::memcpy(&v, h.data(), sizeof v);
        return v;
    }
};

// Returns the block method; device is set for the DEFLATE levels ("GPU", "CPU" or "ALL").
uint8_t parse_level(const std::string& level, std::string& device) {
    if (level.empty() || level == "fast") return kXpressHuff;
    if (level == "max") return kLzms;
    if (level == "store") return kStored;
    if (level == "gpu") device = "GPU";
    else if (level == "all") device = "ALL";
    else if (level == "deflate") device = "CPU";
    else if (level.rfind("deflate:", 0) == 0) device = level.substr(8);  // "CPU", "ALL" or a GPU name
    else throw Failure("Unknown --level " + level + " (use fast, max, store, gpu, all or deflate)");
    if (device.empty()) device = "GPU";
    return kDeflate;
}

// Lower-case extension of a stored name ("" when there is none).
std::string extension_of(const std::string& name) {
    size_t slash = name.find_last_of('/'), dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == name.size()) return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext;
}

void put_bytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}

}  // namespace

void create_pack(const std::wstring& source_in, const std::wstring& target_in, const std::string& level) {
    std::wstring source = absolute_path(source_in), target = absolute_path(target_in);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(long_path(source).c_str(), GetFileExInfoStandard, &info))
        throw Failure("Source not found: " + utf8(source));
    bool source_is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (exists(target)) throw Failure("Choose a new archive name");
    if (source_is_dir && is_within(target, source)) throw Failure("Save the archive outside the source folder");
    std::string device;
    const uint8_t method = parse_level(level, device);
    const size_t block_size = size_t(env_chunks("OCLZIP_PACK_BLOCK_MB", method == kLzms ? 16 : 4)) << 20;
    const unsigned threads = std::max(1u, env_chunks("OCLZIP_PACK_THREADS", std::thread::hardware_concurrency()));
    const bool dedupe = env_flag("OCLZIP_DEDUPE", true);

    double start = seconds_now();
    check_link(source, info.dwFileAttributes);
    std::vector<Entry> entries;
    std::string root = utf8(name_of(source));
    entries.push_back({source, root + (source_is_dir ? "/" : ""), source_is_dir, info.ftLastWriteTime,
                       (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow});
    if (source_is_dir) walk(source, root + "/", entries);
    // DEFLATE levels: the ZIP encoder on the GPU / CPU / both, fed many blocks per batch.
    const uint32_t chunk = deflate_chunk_size();
    std::vector<std::unique_ptr<ChunkCompressor>> engines;
    if (method == kDeflate) {
        engines = make_chunk_compressors(device, uint32_t((block_size + chunk - 1) / chunk));
        std::string names;
        for (auto& e : engines) names += (names.empty() ? "" : " + ") + e->name();
        say("Solid pack: DEFLATE on " + names + ", " + commas(block_size >> 20) + " MiB blocks, " +
            commas(chunk >> 10) + " KiB chunks; repeated data stored once.");
    } else {
        say("Solid pack: " + std::string(method_name(method)) + ", " + commas(block_size >> 20) + " MiB blocks, " +
            std::to_string(threads) + " compression threads; repeated data stored once.");
    }

    uint64_t duplicate_bytes = 0, duplicate_files = 0;
    // Files go into the stream in folder order (duplicates are left out as they are recognised).
    std::vector<Entry> stream_files;
    std::vector<uint32_t> stream_entry;
    for (uint32_t i = 0; i < entries.size(); i++)
        if (!entries[i].directory) stream_entry.push_back(i);
    // OCLZIP_PACK_ORDER=type places files of the same type next to each other (then in folder
    // order). It made no real difference on the test data, so folder order is the default.
    if (env_value("OCLZIP_PACK_ORDER") == "type") {
        std::vector<std::string> ext(entries.size());
        for (uint32_t i : stream_entry) ext[i] = extension_of(entries[i].name);
        std::stable_sort(stream_entry.begin(), stream_entry.end(),
                         [&](uint32_t x, uint32_t y) { return ext[x] < ext[y]; });
    }
    for (uint32_t i : stream_entry) stream_files.push_back(entries[i]);
    // With dedupe, the read threads cut every file into content-defined chunks and hash them.
    std::vector<char> chunked(stream_files.size(), dedupe ? 2 : 0);

    std::vector<PackEntry> table(entries.size());
    for (uint32_t i = 0; i < entries.size(); i++) {
        table[i].name = entries[i].name;
        table[i].directory = entries[i].directory;
        table[i].modified = entries[i].modified;
        DWORD attr = GetFileAttributesW(long_path(entries[i].path).c_str());
        table[i].attributes = attr == INVALID_FILE_ATTRIBUTES ? 0 : attr;
    }
    std::vector<BlockRecord> blocks;
    uint64_t stream_size = 0, stored_blocks = 0;
    double read_wait = 0, write_wait = 0, free_wait = 0;
    {
        TempDir temp(parent_of(target), L".ocpk-");
        std::wstring staged = join(temp.path(), L"archive.ocpk");
        {
            ZipWriter out(staged);
            std::vector<uint8_t> header(kHeaderSize, 0);
            std::memcpy(header.data(), kMagic, 8);
            uint32_t version = method == kDeflate ? 3 : 2;  // older readers still open non-DEFLATE packs
            std::memcpy(header.data() + 8, &version, 4);
            uint32_t bs = uint32_t(block_size);
            std::memcpy(header.data() + 12, &bs, 4);
            out.write(header.data(), header.size());
            uint64_t estimate = kHeaderSize + kFooterSize;
            for (auto& e : entries) estimate += e.size + (e.size / block_size + 1) * kBlockRecord + e.name.size() + 40;
            out.reserve(estimate);

            // Pipeline: this thread fills blocks from the read-ahead; workers compress; a writer
            // thread writes them back in order.
            size_t pool_size = size_t(threads) * 2 + 4;
            if (method == kDeflate) {  // room for two full batches per engine
                pool_size = 4;
                for (auto& e : engines)
                    pool_size += size_t(2 * ((uint64_t(e->capacity()) * chunk + block_size - 1) / block_size + 1));
            }
            std::vector<std::unique_ptr<PackBlock>> pool;
            Queue<PackBlock*> free_blocks, to_compress;
            for (size_t i = 0; i < pool_size; i++) {
                pool.push_back(std::make_unique<PackBlock>());
                pool.back()->raw.resize(block_size);
                pool.back()->packed.resize(block_size);
                free_blocks.push(pool.back().get());
            }
            std::mutex done_mutex;
            std::condition_variable done_cv;
            std::map<uint64_t, PackBlock*> done;
            uint64_t total_blocks = UINT64_MAX;  // known once reading has finished
            std::exception_ptr error;
            std::atomic<bool> failed{false};
            auto fail = [&](std::exception_ptr e) {
                {
                    std::lock_guard<std::mutex> lock(done_mutex);
                    if (!error) error = e;
                }
                failed = true;
                free_blocks.close();
                to_compress.close();
                done_cv.notify_all();
            };

            std::vector<std::thread> workers;
            auto finished = [&](PackBlock* b) {
                {
                    std::lock_guard<std::mutex> lock(done_mutex);
                    done[b->seq] = b;
                }
                done_cv.notify_all();
            };
            for (auto& engine : engines) {
                workers.emplace_back([&, e = engine.get()] {
                    try {
                        WorkerPool finishers(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
                        std::vector<PackBlock*> held;
                        std::vector<uint32_t> first;
                        uint32_t used = 0;
                        auto chunks_of = [&](const PackBlock* x) { return uint32_t((x->raw_size + chunk - 1) / chunk); };
                        auto add = [&](PackBlock* x) {
                            first.push_back(used);
                            for (size_t at = 0; at < x->raw_size; at += chunk, used++) {
                                uint32_t n = uint32_t(std::min<size_t>(chunk, x->raw_size - at));
                                std::memcpy(e->input() + size_t(used) * chunk, x->raw.data() + at, n);
                                e->lengths()[used] = n;
                            }
                            held.push_back(x);
                        };
                        PackBlock* b;
                        while (to_compress.pop(b)) {
                            held.clear();
                            first.clear();
                            used = 0;
                            add(b);
                            // Take more blocks that are already waiting, up to a full batch.
                            while (to_compress.try_pop_if(b, [&](PackBlock* x) { return used + chunks_of(x) <= e->capacity(); }))
                                add(b);
                            e->run(used);
                            std::atomic<size_t> next{0};
                            finishers.run([&](unsigned) {
                                for (size_t k; (k = next.fetch_add(1)) < held.size();) {
                                    PackBlock* x = held[k];
                                    x->crc = crc32(0, x->raw.data(), x->raw_size);
                                    x->method = kStored;
                                    x->packed_size = 0;
                                    uint32_t n = chunks_of(x), size;
                                    size_t total = 5;
                                    for (uint32_t j = 0; j < n; j++) {
                                        e->output(first[k] + j, size);
                                        total += size;
                                    }
                                    if (total >= x->raw_size - x->raw_size / 100) continue;  // keep it only if it saves 1%
                                    size_t o = 0;
                                    for (uint32_t j = 0; j < n; j++) {
                                        const uint8_t* p = e->output(first[k] + j, size);
                                        std::memcpy(x->packed.data() + o, p, size);
                                        o += size;
                                    }
                                    static const uint8_t final_block[5] = {1, 0, 0, 0xFF, 0xFF};
                                    std::memcpy(x->packed.data() + o, final_block, 5);
                                    x->method = kDeflate;
                                    x->packed_size = o + 5;
                                }
                            });
                            for (PackBlock* x : held) finished(x);
                        }
                    } catch (...) {
                        fail(std::current_exception());
                    }
                });
            }
            for (unsigned t = 0; t < (method == kDeflate ? 0u : threads); t++) {
                workers.emplace_back([&] {
                    // Below normal, so the read threads (and their CRC/SHA work) always get a core.
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    try {
                        Codec codec(method);
                        std::vector<uint8_t> scratch;
                        PackBlock* b;
                        while (to_compress.pop(b)) {
                            b->crc = crc32(0, b->raw.data(), b->raw_size);
                            b->method = kStored;
                            b->packed_size = 0;
                            if (method != kStored && !looks_incompressible(codec, b->raw.data(), b->raw_size, scratch)) {
                                // Keep it only if it saves at least 1%.
                                size_t packed = codec.compress(b->raw.data(), b->raw_size, b->packed.data(),
                                                               b->raw_size - b->raw_size / 100);
                                if (packed) {
                                    b->method = method;
                                    b->packed_size = packed;
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(done_mutex);
                                done[b->seq] = b;
                            }
                            done_cv.notify_all();
                        }
                    } catch (...) {
                        fail(std::current_exception());
                    }
                });
            }
            std::thread writer([&] {
                try {
                    for (uint64_t next = 0;; next++) {
                        PackBlock* b;
                        {
                            double t = seconds_now();
                            std::unique_lock<std::mutex> lock(done_mutex);
                            done_cv.wait(lock, [&] { return failed || next >= total_blocks || done.count(next); });
                            write_wait += seconds_now() - t;
                            if (failed || next >= total_blocks) return;
                            b = done[next];
                            done.erase(next);
                        }
                        BlockRecord r{out.tell(), uint32_t(b->raw_size), 0, b->crc, b->method};
                        if (b->method == kStored) {
                            out.write(b->raw.data(), b->raw_size);
                            r.packed = uint32_t(b->raw_size);
                            stored_blocks++;
                        } else {
                            out.write(b->packed.data(), b->packed_size);
                            r.packed = uint32_t(b->packed_size);
                        }
                        blocks.push_back(r);
                        free_blocks.push(b);
                    }
                } catch (...) {
                    fail(std::current_exception());
                }
            });

            try {
                ReadAhead ahead(stream_files, env_chunks("OCLZIP_READ_THREADS", 8), chunked);
                PackBlock* b = nullptr;
                uint64_t seq = 0;
                auto submit = [&] {
                    b->seq = seq++;
                    if (!to_compress.push(b)) throw Cancelled{};
                    b = nullptr;
                };
                auto append = [&](const uint8_t* data, size_t size) {  // copies bytes into the stream
                    for (size_t done_bytes = 0; done_bytes < size;) {
                        if (!b) {
                            double t = seconds_now();
                            if (!free_blocks.pop(b)) throw Cancelled{};
                            free_wait += seconds_now() - t;
                            b->raw_size = 0;
                        }
                        size_t n = std::min(size - done_bytes, block_size - b->raw_size);
                        std::memcpy(b->raw.data() + b->raw_size, data + done_bytes, n);
                        b->raw_size += n;
                        done_bytes += n;
                        if (b->raw_size == block_size) submit();
                    }
                };
                auto take = [&](uint32_t index) {
                    double t = seconds_now();
                    ReadAhead::Loaded piece = ahead.take(index);
                    read_wait += seconds_now() - t;
                    if (piece.error) std::rethrow_exception(piece.error);
                    return piece;
                };
                // First stream offset of every chunk stored so far.
                std::unordered_map<std::array<uint8_t, 32>, uint64_t, ShaKey> stored;
                for (uint32_t k = 0; k < stream_files.size(); k++) {
                    PackEntry& e = table[stream_entry[k]];
                    uint32_t crc = 0;
                    uint64_t size = 0, appended = 0;
                    auto add_extent = [&](uint64_t at, uint64_t length) {
                        if (!e.extents.empty() && e.extents.back().first + e.extents.back().second == at)
                            e.extents.back().second += length;
                        else
                            e.extents.push_back({at, length});
                    };
                    uint32_t first = ahead.first_piece(k), count = ahead.piece_count(k);
                    for (uint32_t p = 0; p < count; p++) {
                        ReadAhead::Loaded piece = take(first + p);
                        crc = p ? crc32_combine(crc, piece.crc, piece.data.size) : piece.crc;
                        size += piece.data.size;
                        if (!dedupe) {
                            add_extent(stream_size, piece.data.size);
                            append(piece.data.data(), piece.data.size);
                            stream_size += piece.data.size;
                            continue;
                        }
                        size_t at = 0;
                        for (const Chunk& c : piece.chunks) {
                            if (c.length >= kMinReference) {
                                auto [it, inserted] = stored.try_emplace(c.sha, stream_size);
                                if (!inserted) {  // seen before: refer to the earlier copy
                                    add_extent(it->second, c.length);
                                    duplicate_bytes += c.length;
                                    at += c.length;
                                    continue;
                                }
                            }
                            add_extent(stream_size, c.length);
                            append(piece.data.data() + at, c.length);
                            stream_size += c.length;
                            appended += c.length;
                            at += c.length;
                        }
                    }
                    if (size && !appended && dedupe) duplicate_files++;
                    if (e.extents.empty()) e.extents.push_back({stream_size, 0});
                    e.offset = e.extents.front().first;
                    if (e.extents.size() == 1) e.extents.clear();
                    e.size = size;
                    e.crc = crc;
                }
                if (b && b->raw_size) submit();
                {
                    std::lock_guard<std::mutex> lock(done_mutex);
                    total_blocks = seq;
                }
                done_cv.notify_all();
                to_compress.finish();
            } catch (const Cancelled&) {
            } catch (...) {
                fail(std::current_exception());
            }
            for (auto& t : workers) t.join();
            writer.join();
            if (error) std::rethrow_exception(error);

            // Index: blocks table, file table, footer.
            uint64_t blocks_offset = out.tell();
            std::vector<uint8_t> idx;
            for (auto& r : blocks) {
                put64(idx, r.offset);
                put32(idx, r.raw);
                put32(idx, r.packed);
                put32(idx, r.crc);
                idx.push_back(r.method);
                idx.insert(idx.end(), 3, 0);
            }
            out.write(idx.data(), idx.size());
            std::vector<uint8_t> files;
            for (auto& e : table) {
                files.push_back(e.directory ? 1 : 0);
                put16(files, uint32_t(e.name.size()));
                put_bytes(files, e.name.data(), e.name.size());
                put64(files, e.size);
                put64(files, e.offset);
                put32(files, e.crc);
                put64(files, (uint64_t(e.modified.dwHighDateTime) << 32) | e.modified.dwLowDateTime);
                put32(files, e.attributes);
                put32(files, uint32_t(e.extents.size()));
                for (auto& [at, length] : e.extents) {
                    put64(files, at);
                    put64(files, length);
                }
            }
            uint64_t files_offset = out.tell();
            uint8_t files_method = kStored;
            std::vector<uint8_t> packed(files.size());
            size_t packed_size = 0;
            if (files.size() > 64) {
                Codec c(kXpressHuff);
                packed_size = c.compress(files.data(), files.size(), packed.data(), files.size());
                if (packed_size) files_method = kXpressHuff;
            }
            if (files_method == kStored) out.write(files.data(), files.size());
            else out.write(packed.data(), packed_size);
            std::vector<uint8_t> footer;
            put64(footer, blocks_offset);
            put64(footer, blocks.size());
            put64(footer, files_offset);
            put32(footer, uint32_t(files_method == kStored ? files.size() : packed_size));
            put32(footer, uint32_t(files.size()));
            put32(footer, crc32(0, files.data(), files.size()));
            footer.push_back(files_method);
            footer.insert(footer.end(), 3, 0);
            put_bytes(footer, kEndMagic, 8);
            out.write(footer.data(), footer.size());
            out.close();
        }
        move_new(staged, target);
    }
    uint64_t total = 0, files_count = 0;
    for (auto& e : entries)
        if (!e.directory) {
            total += e.size;
            files_count++;
        }
    uint64_t size = File::open_read(target).size();
    double wall = seconds_now() - start;
    say(commas(files_count) + " files; " + commas(total) + " source bytes -> " + commas(size) + " archive bytes");
    say("Space saved: " + percent(total ? double(int64_t(total) - int64_t(size)) / double(total) : 0.0));
    char line[200];
    if (dedupe) {
        std::snprintf(line, sizeof line, "Repeated data stored once: %s bytes not stored again (%s files entirely)",
                      commas(duplicate_bytes).c_str(), commas(duplicate_files).c_str());
        say(line);
    }
    say(commas(blocks.size()) + " blocks, " + commas(stored_blocks) + " stored because they did not shrink");
    std::snprintf(line, sizeof line,
                  "Elapsed: %.3fs. Packer waited for reads %.3fs and for free blocks %.3fs; writer waited for compressed blocks %.3fs",
                  wall, read_wait, free_wait, write_wait);
    say(line);
}

namespace {

struct Archive {
    File file;
    std::vector<BlockRecord> blocks;
    std::vector<PackEntry> entries;
    uint32_t block_size = 0;
    uint64_t stream_size = 0;
};

Archive open_pack(const std::wstring& path) {
    Archive a;
    a.file = File::open_read(path);
    uint64_t size = a.file.size();
    if (size < kHeaderSize + kFooterSize) throw Failure("Not an .ocpk archive");
    uint8_t header[kHeaderSize], footer[kFooterSize];
    a.file.read_exact(header, sizeof header);
    uint32_t version = get32(header + 8);
    if (std::memcmp(header, kMagic, 8)) throw Failure("Not an .ocpk archive");
    if (version < 1 || version > kVersion) throw Failure("This .ocpk archive needs a newer version of this tool");
    a.block_size = get32(header + 12);
    a.file.seek(size - kFooterSize);
    a.file.read_exact(footer, sizeof footer);
    if (std::memcmp(footer + 40, kEndMagic, 8)) throw Failure("Truncated or damaged .ocpk archive");
    uint64_t blocks_offset = get64(footer), block_count = get64(footer + 8), files_offset = get64(footer + 16);
    uint32_t files_packed = get32(footer + 24), files_raw = get32(footer + 28), files_crc = get32(footer + 32);
    uint8_t files_method = footer[36];
    if (blocks_offset > size || block_count > (size - blocks_offset) / kBlockRecord ||
        blocks_offset + block_count * kBlockRecord != files_offset || files_offset + files_packed + kFooterSize != size)
        throw Failure("Damaged .ocpk index");
    std::vector<uint8_t> idx(size_t(block_count * kBlockRecord));
    a.file.seek(blocks_offset);
    a.file.read_exact(idx.data(), idx.size());
    uint64_t expect = kHeaderSize;
    for (uint64_t i = 0; i < block_count; i++) {
        const uint8_t* p = idx.data() + i * kBlockRecord;
        BlockRecord r{get64(p), get32(p + 8), get32(p + 12), get32(p + 16), p[20]};
        if (r.offset != expect || r.raw == 0 || r.raw > a.block_size || r.method > kDeflate ||
            (r.method == kDeflate && version < 3) ||
            (r.method == kStored && r.packed != r.raw))
            throw Failure("Damaged .ocpk block table");
        expect += r.packed;
        a.stream_size += r.raw;
        a.blocks.push_back(r);
    }
    if (expect != blocks_offset) throw Failure("Damaged .ocpk block table");
    std::vector<uint8_t> packed(files_packed), files(files_raw);
    a.file.seek(files_offset);
    a.file.read_exact(packed.data(), packed.size());
    if (files_method == kStored) {
        if (files_packed != files_raw) throw Failure("Damaged .ocpk file table");
        files = packed;
    } else {
        Decoder(files_method).decompress(packed.data(), packed.size(), files.data(), files.size());
    }
    if (crc32(0, files.data(), files.size()) != files_crc) throw Failure("Damaged .ocpk file table (CRC)");
    for (size_t at = 0; at < files.size();) {
        if (files.size() - at < 3) throw Failure("Damaged .ocpk file table");
        PackEntry e;
        e.directory = files[at] == 1;
        uint32_t n = get16(files.data() + at + 1);
        at += 3;
        if (files.size() - at < n + 32ull) throw Failure("Damaged .ocpk file table");
        e.name.assign(reinterpret_cast<const char*>(files.data() + at), n);
        at += n;
        e.size = get64(files.data() + at);
        e.offset = get64(files.data() + at + 8);
        e.crc = get32(files.data() + at + 16);
        uint64_t t = get64(files.data() + at + 20);
        e.modified.dwLowDateTime = DWORD(t);
        e.modified.dwHighDateTime = DWORD(t >> 32);
        e.attributes = get32(files.data() + at + 28);
        at += 32;
        if (version >= 2) {
            if (files.size() - at < 4) throw Failure("Damaged .ocpk file table");
            uint32_t count = get32(files.data() + at);
            at += 4;
            if (count > (files.size() - at) / 16) throw Failure("Damaged .ocpk file table");
            uint64_t sum = 0;
            for (uint32_t x = 0; x < count; x++, at += 16) {
                uint64_t off = get64(files.data() + at), length = get64(files.data() + at + 8);
                if (off > a.stream_size || length > a.stream_size - off || length > e.size - sum)
                    throw Failure("Damaged .ocpk file table (range)");
                sum += length;
                e.extents.push_back({off, length});
            }
            if (count && (e.directory || sum != e.size)) throw Failure("Damaged .ocpk file table (extents)");
        }
        if (!e.directory && e.extents.empty()) {
            if (e.offset > a.stream_size || e.size > a.stream_size - e.offset)
                throw Failure("Damaged .ocpk file table (range)");
            e.extents.push_back({e.offset, e.size});
        }
        a.entries.push_back(std::move(e));
    }
    return a;
}

// A part of one file that lies inside one block.
struct Span {
    uint32_t entry;
    uint64_t file_offset;  // where in the file
    uint32_t block_offset, length;
};

}  // namespace

// Extracts (or with target empty, only tests) an .ocpk archive.
void restore_pack(const std::wstring& source_in, const std::wstring& target_in) {
    bool test_only = target_in.empty();
    std::wstring source = absolute_path(source_in);
    std::wstring target = test_only ? std::wstring() : absolute_path(target_in);
    if (!test_only && exists(target)) throw Failure("Choose a new destination folder");
    double start = seconds_now();
    Archive a = open_pack(source);
    const unsigned threads = std::max(1u, env_chunks("OCLZIP_PACK_THREADS", std::thread::hardware_concurrency()));

    // Which parts of which files each block holds (duplicates share the same stream bytes).
    std::vector<uint64_t> block_start(a.blocks.size() + 1, 0);
    for (size_t i = 0; i < a.blocks.size(); i++) block_start[i + 1] = block_start[i] + a.blocks[i].raw;
    std::vector<std::vector<Span>> spans(a.blocks.size());
    for (uint32_t i = 0; i < a.entries.size(); i++) {
        const PackEntry& e = a.entries[i];
        if (e.directory || !e.size) continue;
        uint64_t file_at = 0;
        for (auto [offset, length] : e.extents) {
            size_t b = size_t(std::upper_bound(block_start.begin(), block_start.end(), offset) - block_start.begin()) - 1;
            for (uint64_t at = offset, end = offset + length; at < end; b++) {
                uint64_t stop = std::min(end, block_start[b + 1]);
                spans[b].push_back({i, file_at + (at - offset), uint32_t(at - block_start[b]), uint32_t(stop - at)});
                at = stop;
            }
            file_at += length;
        }
    }

    std::unique_ptr<TempDir> temp;
    std::wstring root;
    std::vector<std::wstring> paths(a.entries.size());
    // Open output files; each is closed (with its time set) when its last piece has been written.
    std::vector<HANDLE> handles(a.entries.size(), INVALID_HANDLE_VALUE);
    std::unique_ptr<std::atomic<uint32_t>[]> remaining(new std::atomic<uint32_t>[a.entries.size()]);
    for (size_t i = 0; i < a.entries.size(); i++) remaining[i] = 0;
    for (auto& list : spans)
        for (const Span& sp : list) remaining[sp.entry]++;
    auto close_file = [&](uint32_t i) {
        if (handles[i] == INVALID_HANDLE_VALUE) return;
        SetFileTime(handles[i], nullptr, nullptr, &a.entries[i].modified);
        CloseHandle(handles[i]);
        handles[i] = INVALID_HANDLE_VALUE;
    };
    struct CloseAll {  // on failure, before the temporary folder is removed
        std::vector<HANDLE>& h;
        ~CloseAll() {
            for (HANDLE x : h)
                if (x != INVALID_HANDLE_VALUE) CloseHandle(x);
        }
    } close_all{handles};
    if (!test_only) {
        temp = std::make_unique<TempDir>(parent_of(target), L".ocpk-restore-");
        root = join(temp->path(), L"restored");
        make_directory(root);
        std::set<std::wstring> names;
        for (uint32_t i = 0; i < a.entries.size(); i++) {
            const PackEntry& e = a.entries[i];
            std::vector<std::wstring> parts = safe_archive_parts(e.name);
            std::wstring key;
            for (auto& part : parts) key += (key.empty() ? L"" : L"/") + part;
            if (!names.insert(fold_case(key)).second) throw Failure("Duplicate archive path");
            std::wstring path = root;
            size_t folders = e.directory ? parts.size() : parts.size() - 1;
            for (size_t k = 0; k < folders; k++) {
                path = join(path, parts[k]);
                make_directory(path);
            }
            if (e.directory) {
                paths[i] = path;
                continue;
            }
            path = join(path, parts.back());
            paths[i] = path;
            // Created at full size now; the blocks fill it in, in any order. The handle stays open
            // until the file's last piece is written: closing a just-written file makes antivirus
            // scanners look at it again, and a large file is written in thousands of pieces.
            HANDLE h = CreateFileW(long_path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) throw Failure("Cannot create " + utf8(path) + ": " + win_error(GetLastError()));
            handles[i] = h;
            FILE_END_OF_FILE_INFO eof;
            eof.EndOfFile.QuadPart = LONGLONG(e.size);
            if (e.size && !SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof))
                throw Failure("Cannot size " + utf8(path));
            if (remaining[i] == 0) close_file(i);  // empty file: done
        }
    }

    // Per-file CRC pieces, combined in file order at the end.
    struct Piece {
        uint64_t offset;
        uint32_t length, crc;
    };
    std::vector<std::vector<Piece>> pieces(a.entries.size());
    std::mutex pieces_mutex;
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex error_mutex;
    std::atomic<bool> failed{false};
    auto work = [&] {
        try {
            File in = File::open_read(source);
            std::vector<uint8_t> packed(a.block_size), raw(a.block_size);
            std::unique_ptr<Decoder> decoders[3];
            for (size_t b; !failed && (b = next.fetch_add(1)) < a.blocks.size();) {
                const BlockRecord& r = a.blocks[b];
                in.seek(r.offset);
                uint8_t* data;
                if (r.method == kStored) {
                    in.read_exact(raw.data(), r.raw);
                } else if (r.method == kDeflate) {
                    in.read_exact(packed.data(), r.packed);
                    inflate_block(packed.data(), r.packed, raw.data(), r.raw);
                } else {
                    in.read_exact(packed.data(), r.packed);
                    if (!decoders[r.method]) decoders[r.method] = std::make_unique<Decoder>(r.method);
                    decoders[r.method]->decompress(packed.data(), r.packed, raw.data(), r.raw);
                }
                data = raw.data();
                if (crc32(0, data, r.raw) != r.crc) throw Failure("Bad CRC-32 in block " + std::to_string(b));
                for (const Span& s : spans[b]) {
                    uint32_t crc = crc32(0, data + s.block_offset, s.length);
                    if (!test_only) {
                        OVERLAPPED at{};
                        at.Offset = DWORD(s.file_offset);
                        at.OffsetHigh = DWORD(s.file_offset >> 32);
                        DWORD wrote = 0;
                        if (!WriteFile(handles[s.entry], data + s.block_offset, s.length, &wrote, &at) ||
                            wrote != s.length)
                            throw Failure("Cannot write " + utf8(paths[s.entry]) + ": " + win_error(GetLastError()));
                        if (remaining[s.entry].fetch_sub(1) == 1) close_file(s.entry);
                    }
                    std::lock_guard<std::mutex> lock(pieces_mutex);
                    pieces[s.entry].push_back({s.file_offset, s.length, crc});
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error) error = std::current_exception();
            failed = true;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);

    uint64_t files = 0, bytes = 0;
    for (uint32_t i = 0; i < a.entries.size(); i++) {
        const PackEntry& e = a.entries[i];
        if (e.directory) continue;
        auto& list = pieces[i];
        std::sort(list.begin(), list.end(), [](const Piece& x, const Piece& y) { return x.offset < y.offset; });
        uint32_t crc = 0;
        uint64_t at = 0;
        for (auto& p : list) {
            if (p.offset != at) throw Failure("Missing data for " + e.name);
            crc = at ? crc32_combine(crc, p.crc, p.length) : p.crc;
            at += p.length;
        }
        if (at != e.size || crc != e.crc) throw Failure("Bad CRC-32 for file " + e.name);
        files++;
        bytes += e.size;
    }
    if (!test_only) {
        // File times were set as each file was closed. Folder times last (deepest first, after their
        // contents), then read-only and similar attributes.
        for (size_t k = a.entries.size(); k-- > 0;) {
            const PackEntry& e = a.entries[k];
            if (e.directory) {
                HANDLE h = CreateFileW(long_path(paths[k]).c_str(), FILE_WRITE_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    SetFileTime(h, nullptr, nullptr, &e.modified);
                    CloseHandle(h);
                }
                continue;
            }
            DWORD keep = e.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
            if (keep) SetFileAttributesW(long_path(paths[k]).c_str(), keep);
        }
        move_new(root, target);
        say("Restored to: " + utf8(target));
    }
    char line[160];
    std::snprintf(line, sizeof line, "%s files, %s bytes, %s blocks: all CRC-32 checks PASS (%.3fs)",
                  commas(files).c_str(), commas(bytes).c_str(), commas(a.blocks.size()).c_str(), seconds_now() - start);
    say(line);
}

bool is_pack_archive(const std::wstring& path) {
    uint8_t magic[8] = {};
    File f = File::open_read(absolute_path(path));
    f.read(magic, sizeof magic);
    return std::memcmp(magic, kMagic, 8) == 0;
}
