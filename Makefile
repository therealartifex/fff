CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG
LDFLAGS = -pthread -flto

TARGET = fff
SOURCES = fff.cpp
PROFDIR = ./profdata

.PHONY: all clean production pgo pgo-use

all: production

production: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCES)

pgo: $(SOURCES)
	@mkdir -p $(PROFDIR)
	$(CXX) -std=c++17 -O3 -march=skylake-avx512 -fno-rtti -DNDEBUG -fprofile-generate=$(PROFDIR) -pthread -o $(TARGET)_pgo_gen $(SOURCES)
	@echo "Run training workload: ./$(TARGET)_pgo_gen <query> <file> > /dev/null"
	@echo "Then run: make pgo-use"

pgo-use: $(SOURCES)
	$(CXX) $(CXXFLAGS) -fprofile-use=$(PROFDIR) -Wno-missing-profile $(LDFLAGS) -o $(TARGET) $(SOURCES)
	rm -rf $(PROFDIR) $(TARGET)_pgo_gen

clean:
	rm -f $(TARGET) $(TARGET)_pgo_gen *.gcda
	rm -rf $(PROFDIR)
