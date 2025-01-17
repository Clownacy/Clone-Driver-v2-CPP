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
	const auto previous_interrupt_mask = ClownMDSDK::MainCPU::M68k::DisableInterrupts();
	ClownMDSDK::MainCPU::Z80::Unsafe::DeassertReset();
	ClownMDSDK::MainCPU::Z80::Unsafe::RequestBus();
	ClownMDSDK::MainCPU::Z80::Unsafe::WaitUntilBusObtained();

	state->pal = ClownMDSDK::MainCPU::Unsafe::IsPAL();

	ClownLZSS::SaxmanDecompress(std::begin(dac_driver), std::end(dac_driver), std::begin(ClownMDSDK::MainCPU::Z80::Unsafe::ram));

	ClownMDSDK::MainCPU::Z80::Reset();
	ClownMDSDK::MainCPU::Z80::Unsafe::ReleaseBus();
	ClownMDSDK::MainCPU::M68k::SetInterruptMask(previous_interrupt_mask);
}
