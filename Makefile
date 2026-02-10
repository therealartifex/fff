CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG
LDFLAGS = -pthread -flto

TARGET = fff
SOURCES = fff.cpp

.PHONY: all clean production pgo

all: production

production: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCES)

pgo: $(SOURCES)
	$(CXX) $(CXXFLAGS) -fprofile-generate $(LDFLAGS) -o $(TARGET)_pgo_gen $(SOURCES)
	mv $(TARGET)_pgo_gen-$(TARGET).gcda $(TARGET).gcda
	@echo "Run training workload: ./$(TARGET) <query> <file> > /dev/null"
	@echo "Then run: make pgo-use"

pgo-use: $(SOURCES)
	$(CXX) $(CXXFLAGS) -fprofile-use $(LDFLAGS) -o $(TARGET) $(SOURCES)
	rm -f *.gcda $(TARGET)_pgo_gen

clean:
	rm -f $(TARGET) $(TARGET)_pgo_gen *.gcda
