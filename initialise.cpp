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
	#include "dac/DAC-Driver.bin.h"
});

STARTING_FUNCTION void SMPS::Initialise()
{
	const auto previous_interrupt_mask = ClownMDSDK::M68k::DisableInterrupts();
	ClownMDSDK::Z80::Unsafe::DeassertReset();
	ClownMDSDK::Z80::Unsafe::RequestBus();
	ClownMDSDK::Z80::Unsafe::WaitUntilBusObtained();

	state->pal = ClownMDSDK::Unsafe::IsPAL();

	ClownLZSS::SaxmanDecompress(std::begin(dac_driver), std::end(dac_driver), ClownMDSDK::Z80::Unsafe::ram);

	ClownMDSDK::Z80::Reset();
	ClownMDSDK::Z80::Unsafe::ReleaseBus();
	ClownMDSDK::M68k::SetInterruptMask(previous_interrupt_mask);
}
