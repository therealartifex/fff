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
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <thread>
#include <mutex>

#include <immintrin.h>

// Global mutex for synchronized output
std::mutex output_mutex;

// File queue for multi-file parallelization
std::atomic<size_t> file_queue_index{0};

// Thread-local reusable match positions vector
thread_local std::vector<size_t> match_positions;

// Thread-local output buffer for batched writes
thread_local std::vector<char> output_buffer;
constexpr size_t OUTPUT_BUFFER_SIZE = 1024 * 1024; // 1MB

static inline void flush_output_buffer() {
    if (output_buffer.empty()) return;
    std::lock_guard<std::mutex> lock(output_mutex);
    fwrite(output_buffer.data(), 1, output_buffer.size(), stdout);
    output_buffer.clear();
}

constexpr size_t ZMM_SIZE = 64;

const char* RED = "\033[41m";
const char* RESET = "\033[0m";

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

// BMH (Boyer-Moore-Horspool) for longer patterns with precomputed table
static void search_bmh(const char* hay, size_t n, const char* pat, size_t m,
                       const uint32_t* shift, std::vector<size_t>& res) {
    res.clear();
    if (m == 0 || n < m) return;

    size_t i = 0;
    while (i + m <= n) {
        if (memcmp(hay + i, pat, m) == 0) {
            res.push_back(i);
            i += m;
        } else {
            i += shift[static_cast<unsigned char>(hay[i + m - 1])];
        }
    }
}

// Build BMH shift table once
static inline void build_bmh_table(const char* pat, size_t m, uint32_t* shift) {
    for (int i = 0; i < 256; ++i) shift[i] = m;
    for (size_t i = 0; i + 1 < m; ++i) {
        shift[static_cast<unsigned char>(pat[i])] = m - 1 - i;
    }
}

// Fast search for short patterns with AVX-512 prefilter
static void search_short_fast(const char* hay, size_t n, const char* pat, size_t m,
                              std::vector<size_t>& res) {
    res.clear();
    if (m == 0 || n < m) return;

    const unsigned char first = static_cast<unsigned char>(pat[0]);
    __m512i firstv = _mm512_set1_epi8(static_cast<char>(first));

    uint64_t pat_prefix = 0;
    memcpy(&pat_prefix, pat, std::min<size_t>(m, sizeof(uint64_t)));

    size_t pos = 0;
    while (pos <= n - m) {
        size_t rel = ZMM_SIZE;
        if (pos + ZMM_SIZE <= n) {
            __mmask64 mask = _mm512_cmpeq_epi8_mask(
                _mm512_loadu_si512(reinterpret_cast<const __m512i*>(hay + pos)), firstv);
            if (mask) rel = __builtin_ctzll(mask);
        } else {
            const char* found = static_cast<const char*>(memchr(hay + pos, first, n - pos));
            rel = found ? static_cast<size_t>(found - (hay + pos)) : (n - pos);
        }
        if (rel >= (n - pos)) break;
        pos += rel;
        if (pos > n - m) break;

        uint64_t w = 0;
        size_t to_copy = std::min<size_t>(m, sizeof(uint64_t));
        memcpy(&w, hay + pos, to_copy);
        if (w == pat_prefix) {
            if (memcmp(hay + pos, pat, m) == 0) {
                res.push_back(pos);
                pos += m;
            } else {
                ++pos;
            }
        } else {
            ++pos;
        }
    }
}

// Hybrid search dispatcher
static void find_all_matches(const char* data, size_t len, const char* query, size_t q_len,
                             const uint32_t* bmh_shift, std::vector<size_t>& matches) {
    if (q_len == 0 || len < q_len) { matches.clear(); return; }
    if (q_len >= 8) {
        search_bmh(data, len, query, q_len, bmh_shift, matches);
    } else {
        search_short_fast(data, len, query, q_len, matches);
    }
}

