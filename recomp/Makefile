# Static recompilation of daybreak.exe (see README.md).
#   make GAME=<folder with daybreak.exe>        translate, then build the host program
#   make run GAME=...                            run it headless
GAME ?= ../Higurashi Daybreak (Japan)/data
GEN ?= gen
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
$(GEN)/stamp: tools/lift.py tools/discover.py tools/pe.py
	$(PYTHON) tools/lift.py "$(GAME)/daybreak.exe" $(GEN) --prefix F
	$(PYTHON) tools/lift.py "$(GAME)/d3dx9_25.dll" $(GEN) --prefix X --base 0x00A00000
	touch $@

GEN_C = $(wildcard $(GEN)/F_*.c $(GEN)/X_*.c)
GEN_OBJ = $(GEN_C:$(GEN)/%.c=$(BUILD)/gen/%.o)
# A few huge generated functions exhaust memory at -O2: those files are built at -O1.

$(BUILD)/gen/%.o: $(GEN)/%.c runtime/x86.h $(GEN)/stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@ || $(CC) $(CFLAGS) -O1 -c $< -o $@

$(BUILD)/rt/%.o: runtime/%.cpp $(wildcard runtime/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@


link: $(GEN_OBJ) $(RT_OBJ)
	$(CXX) $(GEN_OBJ) $(RT_OBJ) -o $(BUILD)/daybreak-recomp -rdynamic -lpthread -lm

run: all
	$(BUILD)/daybreak-recomp "$(GAME)"

clean:
	rm -rf $(BUILD) $(GEN)

.PHONY: all link run clean
