CXX      ?= g++
CC       ?= cc
# Extra flags from the environment are appended, never replace the required ones.
# -ffp-contract=off: no fused multiply-add, so bounding boxes match on x86-64 and arm64.
override CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -ffp-contract=off $(CXXFLAGS)
override CFLAGS   := -std=c11 -O2 -Wall -Wextra -ffp-contract=off $(CFLAGS)
LIB      := src/archive.cpp src/cnv.cpp src/inflate.cpp src/xfile.cpp src/stage.cpp src/identify.cpp src/scenario.cpp src/geometry.cpp src/raster.cpp src/talk.cpp
BUILD    ?= build
BIN      := $(BUILD)/stageinfo
CAPI     := $(BUILD)/capi_test
SETTEST  := $(BUILD)/archive_set_test
CNVTEST  := $(BUILD)/cnv_test
SCNTEST  := $(BUILD)/scenario_test
RENDER   := $(BUILD)/render_stage

# glibc has iconv built in; macOS ships it as a separate libiconv.
ifeq ($(shell uname -s),Darwin)
LDLIBS   += -liconv
endif

all: $(BIN) $(CAPI) $(SETTEST) $(CNVTEST) $(SCNTEST) $(RENDER)

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

# Scenario script parser and its C API (synthetic scripts).
$(SCNTEST): $(LIB) src/daybreak_c.cpp tests/scenario_test.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(LIB) src/daybreak_c.cpp tests/scenario_test.cpp $(LDLIBS)

# CPU reference renderer (host-only inspection tool, writes a PNG).
$(RENDER): $(LIB) src/png.cpp tools/render_stage.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(LIB) src/png.cpp tools/render_stage.cpp $(LDLIBS)

test: $(SETTEST) $(CNVTEST) $(SCNTEST)
	$(SETTEST) $(dir $(SETTEST))
	$(CNVTEST) ios/Fixtures
	$(SCNTEST)

# Same tests under AddressSanitizer + UBSan, built into build/asan.
SANFLAGS := -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
asan:
	$(MAKE) BUILD=build/asan CXXFLAGS="$(SANFLAGS)" CFLAGS="$(SANFLAGS)" test

# The iOS app builds with DB_NO_ICONV=1 (ios/project.yml); run the tests that way too.
noiconv:
	$(MAKE) BUILD=build/noiconv CXXFLAGS="-DDB_NO_ICONV=1" test

clean:
	rm -rf build

.PHONY: all clean test asan noiconv
