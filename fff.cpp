#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#include <sys/syscall.h>
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
#include <cstdio>
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
    const char* common = " etaoinsrhldcumfpgwybvkxjqz"
                         "ETAOINSRHLDCUMFPGWYBVKXJQZ"
                         "0123456789"
                         ".,:/-_=\"'()[]{}<>|;@#&*+\\%$!?\t\r";
    uint8_t rank = 255;
    for (const char* c = common; *c; ++c) {
        uint8_t b = static_cast<uint8_t>(*c);
        if (g_freq[b] == 0) { g_freq[b] = rank; if (rank > 1) --rank; }
    }
    g_freq[static_cast<uint8_t>('\n')] = 255; // never filter on newline
}

struct Pattern {
    const char* p = nullptr;
    size_t m = 0;
    size_t i1 = 0; // offset of rarest byte
    size_t i2 = 0; // offset of second-rarest byte (== i1 when m == 1)
};

static Pattern make_pattern(const char* p, size_t m) {
    Pattern pat{p, m, 0, 0};
    if (m == 0) return pat;
    for (size_t k = 1; k < m; ++k)
        if (g_freq[(uint8_t)p[k]] < g_freq[(uint8_t)pat.p[pat.i1]]) pat.i1 = k;
    if (m == 1) { pat.i2 = pat.i1; return pat; }
    pat.i2 = (pat.i1 == 0) ? 1 : 0;
    for (size_t k = 0; k < m; ++k)
        if (k != pat.i1 && g_freq[(uint8_t)p[k]] < g_freq[(uint8_t)p[pat.i2]]) pat.i2 = k;
    return pat;
}

// ---------------------------------------------------------------------------
// Line boundary helpers (operate within a thread's [lo, hi) range).
// ---------------------------------------------------------------------------
static inline size_t line_begin(const char* data, size_t s, size_t lo) {
    const __m512i newline = _mm512_set1_epi8('\n');
    while (s - lo >= ZMM_SIZE) {
        const auto mask = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(data + s - ZMM_SIZE), newline);
        if (mask) return s - ZMM_SIZE + 64 - __builtin_clzll(mask);
        s -= ZMM_SIZE;
    }
    while (s > lo && data[s - 1] != '\n') --s;
    return s;
}
static inline size_t line_end(const char* data, size_t from, size_t hi) {
    const char* nl = static_cast<const char*>(memchr(data + from, '\n', hi - from));
    return nl ? static_cast<size_t>(nl - data) : hi;
}

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
// ---------------------------------------------------------------------------
static void process_range(const char* data, size_t start, size_t end, const Pattern& pat) {
    const size_t m = pat.m;
    if (m == 0 || end < start || (end - start) < m) return;

    if (output_buffer.size() != OUTPUT_BUFFER_SIZE) output_buffer.resize(OUTPUT_BUFFER_SIZE);

    const __m512i v1 = _mm512_set1_epi8(pat.p[pat.i1]);
    const __m512i v2 = _mm512_set1_epi8(pat.p[pat.i2]);

    size_t pos = start;
    while (end - pos >= (m - 1) + ZMM_SIZE) {
        __mmask64 mask =
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void*)(data + pos + pat.i1)), v1) &
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void*)(data + pos + pat.i2)), v2);
        // Batch the no-candidate case without extra loads on dense blocks.
        // All loads stay inside the range, even for filter offsets at m - 1.
        if (!mask && end - pos >= (m - 1) + 4 * ZMM_SIZE) {
            __mmask64 combined = 0;
            for (size_t block = 1; block < 4; ++block) {
                const size_t at = pos + block * ZMM_SIZE;
                combined |= _mm512_cmpeq_epi8_mask(
                    _mm512_loadu_si512(data + at + pat.i1), v1) &
                    _mm512_cmpeq_epi8_mask(
                    _mm512_loadu_si512(data + at + pat.i2), v2);
            }
            if (!combined) { pos += 4 * ZMM_SIZE; continue; }
        }

        bool jumped = false;
        while (mask) {
            size_t s = pos + (size_t)__builtin_ctzll(mask);
            if (memcmp(data + s, pat.p, m) == 0) {
                size_t ls = line_begin(data, s, start);
                size_t le = line_end(data, s + m, end);
                emit_line(data, ls, le, pat);
                pos = le;
                jumped = true;
                break;
            }
            mask &= mask - 1;
        }
        if (!jumped) pos += ZMM_SIZE;
    }

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

// ---------------------------------------------------------------------------
// Work chunk: a line-aligned [start, end) region of one mapped file.
// ---------------------------------------------------------------------------
struct WorkChunk { const char* data; size_t start; size_t end; };

// Split a mapped file into line-aligned chunks of ~target_chunk_bytes.
static void build_chunks(const char* data, size_t size, size_t target_chunk_bytes,
                         std::vector<WorkChunk>& chunks) {
    if (!data || size == 0) return;
    size_t file_chunks = std::max<size_t>(1, (size + target_chunk_bytes / 2) / target_chunk_bytes);
    size_t chunk_size = size / file_chunks;
    size_t prev = 0;
    for (size_t c = 1; c < file_chunks; ++c) {
        size_t target = c * chunk_size;
        while (target < size && data[target] != '\n') ++target;
        if (target < size) { chunks.push_back({data, prev, target + 1}); prev = target + 1; }
    }
    chunks.push_back({data, prev, size});
}

