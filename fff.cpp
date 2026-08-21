#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#include <sched.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <array>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <thread>
#include <mutex>

#include <immintrin.h>

// ---------------------------------------------------------------------------
// Output: one shared mutex, thread-local fixed-capacity buffers (manual length,
// no value-initialization on append).
// ---------------------------------------------------------------------------
static std::mutex output_mutex;

constexpr size_t OUTPUT_BUFFER_SIZE = 1024 * 1024; // 1 MB
thread_local std::vector<char> output_buffer;      // sized once to CAP; manual length
thread_local size_t output_len = 0;

static inline void out_flush() {
    if (output_len == 0) return;
    std::lock_guard<std::mutex> lock(output_mutex);
    fwrite(output_buffer.data(), 1, output_len, stdout);
    output_len = 0;
}

static inline void out_write_direct(const char* p, size_t len, bool add_nl) {
    std::lock_guard<std::mutex> lock(output_mutex);
    fwrite(p, 1, len, stdout);
    if (add_nl) fputc('\n', stdout);
}

constexpr size_t ZMM_SIZE = 64;

const char* RED   = "\033[41m";
const char* RESET = "\033[0m";
constexpr size_t RED_LEN   = 5; // "\033[41m"
constexpr size_t RESET_LEN = 4; // "\033[0m"

bool use_color = isatty(STDOUT_FILENO);

// Cross-platform CPU count
static inline int get_cpu_count() {
#ifdef __linux__
    return get_nprocs();
#elif defined(__APPLE__)
    int count;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.ncpu", &count, &size, nullptr, 0) == 0) {
        return count;
    }
    return std::thread::hardware_concurrency();
#else
    return std::thread::hardware_concurrency();
#endif
}

// ---------------------------------------------------------------------------
// Byte-frequency table (higher = more common in text/log data). Used to pick
// the two rarest bytes of the pattern for the SIMD prefilter. Bytes not listed
// (control chars, high-bit bytes typical of ids/hex/binary) stay 0 == rarest,
// which is exactly what we want to filter on.
// ---------------------------------------------------------------------------
static std::array<uint8_t, 256> g_freq;

static void init_freq_table() {
    g_freq.fill(0);
    // Rough ordering, most-common -> less-common. Exact values are not critical;
    // the goal is only to avoid selecting very common bytes (space, e, t, ...).
    const char* common = " etaoinsrhldcumfpgwybvkxjqz"
                         "ETAOINSRHLDCUMFPGWYBVKXJQZ"
                         "0123456789"
                         ".,:/-_=\"'()[]{}<>|;@#&*+\\%$!?\t\r";
    uint8_t rank = 255;
    for (const char* c = common; *c; ++c) {
        uint8_t b = static_cast<uint8_t>(*c);
        if (g_freq[b] == 0) {           // first (most common) occurrence wins
            g_freq[b] = rank;
            if (rank > 1) --rank;
        }
    }
    g_freq[static_cast<uint8_t>('\n')] = 255; // never filter on newline
}

// Precomputed pattern descriptor.
struct Pattern {
    const char* p = nullptr;
    size_t m = 0;
    size_t i1 = 0; // offset of rarest byte
    size_t i2 = 0; // offset of second-rarest byte (== i1 when m == 1)
};

static Pattern make_pattern(const char* p, size_t m) {
    Pattern pat{p, m, 0, 0};
    if (m == 0) return pat;
    // rarest byte
    for (size_t k = 1; k < m; ++k)
        if (g_freq[(uint8_t)p[k]] < g_freq[(uint8_t)pat.p[pat.i1]]) pat.i1 = k;
    if (m == 1) { pat.i2 = pat.i1; return pat; }
    // second-rarest byte at a different offset
    pat.i2 = (pat.i1 == 0) ? 1 : 0;
    for (size_t k = 0; k < m; ++k)
        if (k != pat.i1 && g_freq[(uint8_t)p[k]] < g_freq[(uint8_t)p[pat.i2]]) pat.i2 = k;
    return pat;
}

// ---------------------------------------------------------------------------
// Line boundary helpers (operate within a thread's [lo, hi) range).
// ---------------------------------------------------------------------------
static inline size_t line_begin(const char* data, size_t s, size_t lo) {
    while (s > lo && data[s - 1] != '\n') --s;
    return s;
}
static inline size_t line_end(const char* data, size_t from, size_t hi) {
    while (from < hi && data[from] != '\n') ++from;
    return from; // index of '\n', or hi
}

// ---------------------------------------------------------------------------
// Emit one matching line [ls, le) (le exclusive, points at '\n' or range end).
// ---------------------------------------------------------------------------
static inline void emit_plain(const char* data, size_t ls, size_t le) {
    size_t len = le - ls;
    size_t need = len + 1;
    if (need > OUTPUT_BUFFER_SIZE) { out_flush(); out_write_direct(data + ls, len, true); return; }
    if (output_len + need > OUTPUT_BUFFER_SIZE) out_flush();
    char* buf = output_buffer.data() + output_len;
    memcpy(buf, data + ls, len);
    buf[len] = '\n';
    output_len += need;
}

