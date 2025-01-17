#include "common.h"
#include "core.h"

using namespace SMPS;

#ifdef __MEGA_DRIVE__
#include "dac/DAC-Driver.h"
register State* state asm("a5");
#endif

STARTING_FUNCTION unsigned int SMPS::GetTrackNote(const unsigned int track_id, const bool modulation)
{
	static constexpr auto track_map = std::to_array<unsigned int>({
		MUSIC_DAC,
		MUSIC_FM1,
		MUSIC_FM2,
		MUSIC_FM3,
		MUSIC_FM4,
		MUSIC_FM5,
		MUSIC_FM6,
		MUSIC_PSG1,
		MUSIC_PSG2,
		MUSIC_PSG3,

		SFX_FM3,
		SFX_FM4,
		SFX_FM5,
		SFX_PSG1,
		SFX_PSG2,
		SFX_PSG3,

	#ifdef SMPS_EnableSpecSFX
		SPECIAL_SFX_FM4,
		SPECIAL_SFX_PSG3,
	#endif
	});

	if (track_id >= track_map.size())
		return 0;

	const auto &track = state->tracks[track_map[track_id]];

	if (!track.IsPlaying())
		return 0;

	if (track.IsResting())
		return 0;

	if (track.IsOverridden())
		return 0;

	const unsigned int note = [&]() -> unsigned int
	{
		if (track_map[track_id] == MUSIC_DAC)
		{
			// DAC
			const auto z80_sample_address = []()
			{
				unsigned char byte1, byte2;
				{
					ClownMDSDK::MainCPU::Z80::Bus z80_bus;
					byte1 = z80_bus.RAM(zSample1Pointer + 0);
					byte2 = z80_bus.RAM(zSample1Pointer + 1);
				}
				return WordFromBytes(byte2, byte1);
			}();

			if (z80_sample_address == zMuteSample)
				return 0;

			return track.saved_dac;
		}
		else
		{
			// Music/SFX FM/PSG
			unsigned int note = track.freq + track.detune;

			if (modulation && track.IsModulationEnabled())
				note += track.modulation_val;

			return note;
		}
	}();

	return note | (track.IsSoundTest() ? 0x8000 : 0);
}
