// Building blocks shared by the ZIP writer (zip_create.cpp) and the solid pack format (pack.cpp):
// folder listing, parallel read-ahead with recycled buffers, a background output writer,
// a persistent worker pool, CRC-32 combination and environment settings.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "common.h"

#include <bcrypt.h>

namespace archive_io {

struct Cancelled {};

inline uint32_t env_chunks(const char* name, uint32_t fallback) {
    char v[32];
    DWORD len = GetEnvironmentVariableA(name, v, sizeof v);
    if (len > 0 && len < sizeof v) {
        long n = std::strtol(v, nullptr, 10);
        if (n >= 1 && n <= 65536) return uint32_t(n);
    }
    return fallback;
}

// An on/off setting: "0" turns it off, "1" on, anything else (or unset) keeps the default.
inline bool env_flag(const char* name, bool fallback) {
    char v[8];
    DWORD len = GetEnvironmentVariableA(name, v, sizeof v);
    if (len == 1 && (v[0] == '0' || v[0] == '1')) return v[0] == '1';
    return fallback;
}

// A short text setting ("" when unset or too long).
inline std::string env_value(const char* name) {
    char v[64];
    DWORD len = GetEnvironmentVariableA(name, v, sizeof v);
    return len > 0 && len < sizeof v ? std::string(v, len) : std::string();
}

// Worker threads that stay alive for the whole run (instead of new threads for every batch).
class WorkerPool {
public:
    explicit WorkerPool(unsigned threads) {
        for (unsigned t = 1; t < threads; t++) threads_.emplace_back([this, t] { loop(t); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        start_.notify_all();
        for (auto& t : threads_) t.join();
    }
    unsigned size() const { return unsigned(threads_.size()) + 1; }
    // Runs job(thread_index) on every thread (the caller is thread 0) and waits for all of them.
    void run(const std::function<void(unsigned)>& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            running_ = unsigned(threads_.size());
            generation_++;
        }
        start_.notify_all();
        job(0);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return running_ == 0; });
        job_ = nullptr;
    }

private:
    void loop(unsigned index) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(unsigned)>* job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) return;
                seen = generation_;
                job = job_;
            }
            (*job)(index);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (--running_ == 0) done_.notify_one();
            }
        }
    }
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    const std::function<void(unsigned)>* job_ = nullptr;
    uint64_t generation_ = 0;
    unsigned running_ = 0;
    bool stopping_ = false;
};

struct Entry {
    std::wstring path;
    std::string name;  // UTF-8 archive name
    bool directory;
    FILETIME modified;
    uint64_t size = 0;  // file size when the folder was listed
};

inline void check_link(const std::wstring& path, DWORD attr) {
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Failure("Links and junctions are not supported: " + utf8(path));
}

// Same order as os.walk(top-down) with sorted(dirs)+sorted(files) per folder.
inline void walk(const std::wstring& folder, const std::string& prefix, std::vector<Entry>& entries) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(long_path(join(folder, L"*")).c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return;
        throw Failure("Cannot list " + utf8(folder) + ": " + win_error(error));
    }
    std::vector<Entry> dirs, files;
    do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = join(folder, name);
        check_link(path, data.dwFileAttributes);
        bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        Entry e{path, prefix + utf8(name), directory, data.ftLastWriteTime,
                (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
        (directory ? dirs : files).push_back(std::move(e));
    } while (FindNextFileW(find, &data));
    DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) throw Failure("Cannot list " + utf8(folder) + ": " + win_error(error));
    auto by_name = [](const Entry& a, const Entry& b) { return name_of(a.path) < name_of(b.path); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    for (auto& d : dirs) entries.push_back({d.path, d.name + "/", true, d.modified});
    for (auto& f : files) entries.push_back(f);
    for (auto& d : dirs) walk(d.path, d.name + "/", entries);
}

