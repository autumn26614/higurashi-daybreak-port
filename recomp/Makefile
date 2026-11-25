# Static recompilation of daybreak.exe (see README.md).
#   make GAME=<folder with daybreak.exe>        translate, then build the host program
#   make run GAME=...                            run it headless
#   make GAME=... KAI=<folder with DaybreakDX.exe>   both games in one build (RECOMP_GAME=kai
#                                                    runs Kai; GAME may then lack daybreak.exe)
GAME ?= ../Higurashi Daybreak (Japan)/data
GEN ?= gen
# KAI: also translate Kai, from that folder (empty: the base game only)
KAI ?=
BUILD ?= build
PYTHON ?= python3 -I
CC ?= gcc
CXX ?= g++
OPT ?= -O2
GENOPT ?= -O2
CFLAGS += $(GENOPT) -g -w -Iruntime -I$(GEN)
CXXFLAGS += $(OPT) -g -std=c++17 -Wall -Wextra -Wno-unused-parameter -Iruntime

RUNTIME := $(wildcard runtime/*.cpp)
RT_OBJ := $(RUNTIME:runtime/%.cpp=$(BUILD)/rt/%.o)

all: $(GEN)/stamp
	$(MAKE) link

# The exe at its own base; d3dx9_25.dll (which prefers the same base) moved to 0x00A00000 (below the heap, and outside the values UTF-16 text takes).
# daybreak.exe becomes F_*, Kai's DaybreakDX.exe (when KAI names its folder) K_*.
D3DX_DIR = $(if $(wildcard $(GAME)/d3dx9_25.dll),$(GAME),$(KAI))
$(GEN)/stamp: tools/lift.py tools/discover.py tools/pe.py
	@test -f "$(GAME)/daybreak.exe" -o -n "$(KAI)" || { echo "no daybreak.exe in GAME=$(GAME), and no KAI="; exit 1; }
	if [ -f "$(GAME)/daybreak.exe" ]; then $(PYTHON) tools/lift.py "$(GAME)/daybreak.exe" $(GEN) --prefix F; fi
	if [ -n "$(KAI)" ]; then $(PYTHON) tools/lift.py "$(KAI)/DaybreakDX.exe" $(GEN) --prefix K; fi
	$(PYTHON) tools/lift.py "$(D3DX_DIR)/d3dx9_25.dll" $(GEN) --prefix X --base 0x00A00000
	touch $@

GEN_C = $(wildcard $(GEN)/F_*.c $(GEN)/K_*.c $(GEN)/X_*.c)
GEN_OBJ = $(GEN_C:$(GEN)/%.c=$(BUILD)/gen/%.o)
# A few huge generated functions exhaust memory at -O2: those files are built at -O1.

$(BUILD)/gen/%.o: $(GEN)/%.c runtime/x86.h $(GEN)/stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@ || $(CC) $(CFLAGS) -O1 -c $< -o $@

$(BUILD)/rt/%.o: runtime/%.cpp $(wildcard runtime/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@


# gen_stub.o: empty weak tables for a game that is not translated (Kai, in a base-only build);
# the translated tables replace them.
$(BUILD)/gen_stub.o: runtime/gen_stub.c runtime/x86.h
	@mkdir -p $(dir $@)
	$(CC) -c -O1 -Iruntime runtime/gen_stub.c -o $@

link: $(GEN_OBJ) $(RT_OBJ) $(BUILD)/gen_stub.o
	$(CXX) $(GEN_OBJ) $(RT_OBJ) $(BUILD)/gen_stub.o -o $(BUILD)/daybreak-recomp -rdynamic -lpthread -lm

run: all
	$(BUILD)/daybreak-recomp "$(GAME)"

clean:
	rm -rf $(BUILD) $(GEN)

.PHONY: all link run clean

# Checks that need no game data (x86-64 Linux with gcc): the flag helpers against the
# CPU, and the lifter on 32-bit code compiled from tests/lifter/cases.c.
test: all
	$(CXX) -std=c++17 -O1 -w -Iruntime tests/flags_test.cpp -o $(BUILD)/flags_test
	$(BUILD)/flags_test
	$(CC) -c -O1 -Iruntime runtime/gen_stub.c -o $(BUILD)/gen_stub.o
	$(CXX) -std=c++17 -O1 -w -Iruntime tests/shims_test.cpp $(filter-out $(BUILD)/rt/main.o,$(RT_OBJ)) $(BUILD)/gen_stub.o -o $(BUILD)/shims_test -lpthread -lm
	$(BUILD)/shims_test
	sh tests/lifter/run.sh

.PHONY: test
