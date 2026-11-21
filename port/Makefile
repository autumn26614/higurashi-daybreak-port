CXX      ?= g++
CC       ?= cc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
LIB      := src/archive.cpp src/cnv.cpp src/inflate.cpp src/xfile.cpp src/stage.cpp
BIN      := build/stageinfo
CAPI     := build/capi_test
SETTEST  := build/archive_set_test
CNVTEST  := build/cnv_test

# glibc has iconv built in; macOS ships it as a separate libiconv.
ifeq ($(shell uname -s),Darwin)
LDLIBS   += -liconv
endif

all: $(BIN) $(CAPI) $(SETTEST) $(CNVTEST)

$(BIN): $(LIB) src/png.cpp src/stageinfo.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(LIB) src/png.cpp src/stageinfo.cpp $(LDLIBS)

# C API smoke test: compiled as C (like the Swift bridging header sees it), linked as C++.
$(CAPI): $(LIB) src/daybreak_c.cpp tests/capi_test.c src/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c tests/capi_test.c -o $@.o
	$(CXX) $(CXXFLAGS) -o $@ $@.o $(LIB) src/daybreak_c.cpp $(LDLIBS)

# Ordered multi-archive lookup (Kai overlay); writes two synthetic archives into build/.
$(SETTEST): $(LIB) src/daybreak_c.cpp tests/archive_set_test.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(LIB) src/daybreak_c.cpp tests/archive_set_test.cpp $(LDLIBS)

# .cnv decoding and the image C API (synthetic data + ios/Fixtures).
$(CNVTEST): $(LIB) src/daybreak_c.cpp tests/cnv_test.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(LIB) src/daybreak_c.cpp tests/cnv_test.cpp $(LDLIBS)

test: $(SETTEST) $(CNVTEST)
	$(SETTEST) $(dir $(SETTEST))
	$(CNVTEST) ios/Fixtures

clean:
	rm -rf build

.PHONY: all clean test
