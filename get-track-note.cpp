#include "common.h"
#include "core.h"

using namespace SMPS;

#ifdef __MEGA_DRIVE__
#include "dac/DAC-Driver.h"
register State* state asm("a5");
#endif

STARTING_FUNCTION unsigned int SMPS::GetTrackNote(const unsigned int track_id, const bool modulation)
{
	const auto &TrackMap = [](unsigned int index) -> Track&
	{
		if (index == 0)
			return state->tracks.music.dac;

		--index;

		if (index == 0)
			return state->tracks.music.FM1();

		--index;

		if (index == 0)
			return state->tracks.music.FM2();

		--index;

		if (index == 0)
			return state->tracks.music.FM3();

		--index;

		if (index == 0)
			return state->tracks.music.FM4();

		--index;

		if (index == 0)
			return state->tracks.music.FM5();

		--index;

		if (index == 0)
			return state->tracks.music.FM6();

		--index;

		if (index == 0)
			return state->tracks.music.PSG1();

		--index;

		if (index == 0)
			return state->tracks.music.PSG2();

		--index;

		if (index == 0)
			return state->tracks.music.PSG3();

	#ifdef SMPS_EnablePSGNoiseDrums
		--index;

		if (index == 0)
			return state->tracks.music.psg_noise();
	#endif

		--index;

		if (index == 0)
			return state->tracks.sfx.FM3();

		--index;

		if (index == 0)
			return state->tracks.sfx.FM4();

		--index;

		if (index == 0)
			return state->tracks.sfx.FM4();

		--index;

		if (index == 0)
			return state->tracks.sfx.PSG1();

		--index;

		if (index == 0)
			return state->tracks.sfx.PSG2();

		--index;

		if (index == 0)
			return state->tracks.sfx.PSG3();

	#ifdef SMPS_EnableSpecSFX
		--index;

		if (index == 0)
			return state->tracks.special_sfx.fm4;

		--index;

		if (index == 0)
			return state->tracks.special_sfx.psg3;
	#endif

		std::unreachable();
	};

	const auto &track = TrackMap(track_id);

	if (!track.IsPlaying())
		return 0;

	if (track.IsResting())
		return 0;

	if (track.IsOverridden())
		return 0;

	const unsigned int note = [&]() -> unsigned int
	{
		if (&TrackMap(track_id) == &state->tracks.music.dac)
		{
			// DAC
			const auto z80_sample_address = []()
			{
				unsigned char byte1, byte2;
				ClownMDSDK::MainCPU::Z80::Bus::Lock(
					[&](ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
					{
						byte1 = z80_bus.RAM(zSample1Pointer + 0);
						byte2 = z80_bus.RAM(zSample1Pointer + 1);
					}
				);
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
#ifdef SMPS_SoundTest
	return note | (track.IsSoundTest() ? 0x8000 : 0);
#else
	return note;
#endif
}
