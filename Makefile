CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG
LDFLAGS = -pthread -flto

TARGET = fff
SOURCES = fff.cpp
PROFDIR = ./profdata
BENCHDIR = ./benchmarks

# Benchmark configuration — override on command line if needed:
#   make bench-hot QUERY=foo FILE=/path/to/file
QUERY ?= hoyoverse
FILE ?= /srv/fastshare/txtlog_alien_751.txt

.PHONY: all clean production pgo pgo-use \
        bench-hot bench-cold bench-vs-grep bench-before-after \
        bench-output-cost bench-perf bench-perf-llc bench-all

all: production

production: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCES)

pgo: $(SOURCES)
	@mkdir -p $(PROFDIR)
	$(CXX) -std=c++17 -O3 -march=skylake-avx512 -fno-rtti -DNDEBUG -fprofile-generate=$(PROFDIR) -pthread -o $(TARGET) $(SOURCES)
	@echo "Run training workload: ./$(TARGET) <query> <file> > /dev/null"
	@echo "Then run: make pgo-use"

pgo-use: $(SOURCES)
	$(CXX) $(CXXFLAGS) -fprofile-use=$(PROFDIR) $(LDFLAGS) -o $(TARGET) $(SOURCES)
	rm -rf $(PROFDIR)

# --- Benchmarks ---

bench-hot: $(TARGET)
	@mkdir -p $(BENCHDIR)
	hyperfine --warmup 3 --runs 10 \
	  --export-json $(BENCHDIR)/hot-cache.json \
	  --export-markdown $(BENCHDIR)/hot-cache.md \
	  './$(TARGET) $(QUERY) $(FILE) > /dev/null'

bench-cold: $(TARGET)
	@mkdir -p $(BENCHDIR)
	@echo "Requires root for cache drop"
	hyperfine --runs 5 \
	  --prepare 'sync; echo 3 > /proc/sys/vm/drop_caches' \
	  --export-json $(BENCHDIR)/cold-cache.json \
	  --export-markdown $(BENCHDIR)/cold-cache.md \
	  './$(TARGET) $(QUERY) $(FILE) > /dev/null'

bench-vs-grep: $(TARGET)
	@mkdir -p $(BENCHDIR)
	hyperfine --warmup 3 --runs 10 \
	  --export-json $(BENCHDIR)/vs-grep.json \
	  --export-markdown $(BENCHDIR)/vs-grep.md \
	  './$(TARGET) $(QUERY) $(FILE) > /tmp/fff_out.txt' \
	  'grep -a $(QUERY) $(FILE) > /tmp/grep_out.txt'

bench-before-after: $(TARGET)
	@mkdir -p $(BENCHDIR)
	@test -f ./$(TARGET)_old || { echo "Error: ./$(TARGET)_old not found. Run 'cp $(TARGET) $(TARGET)_old' before rebuilding."; exit 1; }
	hyperfine --warmup 3 --runs 10 \
	  --export-json $(BENCHDIR)/before-after.json \
	  --export-markdown $(BENCHDIR)/before-after.md \
	  './$(TARGET)_old $(QUERY) $(FILE) > /dev/null' \
	  './$(TARGET) $(QUERY) $(FILE) > /dev/null'

bench-output-cost: $(TARGET)
	@mkdir -p $(BENCHDIR)
	hyperfine --warmup 3 --runs 10 \
	  --export-json $(BENCHDIR)/output-cost.json \
	  --export-markdown $(BENCHDIR)/output-cost.md \
	  './$(TARGET) $(QUERY) $(FILE) > /dev/null' \
	  './$(TARGET) $(QUERY) $(FILE) > /tmp/fff_out.txt'

bench-perf: $(TARGET)
	@mkdir -p $(BENCHDIR)
	perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses,page-faults \
	  ./$(TARGET) $(QUERY) $(FILE) > /dev/null \
	  2> $(BENCHDIR)/perf-stat.log
	@cat $(BENCHDIR)/perf-stat.log

bench-perf-llc: $(TARGET)
	@mkdir -p $(BENCHDIR)
	perf stat -e LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses \
	  ./$(TARGET) $(QUERY) $(FILE) > /dev/null \
	  2> $(BENCHDIR)/perf-llc.log
	@cat $(BENCHDIR)/perf-llc.log

bench-all: bench-hot bench-vs-grep bench-output-cost bench-perf bench-perf-llc
	@echo "All benchmarks complete. Results in $(BENCHDIR)/"

clean:
	rm -f $(TARGET) *.gcda
	rm -rf $(PROFDIR)