// Flat lock-free work queue across all chunks (used when NUMA is unavailable
// or not beneficial). Identical distribution to the previous multi-file path.
static void run_flat(std::vector<WorkChunk>& chunks, size_t num_threads, const Pattern& pat) {
    std::atomic<size_t> work_index{0};
    size_t actual = std::min(num_threads, chunks.size());
    std::vector<std::thread> threads;
    auto worker = [&chunks, &pat, &work_index]() {
        for (;;) {
            size_t ci = work_index.fetch_add(1, std::memory_order_relaxed);
            if (ci >= chunks.size()) break;
            process_range(chunks[ci].data, chunks[ci].start, chunks[ci].end, pat);
        }
    };
    for (size_t t = 0; t < actual; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();
}

// ---------------------------------------------------------------------------
// NUMA-aware distribution (Linux, dependency-free: sysfs + move_pages/affinity
// syscalls). Each chunk is assigned to the node whose memory physically holds
// it; each node's cores drain only their own queue (no cross-node stealing,
// since extra remote-reading cores add contention, not bandwidth, on a
// memory-bound scan). Falls back to the flat queue whenever NUMA can't help.
// ---------------------------------------------------------------------------
#ifdef __linux__
constexpr int NUMA_MAX_NODES = 64;

struct NumaTopo {
    std::vector<int> node_ids;          // worker nodes (online, >=1 cpu)
    std::vector<cpu_set_t> masks;       // parallel to node_ids
    std::vector<int> cpu_count;         // parallel to node_ids
    bool usable() const {
        return node_ids.size() >= 2 && node_ids.size() <= (size_t)NUMA_MAX_NODES;
    }
};

static void parse_int_list(const char* s, std::vector<int>& out) {
    while (*s) {
        while (*s == ',' || *s == ' ' || *s == '\n' || *s == '\t') ++s;
        if (*s < '0' || *s > '9') break;
        int a = 0;
        while (*s >= '0' && *s <= '9') { a = a * 10 + (*s - '0'); ++s; }
        int b = a;
        if (*s == '-') { ++s; b = 0; while (*s >= '0' && *s <= '9') { b = b * 10 + (*s - '0'); ++s; } }
        for (int v = a; v <= b; ++v) out.push_back(v);
    }
}

static bool read_sysfs(const char* path, char* buf, size_t n) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = '\0';
    fclose(f);
    return r > 0;
}

static NumaTopo detect_numa() {
    NumaTopo t;
    char buf[8192];
    if (!read_sysfs("/sys/devices/system/node/online", buf, sizeof(buf))) return t;
    std::vector<int> online;
    parse_int_list(buf, online);
    for (int nd : online) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", nd);
        if (!read_sysfs(path, buf, sizeof(buf))) continue;
        std::vector<int> cpus;
        parse_int_list(buf, cpus);
        if (cpus.empty()) continue; // CPU-less node (e.g. PMEM/GPU): never a worker
        cpu_set_t set; CPU_ZERO(&set);
        for (int c : cpus) if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
        t.node_ids.push_back(nd);
        t.masks.push_back(set);
        t.cpu_count.push_back((int)cpus.size());
    }
    return t;
}

static long move_pages_query(int count, void** pages, int* status) {
    return syscall(SYS_move_pages, 0, (unsigned long)count, pages,
                   (const int*)nullptr, status, 0);
}

