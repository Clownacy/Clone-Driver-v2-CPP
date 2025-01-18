	include /opt/clownmdsdk/generic.mk

CXXFLAGS += -O3 -Wall -Wextra -Wpedantic -ffixed-a5 -std=c++23 -mpcrel -flto -fvisibility=hidden -DBINARY_BLOB -DNDEBUG
LDFLAGS += -save-temps=obj -shared -Wl,-shared

all: output/core.bin output/get-track-note.bin output/initialise.bin output/exports.asm

output/exports/output.bin: exports.cpp
	mkdir -p $(@D)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS) -T exports.ld

output/%/output.bin: %.cpp
	mkdir -p $(@D)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS) -T link.ld

output/%.bin: output/%/output.bin
	cp $^ $@

output/%.asm: output/%/output.bin
	cp $^ $@