static void emit_color(const char* data, size_t ls, size_t le, const Pattern& pat) {
    thread_local std::string cline;
    cline.clear();
    const size_t m = pat.m;
    size_t prev = ls, pos = ls;
    while (pos + m <= le) {
        if (data[pos + pat.i1] == pat.p[pat.i1] &&
            data[pos + pat.i2] == pat.p[pat.i2] &&
            memcmp(data + pos, pat.p, m) == 0) {
            cline.append(data + prev, pos - prev);
            cline.append(RED, RED_LEN);
            cline.append(data + pos, m);
            cline.append(RESET, RESET_LEN);
            prev = pos + m;
            pos += m;
        } else {
            ++pos;
        }
    }
    cline.append(data + prev, le - prev);
    cline.push_back('\n');

    size_t len = cline.size();
    if (len > OUTPUT_BUFFER_SIZE) { out_flush(); out_write_direct(cline.data(), len, false); return; }
    if (output_len + len > OUTPUT_BUFFER_SIZE) out_flush();
    memcpy(output_buffer.data() + output_len, cline.data(), len);
    output_len += len;
}

static inline void emit_line(const char* data, size_t ls, size_t le, const Pattern& pat) {
    if (use_color) emit_color(data, ls, le, pat);
    else           emit_plain(data, ls, le);
}

// ---------------------------------------------------------------------------
// Single-pass search over a thread's line-aligned range [start, end).
// Two-rare-byte AVX-512 prefilter; line boundaries derived lazily around each
// verified match. After emitting a line, the scan jumps to the line end, so no
// line is ever emitted twice and dense lines aren't re-scanned.
// ---------------------------------------------------------------------------
static void process_range(const char* data, size_t start, size_t end, const Pattern& pat) {
    const size_t m = pat.m;
    if (m == 0 || end < start || (end - start) < m) return;

    if (output_buffer.size() != OUTPUT_BUFFER_SIZE) output_buffer.resize(OUTPUT_BUFFER_SIZE);

    const __m512i v1 = _mm512_set1_epi8(pat.p[pat.i1]);
    const __m512i v2 = _mm512_set1_epi8(pat.p[pat.i2]);

    size_t pos = start;
    // Main SIMD loop: both 64-byte loads and full verify stay in-bounds while
    // pos + (m-1) + 64 <= end.
    while (pos + (m - 1) + ZMM_SIZE <= end) {
        __mmask64 mask =
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void*)(data + pos + pat.i1)), v1) &
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void*)(data + pos + pat.i2)), v2);

        bool jumped = false;
        while (mask) {
            size_t s = pos + (size_t)__builtin_ctzll(mask);
            if (memcmp(data + s, pat.p, m) == 0) {
                size_t ls = line_begin(data, s, start);
                size_t le = line_end(data, s + m, end);
                emit_line(data, ls, le, pat);
                pos = le;          // skip the rest of this line
                jumped = true;
                break;
            }
            mask &= mask - 1;
        }
        if (!jumped) pos += ZMM_SIZE;
    }

    // Scalar tail for the final < (m-1)+64 bytes.
    while (pos + m <= end) {
        if (data[pos + pat.i1] == pat.p[pat.i1] &&
            data[pos + pat.i2] == pat.p[pat.i2] &&
            memcmp(data + pos, pat.p, m) == 0) {
            size_t ls = line_begin(data, pos, start);
            size_t le = line_end(data, pos + m, end);
            emit_line(data, ls, le, pat);
            pos = le;
        } else {
            ++pos;
        }
    }

    out_flush();
}

// ---------------------------------------------------------------------------
// RAII guards
// ---------------------------------------------------------------------------
struct FdGuard {
    int fd;
    explicit FdGuard(int fd) : fd(fd) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

struct MmapGuard {
    void* addr;
    size_t len;
    MmapGuard(void* addr, size_t len) : addr(addr), len(len) {}
    ~MmapGuard() { if (addr != MAP_FAILED) munmap(addr, len); }
    MmapGuard(const MmapGuard&) = delete;
    MmapGuard& operator=(const MmapGuard&) = delete;
};

void search_file(const std::string& filename, const Pattern& pat,
                 bool use_internal_parallelization = true) {
    int raw_fd = open(filename.c_str(), O_RDONLY);
    if (raw_fd == -1) return;
    FdGuard fd_guard(raw_fd);

    struct stat sb;
    if (fstat(raw_fd, &sb) == -1 || sb.st_size <= 0) return;
    const size_t file_size = static_cast<size_t>(sb.st_size);

    char* data = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, raw_fd, 0));
    if (data == MAP_FAILED) return;
    MmapGuard mmap_guard(data, file_size);
#ifdef __linux__
    posix_fadvise(raw_fd, 0, file_size, POSIX_FADV_SEQUENTIAL);
    madvise(data, file_size, MADV_SEQUENTIAL);