// Sequential ZIP output on its own I/O thread. The main thread copies into 8 MiB page-aligned
// blocks; the I/O thread writes them with FILE_FLAG_NO_BUFFERING, so tens of GB of output do not
// pass through (and flush out) the Windows file cache. Header patches for entries that span
// batches are collected and written at the end with positioned writes.
// OCLZIP_UNBUFFERED_WRITE=0 uses ordinary cached writes instead (same bytes).
class ZipWriter {
public:
    explicit ZipWriter(const std::wstring& path) : path_(path) {
        unbuffered_ = env_flag("OCLZIP_UNBUFFERED_WRITE", true);
        handle_ = CreateFileW(long_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | (unbuffered_ ? FILE_FLAG_NO_BUFFERING : 0), nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw Failure("Cannot create " + utf8(path) + ": " + win_error(GetLastError()));
        for (int i = 0; i < kBlocks; i++) {
            void* p = VirtualAlloc(nullptr, kBlockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!p) throw Failure("Out of memory for the ZIP write buffers");
            blocks_.push_back(static_cast<uint8_t*>(p));
            free_.push_back(blocks_.back());
        }
        unsigned io_threads = env_chunks("OCLZIP_WRITE_THREADS", 1);
        for (unsigned t = 0; t < std::min(io_threads, 4u); t++) io_.emplace_back([this] { io_loop(); });
        current_ = take_free();
    }
    ~ZipWriter() {
        stop_io();
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        for (uint8_t* p : blocks_) VirtualFree(p, 0, MEM_RELEASE);
    }
    uint64_t tell() const { return position_; }
    // Reserves disk space for the whole archive up front (one allocation instead of growing the
    // file 8 MiB at a time). Only a hint: failure is ignored, and close() sets the real size.
    void reserve(uint64_t bytes) {
        FILE_ALLOCATION_INFO info;
        info.AllocationSize.QuadPart = LONGLONG(bytes);
        SetFileInformationByHandle(handle_, FileAllocationInfo, &info, sizeof info);
    }
    void write(const void* data, size_t size) {
        auto* p = static_cast<const uint8_t*>(data);
        while (size) {
            size_t n = std::min(size, kBlockSize - used_);
            std::memcpy(current_ + used_, p, n);
            used_ += n;
            position_ += n;
            p += n;
            size -= n;
            if (used_ == kBlockSize) submit();
        }
    }
    // Overwrites bytes already written (applied when the file is closed).
    void patch(uint64_t offset, const std::vector<uint8_t>& bytes) { patches_.push_back({offset, bytes}); }
    void close() {
        if (used_) submit();  // the last block is padded to a sector multiple, then the end is cut back
        stop_io();
        if (error_) std::rethrow_exception(error_);
        FILE_END_OF_FILE_INFO eof;
        eof.EndOfFile.QuadPart = LONGLONG(position_);
        if (!SetFileInformationByHandle(handle_, FileEndOfFileInfo, &eof, sizeof eof))
            throw Failure("Cannot set ZIP size: " + win_error(GetLastError()));
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        if (patches_.empty()) return;
        File f = File::open_write_existing(path_);
        for (auto& [offset, bytes] : patches_) {
            f.seek(offset);
            f.write(bytes.data(), bytes.size());
        }
        f.close();
    }
    double wait_seconds = 0;  // main thread waiting for a free block (the disk is behind)

private:
    static constexpr size_t kBlockSize = 8u << 20;
    static constexpr int kBlocks = 6;
    struct Job {
        uint8_t* data;
        size_t size;
        uint64_t offset;  // positioned writes, so several I/O threads can have writes in flight
    };

    uint8_t* take_free() {
        double t = seconds_now();
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return !free_.empty() || error_; });
        if (error_) std::rethrow_exception(error_);
        uint8_t* p = free_.front();
        free_.pop_front();
        wait_seconds += seconds_now() - t;
        return p;
    }
    void submit() {
        size_t size = used_;
        if (unbuffered_ && size % kSector) {
            size_t padded = (size + kSector - 1) / kSector * kSector;
            std::memset(current_ + size, 0, padded - size);
            size = padded;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_) std::rethrow_exception(error_);
            jobs_.push_back({current_, size, block_offset_});
        }
        changed_.notify_all();
        block_offset_ += used_;  // only the last block is short, so offsets stay sector-aligned
        used_ = 0;
        current_ = take_free();
    }
    void io_loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;
                job = jobs_.front();
                jobs_.pop_front();
            }
            DWORD wrote = 0;
            OVERLAPPED at{};
            at.Offset = DWORD(job.offset);
            at.OffsetHigh = DWORD(job.offset >> 32);
            bool ok = WriteFile(handle_, job.data, DWORD(job.size), &wrote, &at) && wrote == job.size;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ok && !error_)
                    error_ = std::make_exception_ptr(
                        Failure("Cannot write " + utf8(path_) + ": " + win_error(GetLastError())));
                free_.push_back(job.data);
            }
            changed_.notify_all();
        }
    }
    void stop_io() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        for (auto& t : io_)
            if (t.joinable()) t.join();
        io_.clear();
    }

    static constexpr size_t kSector = 4096;  // a multiple of every common sector size
    std::wstring path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool unbuffered_ = true;
    std::vector<uint8_t*> blocks_;
    std::deque<uint8_t*> free_;
    std::deque<Job> jobs_;
    std::vector<std::thread> io_;
    uint64_t block_offset_ = 0;  // file offset of the block being filled
    std::mutex mutex_;
    std::condition_variable changed_;
    std::exception_ptr error_;
    bool stopping_ = false;
    uint8_t* current_ = nullptr;
    size_t used_ = 0;
    uint64_t position_ = 0;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> patches_;
};