static void run_numa(std::vector<WorkChunk>& chunks, const NumaTopo& topo, const Pattern& pat) {
    const int NN = (int)topo.node_ids.size();
    int max_node = 0;
    for (int nd : topo.node_ids) max_node = std::max(max_node, nd);
    std::vector<int> id2idx(max_node + 1, -1);
    for (int i = 0; i < NN; ++i) id2idx[topo.node_ids[i]] = i;

    // Sample up to 3 pages per chunk and query their physical node in one pass.
    const size_t C = chunks.size();
    std::vector<void*> pages;
    pages.reserve(C * 3);
    for (const auto& c : chunks) {
        size_t len = c.end - c.start;
        pages.push_back((void*)(c.data + c.start));
        pages.push_back((void*)(c.data + c.start + len / 2));
        pages.push_back((void*)(c.data + (c.end > 0 ? c.end - 1 : c.start)));
    }
    std::vector<int> status(pages.size(), -1);
    const int BATCH = 512;
    for (size_t off = 0; off < pages.size(); off += BATCH) {
        int cnt = (int)std::min((size_t)BATCH, pages.size() - off);
        move_pages_query(cnt, pages.data() + off, status.data() + off);
    }

    // Assign each chunk to a worker node: majority vote of its samples; chunks
    // whose pages are unknown (not resident) or on a non-worker node are spread
    // round-robin so every chunk lands on a node that has cores.
    std::vector<std::vector<WorkChunk>> node_chunks(NN);
    size_t rr = 0;
    for (size_t i = 0; i < C; ++i) {
        int votes[NUMA_MAX_NODES] = {0};
        bool any = false;
        for (int k = 0; k < 3; ++k) {
            int st = status[i * 3 + k];
            if (st >= 0 && st <= max_node && id2idx[st] >= 0) { votes[id2idx[st]]++; any = true; }
        }
        int dest;
        if (any) {
            int best = 0;
            for (int j = 1; j < NN; ++j) if (votes[j] > votes[best]) best = j;
            dest = best;
        } else {
            dest = (int)(rr++ % NN);
        }
        node_chunks[dest].push_back(chunks[i]);
    }

    std::vector<std::atomic<size_t>> node_index(NN);
    for (int i = 0; i < NN; ++i) node_index[i].store(0, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    for (int ni = 0; ni < NN; ++ni) {
        if (node_chunks[ni].empty()) continue; // no work for this node -> its cores idle
        cpu_set_t mask = topo.masks[ni];
        int nthreads = topo.cpu_count[ni];
        for (int tI = 0; tI < nthreads; ++tI) {
            threads.emplace_back([ni, &node_chunks, &node_index, &pat, mask]() {
                cpu_set_t m = mask;
                sched_setaffinity(0, sizeof(m), &m); // keep this thread on its node
                auto& q = node_chunks[ni];
                auto& idx = node_index[ni];
                for (;;) {
                    size_t i = idx.fetch_add(1, std::memory_order_relaxed);
                    if (i >= q.size()) break;
                    process_range(q[i].data, q[i].start, q[i].end, pat);
                }
            });
        }
    }
    for (auto& th : threads) th.join();
}
#endif // __linux__

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------
struct FileMapping { char* data = nullptr; size_t size = 0; int fd = -1; };

static FileMapping map_file(const std::string& filename) {
    FileMapping fm;
    fm.fd = open(filename.c_str(), O_RDONLY);
    if (fm.fd == -1) return fm;
    struct stat sb;
    if (fstat(fm.fd, &sb) == -1 || sb.st_size <= 0) { close(fm.fd); fm.fd = -1; return fm; }
    fm.size = static_cast<size_t>(sb.st_size);
    fm.data = static_cast<char*>(mmap(nullptr, fm.size, PROT_READ, MAP_PRIVATE, fm.fd, 0));
    if (fm.data == MAP_FAILED) { fm.data = nullptr; close(fm.fd); fm.fd = -1; return fm; }
#ifdef __linux__
    posix_fadvise(fm.fd, 0, fm.size, POSIX_FADV_SEQUENTIAL);
    madvise(fm.data, fm.size, MADV_SEQUENTIAL);
#endif
    return fm;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: fff <query> <file1> [file2 ...]" << std::endl;
        return 1;
    }

    init_freq_table();

    std::string query = argv[1];
    std::vector<std::string> files(argv + 2, argv + argc);

    Pattern pat = make_pattern(query.c_str(), query.length());
    if (pat.m == 0) return 0; // empty query matches nothing (preserves prior behavior)
    // Line-oriented semantics: a pattern containing '\n' can never match within a
    // single line, so it matches nothing (matches the original's per-line search).
    if (query.find('\n') != std::string::npos) return 0;

    int num_cores = get_cpu_count();
    size_t num_threads = num_cores > 0 ? static_cast<size_t>(num_cores) : 1;

    // Map every file up front (works for the single-file case too).
    std::vector<FileMapping> mappings;
    mappings.reserve(files.size());
    size_t total_size = 0;
    for (const auto& f : files) {
        FileMapping fm = map_file(f);
        if (fm.data) total_size += fm.size;
        mappings.push_back(fm);
    }

    if (total_size == 0) { /* nothing readable */ }
    else if (total_size < 100 * 1024 * 1024 || num_threads <= 1) {
        // Small aggregate size (or single core): process each file sequentially.
        for (auto& fm : mappings)
            if (fm.data) process_range(fm.data, 0, fm.size, pat);
    } else {
        // Build line-aligned chunks across all files. Aim for several chunks per
        // core so load balances within a node; floor keeps per-chunk overhead low.
        size_t target = total_size / (num_threads * 4);
        if (target < 8 * 1024 * 1024) target = 8 * 1024 * 1024;
        std::vector<WorkChunk> chunks;
        for (auto& fm : mappings)
            if (fm.data) build_chunks(fm.data, fm.size, target, chunks);

#ifdef __linux__
        NumaTopo topo = detect_numa();
        if (topo.usable()) run_numa(chunks, topo, pat);
        else               run_flat(chunks, num_threads, pat);
#else
        run_flat(chunks, num_threads, pat);
#endif
    }

    for (auto& fm : mappings) {
        if (fm.data) munmap(fm.data, fm.size);
        if (fm.fd >= 0) close(fm.fd);
    }
    return 0;
}