// Fused single-pass: find newlines in chunk and return offsets
inline void find_newlines_in_chunk(const char* data, size_t chunk_start, size_t chunk_size, 
                                    std::vector<size_t>& newlines) {
    newlines.clear();
    const __m512i nl_vec = _mm512_set1_epi8('\n');
    size_t pos = 0;
    
    // Process 64-byte chunks with AVX-512
    while (pos + ZMM_SIZE <= chunk_size) {
        __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + chunk_start + pos));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, nl_vec);
        
        while (mask) {
            unsigned long bit = __builtin_ctzll(mask);
            newlines.push_back(chunk_start + pos + bit);
            mask &= mask - 1;
        }
        pos += ZMM_SIZE;
    }
    
    // Handle remaining bytes in chunk
    while (pos < chunk_size) {
        if (data[chunk_start + pos] == '\n') {
            newlines.push_back(chunk_start + pos);
        }
        ++pos;
    }
}

// Process and output a single line with matches
static void process_line(const char* data, size_t line_start, size_t line_len, 
                         const char* query, size_t q_len, const uint32_t* bmh_shift) {
    if (line_len == 0) return;
    
    find_all_matches(data + line_start, line_len, query, q_len, bmh_shift, match_positions);
    if (match_positions.empty()) return;
    
    // Calculate size upper bound (matches already in order from left-to-right scan)
    constexpr size_t RED_LEN = 5;   // "\033[41m"
    constexpr size_t RESET_LEN = 4; // "\033[0m"
    size_t max_size = line_len + 1; // line + newline
    if (use_color) {
        max_size += match_positions.size() * (RED_LEN + RESET_LEN);
    }
    
    // Write directly into output_buffer — no intermediate copy
    size_t write_start = output_buffer.size();
    output_buffer.resize(write_start + max_size);
    char* buf = output_buffer.data() + write_start;
    size_t pos = 0;
    
    size_t prev = line_start;
    size_t line_end_pos = line_start + line_len;
    
    for (size_t rel_pos : match_positions) {
        size_t s = line_start + rel_pos;
        size_t l = q_len;
        if (s < prev || s + l > line_end_pos) continue;
        
        if (s > prev) {
            size_t len = s - prev;
            memcpy(buf + pos, data + prev, len);
            pos += len;
        }
        
        if (use_color) {
            memcpy(buf + pos, RED, RED_LEN);
            pos += RED_LEN;
        }
        memcpy(buf + pos, data + s, l);
        pos += l;
        if (use_color) {
            memcpy(buf + pos, RESET, RESET_LEN);
            pos += RESET_LEN;
        }
        
        prev = s + l;
    }
    
    if (prev < line_end_pos) {
        size_t len = line_end_pos - prev;
        memcpy(buf + pos, data + prev, len);
        pos += len;
    }
    buf[pos++] = '\n';
    
    // Trim to actual size written
    output_buffer.resize(write_start + pos);
    
    if (output_buffer.size() >= OUTPUT_BUFFER_SIZE) {
        flush_output_buffer();
    }
}

// Process a range of the file (thread worker)
static void process_file_range(const char* data, size_t start, size_t end, 
                               const std::string& query, const uint32_t* bmh_shift) {
    size_t q_len = query.length();
    const size_t CHUNK_SIZE = 16 * 1024 * 1024;
    
    // Pre-allocate output buffer to avoid repeated reallocations
    output_buffer.reserve(OUTPUT_BUFFER_SIZE + 65536);
    
    std::vector<size_t> newlines;
    newlines.reserve(CHUNK_SIZE / 80);
    
    size_t file_pos = start;
    size_t line_start = start;
    
    // Process assigned range in chunks
    while (file_pos < end) {
        size_t chunk_size = std::min(CHUNK_SIZE, end - file_pos);
        
        find_newlines_in_chunk(data, file_pos, chunk_size, newlines);
        
        for (size_t nl_offset : newlines) {
            if (nl_offset >= end) break;
            size_t line_len = nl_offset - line_start;
            process_line(data, line_start, line_len, query.c_str(), q_len, bmh_shift);
            line_start = nl_offset + 1;
        }
        
        file_pos += chunk_size;
    }
    
    // Handle final line in this range
    if (line_start < end) {
        size_t line_len = end - line_start;
        process_line(data, line_start, line_len, query.c_str(), q_len, bmh_shift);
    }
    
    // Flush remaining buffered output
    flush_output_buffer();
}

