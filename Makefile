	include /opt/clownmdsdk/bare.mk

CXXFLAGS += -O3 -Wall -Wextra -Wpedantic -ffixed-a5 -std=c++23 -mpcrel -flto -fvisibility=hidden -DBINARY_BLOB
LDFLAGS += -save-temps=obj -shared -Wl,-shared -T link.ld

all: output/core.bin output/get-track-note.bin output/initialise.bin output/exports.asm

output/exports/output.bin: exports.cpp
	mkdir -p $(@D)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS)

output/%/output.bin: %.cpp
	mkdir -p $(@D)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS) -lc

output/%.bin: output/%/output.bin
	cp $^ $@

output/%.asm: output/%/output.bin
	cp $^ $@
