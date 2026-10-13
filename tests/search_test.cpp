// Standalone property checks for the search kernel and chunk boundaries.
#define main fff_cli_main
#include "../fff.cpp"
#undef main
#include <random>
#include <cassert>

static std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        result.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    std::sort(result.begin(), result.end());
    return result;
}

int main() {
    init_freq_table();
    use_color = false;
    FILE* capture = tmpfile();
    assert(capture);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    auto check = [&](const char* data, size_t size, const std::string& query, size_t chunk) {
        fflush(stdout);
        assert(ftruncate(fileno(capture), 0) == 0);
        rewind(stdout);
        std::vector<WorkChunk> chunks;
        build_chunks(data, size, chunk, chunks);
        auto pat = make_pattern(query.data(), query.size());
        for (const auto& c : chunks) process_range(c.data, c.start, c.end, pat);
        fflush(stdout);
        long length = ftell(stdout);
        std::string actual(static_cast<size_t>(length), '\0');
        assert(pread(fileno(capture), actual.data(), actual.size(), 0) == static_cast<ssize_t>(actual.size()));
        std::vector<std::string> expected;
        for (const auto& line : lines(std::string(data, size)))
            if (line.find(query) != std::string::npos) expected.push_back(line);
        if (lines(actual) != expected) {
            fprintf(stderr, "mismatch size=%zu m=%zu chunk=%zu got=%zu expected=%zu bytes=%ld\n", size, query.size(), chunk, lines(actual).size(), expected.size(), length);
            for (const auto& x : lines(actual)) fprintf(stderr, "got length %zu\n", x.size());
            for (const auto& x : expected) fprintf(stderr, "expected length %zu\n", x.size());
            abort();
        }
    };
    // Put every search tail directly against an inaccessible page.
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    char* mapping = static_cast<char*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(mapping != MAP_FAILED);
    assert(mprotect(mapping + page, page, PROT_NONE) == 0);
    std::mt19937 rng(1729);
    for (size_t size = 1; size <= 768; ++size) {
        char* data = mapping + page - size;
        for (size_t i = 0; i < size; ++i) data[i] = "abcz\n\0"[rng() % 6];
        for (size_t m : {1u, 2u, 7u, 8u, 63u, 64u, 65u, 129u}) {
            std::string query(m, 'a');
            if (size % 2) query.back() = 'z'; // rare filter byte at the final offset
            if (size >= m) {
                size_t at = rng() % (size - m + 1);
                // Include true matches spanning SIMD blocks and near file tails.
                memcpy(data + at, query.data(), m);
            }
            check(data, size, query, 97);
        }
    }
    std::string long_line(2 * OUTPUT_BUFFER_SIZE, 'x');
    long_line += "needle\nneedle needle\nlast needle";
    check(long_line.data(), long_line.size(), "needle", 4096);
    munmap(mapping, 2 * page);
    fclose(capture);
    fprintf(stderr, "Search, guarded tails, binary bytes, and chunk boundaries passed.\n");
}