// CRC-32 of A followed by B, from crc(A), crc(B) and len(B) (zlib's crc32_combine method).
inline uint32_t crc_multiply(uint32_t a, uint32_t b) {
    uint32_t m = 1u << 31, p = 0;
    for (;;) {
        if (a & m) {
            p ^= b;
            if ((a & (m - 1)) == 0) break;
        }
        m >>= 1;
        b = (b & 1) ? (b >> 1) ^ 0xEDB88320u : b >> 1;
    }
    return p;
}

inline uint32_t crc32_combine(uint32_t crc_a, uint32_t crc_b, uint64_t len_b) {
    static const auto powers = [] {  // x^(2^k) mod p, for k = 0..63 (bits)
        std::array<uint32_t, 64> t{};
        uint32_t p = 1u << 30;  // x^1
        for (auto& v : t) {
            v = p;
            p = crc_multiply(p, p);
        }
        return t;
    }();
    uint32_t x = 1u << 31;  // x^0
    unsigned k = 3;         // len_b is in bytes = 2^3 bits
    for (uint64_t n = len_b; n; n >>= 1, k++)
        if (n & 1) x = crc_multiply(powers[k & 63], x);
    return crc_multiply(x, crc_a) ^ crc_b;
}

// Parallel reading: every file is split into pieces of up to 8 MiB. Several threads read and
// checksum pieces at the same time (many disk requests in flight, CRC on many cores), while the
// reader takes the pieces strictly in archive order and only copies them into batches.
constexpr uint64_t kPieceSize = 8ull << 20;
constexpr uint64_t kReadAheadBudget = 512ull << 20;  // memory for pieces read ahead
constexpr uint32_t kReadAheadPieces = 16384;          // how far ahead (in pieces)

// Page-aligned read buffer. Full-piece buffers are recycled through a small pool, so each 8 MiB
// read lands in memory that is already mapped (no zero-filling, no page faults per piece).
class PieceBuffer {
public:
    static constexpr size_t kAlign = 4096;
    static constexpr size_t kPooled = kPieceSize + 2 * kAlign;  // a full piece plus the extra byte, rounded

    PieceBuffer() = default;
    explicit PieceBuffer(size_t capacity) {
        capacity = (std::max<size_t>(capacity, 1) + kAlign - 1) / kAlign * kAlign;
        if (capacity > kPieceSize / 4 && capacity <= kPooled) {
            capacity = kPooled;
            std::lock_guard<std::mutex> lock(pool_mutex());
            if (!pool().empty()) {
                data_ = pool().back();
                pool().pop_back();
            }
        }
        if (!data_) data_ = static_cast<uint8_t*>(_aligned_malloc(capacity, kAlign));
        if (!data_) throw Failure("Out of memory for read buffers");
        capacity_ = capacity;
    }
    PieceBuffer(PieceBuffer&& o) noexcept { *this = std::move(o); }
    PieceBuffer& operator=(PieceBuffer&& o) noexcept {
        if (this != &o) {
            release();
            data_ = o.data_;
            capacity_ = o.capacity_;
            size = o.size;
            o.data_ = nullptr;
            o.capacity_ = o.size = 0;
        }
        return *this;
    }
    ~PieceBuffer() { release(); }
    uint8_t* data() const { return data_; }
    size_t capacity() const { return capacity_; }
    // Grows (keeping the first `keep` bytes).
    void grow(size_t capacity, size_t keep) {
        PieceBuffer bigger(capacity);
        if (keep) std::memcpy(bigger.data_, data_, keep);
        *this = std::move(bigger);
    }
    size_t size = 0;

private:
    void release() {
        if (!data_) return;
        if (capacity_ == kPooled) {
            std::lock_guard<std::mutex> lock(pool_mutex());
            if (pool().size() < 96) {
                pool().push_back(data_);
                data_ = nullptr;
                return;
            }
        }
        _aligned_free(data_);
        data_ = nullptr;
    }
    static std::vector<uint8_t*>& pool() {
        static std::vector<uint8_t*> p;
        return p;
    }
    static std::mutex& pool_mutex() {
        static std::mutex m;
        return m;
    }
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
};