// RAII wrapper for file descriptors
struct FdGuard {
    int fd;
    explicit FdGuard(int fd) : fd(fd) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

// RAII wrapper for mmap regions
struct MmapGuard {
    void* addr;
    size_t len;
    MmapGuard(void* addr, size_t len) : addr(addr), len(len) {}
    ~MmapGuard() { if (addr != MAP_FAILED) munmap(addr, len); }
    MmapGuard(const MmapGuard&) = delete;
    MmapGuard& operator=(const MmapGuard&) = delete;
};

void search_file(const std::string& filename, const std::string& query, bool use_internal_parallelization = true) {
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
#endif

    size_t q_len = query.length();
    
    // Build BMH shift table once (for patterns >= 8 bytes)
    uint32_t bmh_shift[256];
    if (q_len >= 8) {
        build_bmh_table(query.c_str(), q_len, bmh_shift);
    }
    
    // Decide on parallelization strategy
    int num_cores = get_cpu_count();
    size_t num_threads = std::min(static_cast<size_t>(16), static_cast<size_t>(num_cores));
    bool parallelize = use_internal_parallelization && 
                       file_size >= 100 * 1024 * 1024 && 
                       num_threads > 1;
    
    if (!parallelize) {
        // Sequential processing (small files or when called from work queue)
        process_file_range(data, 0, file_size, query, bmh_shift);
    } else {
        // Large files: split into line-aligned chunks
        size_t chunk_size = file_size / num_threads;
        std::vector<std::thread> threads;
        std::vector<size_t> boundaries;
        boundaries.push_back(0);
        
        // Find line-aligned boundaries
        for (size_t t = 1; t < num_threads; ++t) {
            size_t target = t * chunk_size;
            while (target < file_size && data[target] != '\n') {
                ++target;
            }
            if (target < file_size) {
                boundaries.push_back(target + 1);
            }
        }
        boundaries.push_back(file_size);
        
        // Launch threads
        for (size_t t = 0; t < boundaries.size() - 1; ++t) {
            size_t start = boundaries[t];
            size_t end = boundaries[t+1];
            threads.emplace_back([data, start, end, &query, bmh_shift]() {
                process_file_range(data, start, end, query, bmh_shift);
            });
        }
        
        for (auto& th : threads) th.join();
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: fff <query> <file1> [file2 ...]" << std::endl;
        return 1;
    }

    std::string query = argv[1];
    std::vector<std::string> files(argv + 2, argv + argc);
    
    int num_cores = get_cpu_count();
    size_t num_files = files.size();
    
    // Strategy: Choose parallelization approach based on workload
    // - Few large files (files <= cores/4): Use internal file parallelization
    // - Many files (files > cores/4): Use work queue across files
    
    if (num_files == 1) {
        // Single file: use internal parallelization (16 threads)
        search_file(files[0], query, true);
    } else if (num_files <= static_cast<size_t>(num_cores) / 4) {
        // Few files: process sequentially with internal parallelization
        for (const auto& file : files) {
            search_file(file, query, true);
        }
    } else {
        // Many files: use work queue parallelization
        file_queue_index.store(0, std::memory_order_relaxed);
        size_t num_threads = std::min(static_cast<size_t>(num_cores), num_files);
        std::vector<std::thread> threads;
        
        // Worker function: pull files from queue (lock-free)
        auto worker = [&files, &query]() {
            while (true) {
                size_t file_idx = file_queue_index.fetch_add(1, std::memory_order_relaxed);
                if (file_idx >= files.size()) break;
                search_file(files[file_idx], query, false);
            }
        };
        
        // Launch worker threads
        for (size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker);
        }
        
        // Wait for completion
        for (auto& th : threads) {
            th.join();
        }
    }

    return 0;
}