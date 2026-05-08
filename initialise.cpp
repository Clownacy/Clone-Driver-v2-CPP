#include <array>

#include <clownmdsdk.h>

#include "clownlzss/decompressors/saxman.h"
#include "common.h"
#include "core.h"

using namespace SMPS;

#ifdef BINARY_BLOB
register State* state asm("a5");
#else
static State* const state = &data->state;
#endif

static const auto dac_driver = std::to_array<unsigned char>({
	#embed "dac/DAC-Driver.sax"
});

STARTING_FUNCTION void SMPS::Initialise()
{
	ClownMDSDK::MainCPU::Z80::Unsafe::DeassertReset();
	ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
		[](auto &bus)
		{
			state->pal = bus.IsConsolePAL();
			ClownLZSS::SaxmanDecompress(std::begin(dac_driver), std::end(dac_driver), std::begin(bus.RAM()));
			ClownMDSDK::MainCPU::Z80::Reset();
		}
	);
}