// OCLZIP_UNBUFFERED_READ=1 reads source files with FILE_FLAG_NO_BUFFERING (bypasses the file cache).
inline bool unbuffered_reads() { return env_flag("OCLZIP_UNBUFFERED_READ", false); }

// ---- Content-defined chunks (for finding repeated data inside and across files) ----
// A cut is made where a rolling "gear" hash of the last 64 bytes has its top 16 bits zero, but not
// before kChunkMin bytes and always at kChunkMax, so chunks average about 80 KiB. Cut points depend
// on the content, so the same data gives the same chunks wherever it appears (pieces of 8 MiB are
// cut independently, which only loses a chunk or so at each piece boundary).
constexpr uint32_t kChunkMin = 16u << 10, kChunkMax = 256u << 10;

struct Chunk {
    uint32_t length;
    std::array<uint8_t, 32> sha;  // SHA-256 of the chunk
};

inline const uint64_t* gear_table() {
    static const std::array<uint64_t, 256> table = [] {
        std::array<uint64_t, 256> g{};
        uint64_t x = 0x0123456789ABCDEFull;
        for (auto& v : g) {  // splitmix64: fixed, so archives are reproducible
            x += 0x9E3779B97F4A7C15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
        return g;
    }();
    return table.data();
}

inline std::array<uint8_t, 32> sha256_once(const uint8_t* p, size_t n) {
    std::array<uint8_t, 32> out{};
    // BCRYPT_SHA256_ALG_HANDLE: the shared provider, no per-call setup (Windows 10 and later).
    if (BCryptHash(reinterpret_cast<BCRYPT_ALG_HANDLE>(uintptr_t(0x41)), nullptr, 0, const_cast<PUCHAR>(p), ULONG(n),
                   out.data(), ULONG(out.size())) != 0) {
        Sha256 h;  // older systems
        h.update(p, n);
        out = h.digest();
    }
    return out;
}

inline void cut_chunks(const uint8_t* p, size_t n, std::vector<Chunk>& out) {
    const uint64_t* g = gear_table();
    for (size_t start = 0; start < n;) {
        size_t limit = std::min<size_t>(n - start, kChunkMax), cut = limit;
        if (limit > kChunkMin) {
            const uint8_t* q = p + start;
            uint64_t fp = 0;
            for (size_t i = kChunkMin - 64; i < kChunkMin; i++) fp = (fp << 1) + g[q[i]];  // fill the window
            for (size_t i = kChunkMin; i < limit; i++) {
                fp = (fp << 1) + g[q[i]];
                if ((fp >> 48) == 0) {
                    cut = i + 1;
                    break;
                }
            }
        }
        out.push_back({uint32_t(cut), sha256_once(p + start, cut)});
        start += cut;
    }
}

class ReadAhead {
public:
    struct Piece {
        uint32_t entry;
        uint64_t offset, size;  // planned size (from the folder listing)
        bool last;              // last piece of its file: read to end of file
    };
    struct Loaded {
        PieceBuffer data;  // data.size bytes valid
        uint32_t crc = 0;
        std::array<uint8_t, 32> sha{};  // SHA-256 of the piece, for entries with hash bit 1
        std::vector<Chunk> chunks;      // content-defined chunks of the piece, for entries with hash bit 2
        std::exception_ptr error;
        bool ready = false;
    };

    // hash (optional): per entry, bit 1 = also compute SHA-256 of every piece, bit 2 = also cut every
    // piece into content-defined chunks with their SHA-256 (both for deduplication).
    ReadAhead(const std::vector<Entry>& entries, unsigned threads, std::vector<char> hash = {})
        : entries_(entries), hash_(std::move(hash)) {
        first_.resize(entries.size() + 1);
        for (uint32_t i = 0; i < entries.size(); i++) {
            first_[i] = uint32_t(pieces_.size());
            if (entries[i].directory) continue;
            uint64_t size = entries[i].size;
            for (uint64_t at = 0;; at += kPieceSize) {
                bool last = size - at <= kPieceSize;
                pieces_.push_back({i, at, last ? size - at : kPieceSize, last});
                if (last) break;
            }
        }
        first_[entries.size()] = uint32_t(pieces_.size());
        slots_.resize(pieces_.size());
        for (unsigned t = 0; t < std::max(1u, threads); t++) threads_.emplace_back([this] { run(); });
    }
    ~ReadAhead() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        for (auto& t : threads_) t.join();
    }
    uint32_t first_piece(uint32_t entry) const { return first_[entry]; }
    uint32_t piece_count(uint32_t entry) const { return first_[entry + 1] - first_[entry]; }

    // Waits for a piece (in order) and takes its data.
    Loaded take(uint32_t index) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return slots_[index].ready || stopping_; });
        if (!slots_[index].ready) throw Cancelled{};
        Loaded out = std::move(slots_[index]);
        slots_[index] = Loaded{};
        in_flight_ -= out.data.size;
        consumed_ = index + 1;
        lock.unlock();
        changed_.notify_all();
        return out;
    }