#endif

    int num_cores = get_cpu_count();
    size_t num_threads = static_cast<size_t>(num_cores);
    bool parallelize = use_internal_parallelization &&
                       file_size >= 100 * 1024 * 1024 &&
                       num_threads > 1;

    if (!parallelize) {
        process_range(data, 0, file_size, pat);
        return;
    }

    // Large files: line-aligned chunks, one per core.
    size_t chunk_size = file_size / num_threads;
    std::vector<std::thread> threads;
    std::vector<size_t> boundaries;
    boundaries.push_back(0);
    for (size_t t = 1; t < num_threads; ++t) {
        size_t target = t * chunk_size;
        while (target < file_size && data[target] != '\n') ++target;
        if (target < file_size) boundaries.push_back(target + 1);
    }
    boundaries.push_back(file_size);

    for (size_t t = 0; t < boundaries.size() - 1; ++t) {
        size_t start = boundaries[t];
        size_t end = boundaries[t + 1];
        threads.emplace_back([data, start, end, &pat]() {
            process_range(data, start, end, pat);
        });
    }
    for (auto& th : threads) th.join();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: fff <query> <file1> [file2 ...]" << std::endl;
        return 1;
    }

    init_freq_table();

    std::string query = argv[1];
    std::vector<std::string> files(argv + 2, argv + argc);
    size_t num_files = files.size();

    Pattern pat = make_pattern(query.c_str(), query.length());
    if (pat.m == 0) return 0; // empty query matches nothing (preserves prior behavior)
    // Line-oriented semantics: a pattern containing '\n' can never match within a
    // single line, so it matches nothing (matches the original's per-line search).
    if (query.find('\n') != std::string::npos) return 0;

    if (num_files == 1) {
        search_file(files[0], pat, true);
        return 0;
    }

    // Multi-file: unified chunk-based distribution across all cores.
    int num_cores = get_cpu_count();
    size_t num_threads = static_cast<size_t>(num_cores);

    struct FileMapping { char* data = nullptr; size_t size = 0; int fd = -1; };
    std::vector<FileMapping> mappings;
    mappings.reserve(num_files);
    size_t total_size = 0;

    for (const auto& filename : files) {
        FileMapping fm;
        fm.fd = open(filename.c_str(), O_RDONLY);
        if (fm.fd == -1) { mappings.push_back(fm); continue; }

        struct stat sb;
        if (fstat(fm.fd, &sb) == -1 || sb.st_size <= 0) {
            close(fm.fd); fm.fd = -1; mappings.push_back(fm); continue;
        }
        fm.size = static_cast<size_t>(sb.st_size);

        fm.data = static_cast<char*>(mmap(nullptr, fm.size, PROT_READ, MAP_PRIVATE, fm.fd, 0));
        if (fm.data == MAP_FAILED) {
            fm.data = nullptr; close(fm.fd); fm.fd = -1; mappings.push_back(fm); continue;
        }
#ifdef __linux__
        posix_fadvise(fm.fd, 0, fm.size, POSIX_FADV_SEQUENTIAL);
        madvise(fm.data, fm.size, MADV_SEQUENTIAL);
#endif
        total_size += fm.size;
        mappings.push_back(fm);
    }

    struct WorkChunk { const char* data; size_t start; size_t end; };
    std::vector<WorkChunk> chunks;

    size_t target_chunk_bytes = (num_threads > 0) ? total_size / num_threads : total_size;
    if (target_chunk_bytes < 1024 * 1024) target_chunk_bytes = 1024 * 1024;

    for (size_t fi = 0; fi < mappings.size(); ++fi) {
        auto& fm = mappings[fi];
        if (!fm.data || fm.size == 0) continue;

        size_t file_chunks = std::max<size_t>(1, (fm.size + target_chunk_bytes / 2) / target_chunk_bytes);
        size_t chunk_size = fm.size / file_chunks;

        size_t prev = 0;
        for (size_t c = 1; c < file_chunks; ++c) {
            size_t target = c * chunk_size;
            while (target < fm.size && fm.data[target] != '\n') ++target;
            if (target < fm.size) { chunks.push_back({fm.data, prev, target + 1}); prev = target + 1; }
        }
        chunks.push_back({fm.data, prev, fm.size});
    }

    std::atomic<size_t> work_queue_index{0};
    size_t actual_threads = std::min(num_threads, chunks.size());
    std::vector<std::thread> threads;

    auto worker = [&chunks, &pat, &work_queue_index]() {
        while (true) {
            size_t ci = work_queue_index.fetch_add(1, std::memory_order_relaxed);
            if (ci >= chunks.size()) break;
            auto& c = chunks[ci];
            process_range(c.data, c.start, c.end, pat);
        }
    };

    for (size_t t = 0; t < actual_threads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    for (auto& fm : mappings) {
        if (fm.data) munmap(fm.data, fm.size);
        if (fm.fd >= 0) close(fm.fd);
    }
    return 0;
}