private:
    void run() {
        for (;;) {
            uint32_t index;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] {
                    return stopping_ || next_ >= pieces_.size() ||
                           (next_ - consumed_ < kReadAheadPieces && in_flight_ < kReadAheadBudget);
                });
                if (stopping_ || next_ >= pieces_.size()) return;
                index = next_++;
                in_flight_ += pieces_[index].size;
            }
            const Piece& piece = pieces_[index];
            const Entry& entry = entries_[piece.entry];
            Loaded result;
            try {
                try {
                    read_piece(piece, entry, unbuffered_, result.data);
                } catch (const Failure&) {
                    if (!unbuffered_) throw;
                    read_piece(piece, entry, false, result.data);  // e.g. a file system without unbuffered I/O
                }
                if (!piece.last && result.data.size != piece.size)
                    throw Failure("File changed while it was being archived: " + utf8(entry.path));
                result.crc = crc32(0, result.data.data(), result.data.size);
                if (piece.entry < hash_.size() && (hash_[piece.entry] & 2))
                    cut_chunks(result.data.data(), result.data.size, result.chunks);
                if (piece.entry < hash_.size() && (hash_[piece.entry] & 1)) {
                    Sha256 h;
                    h.update(result.data.data(), result.data.size);
                    result.sha = h.digest();
                }
            } catch (...) {
                result.error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                in_flight_ += result.data.size;
                in_flight_ -= piece.size;
                result.ready = true;
                slots_[index] = std::move(result);
            }
            changed_.notify_all();
        }
    }

    // Reads one piece. The last piece of a file reads one byte extra (rounded up to a whole
    // sector when unbuffered) to notice a file that grew since it was listed, and then keeps going.
    static void read_piece(const Piece& piece, const Entry& entry, bool unbuffered, PieceBuffer& out) {
        File f = unbuffered ? File::open_read_unbuffered(entry.path) : File::open_read(entry.path);
        if (piece.offset) f.seek(piece.offset);
        size_t want = size_t(piece.size) + (piece.last ? 1 : 0);
        if (unbuffered) want = (want + PieceBuffer::kAlign - 1) / PieceBuffer::kAlign * PieceBuffer::kAlign;
        if (out.capacity() < want) out = PieceBuffer(want);
        size_t got = f.read(out.data(), want);
        while (piece.last && got == want) {  // the file is longer than listed
            size_t bigger = (want * 2 + PieceBuffer::kAlign) / PieceBuffer::kAlign * PieceBuffer::kAlign;
            out.grow(bigger, got);
            got += f.read(out.data() + got, bigger - got);
            want = bigger;
        }
        out.size = got;
    }

    const std::vector<Entry>& entries_;
    std::vector<char> hash_;
    bool unbuffered_ = unbuffered_reads();
    std::vector<Piece> pieces_;
    std::vector<uint32_t> first_;
    std::vector<Loaded> slots_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable changed_;
    uint32_t next_ = 0, consumed_ = 0;
    uint64_t in_flight_ = 0;
    bool stopping_ = false;
};

}  // namespace archive_io
