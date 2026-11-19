#include "common.h"
#include "core.h"

using namespace SMPS;

#ifdef __MEGA_DRIVE__
#include "dac/DAC-Driver.h"
register State* state asm("a5");
#endif

STARTING_FUNCTION unsigned int SMPS::GetTrackNote(const unsigned int track_id, const bool modulation)
{
	const auto &TrackMap = [](unsigned int index) -> Track*
	{
		if (index == 0)
			return &state->music_tracks.dac;

		--index;

		if (index == 0)
			return &state->music_tracks.FM1();

		--index;

		if (index == 0)
			return &state->music_tracks.FM2();

		--index;

		if (index == 0)
			return &state->music_tracks.FM3();

		--index;

		if (index == 0)
			return &state->music_tracks.FM4();

		--index;

		if (index == 0)
			return &state->music_tracks.FM5();

		--index;

		if (index == 0)
			return &state->music_tracks.FM6();

		--index;

		if (index == 0)
			return &state->music_tracks.PSG1();

		--index;

		if (index == 0)
			return &state->music_tracks.PSG2();

		--index;

		if (index == 0)
			return &state->music_tracks.PSG3();

	#ifdef SMPS_EnablePSGNoiseDrums
		--index;

		if (index == 0)
			return &state->music_tracks.psg_noise;
	#endif

		return state->VisitShared(
			[&](State::Shared::OtherTracks &other_tracks) -> Track*
			{
				--index;

				if (index == 0)
					return &other_tracks.sfx.FM3();

				--index;

				if (index == 0)
					return &other_tracks.sfx.FM4();

				--index;

				if (index == 0)
					return &other_tracks.sfx.FM4();

				--index;

				if (index == 0)
					return &other_tracks.sfx.PSG1();

				--index;

				if (index == 0)
					return &other_tracks.sfx.PSG2();

				--index;

				if (index == 0)
					return &other_tracks.sfx.PSG3();

			#ifdef SMPS_EnableSpecSFX
				--index;

				if (index == 0)
					return &other_tracks.special_sfx.fm4;

				--index;

				if (index == 0)
					return &other_tracks.special_sfx.psg3;
			#endif

				return nullptr;
			},
			[](auto &) -> Track*
			{
				return nullptr;
			}
		);
	};

	const auto &track = TrackMap(track_id);

	if (track == nullptr)
		return 0;

	if (!track->IsPlaying())
		return 0;

	if (track->IsResting())
		return 0;

	if (track->IsOverridden())
		return 0;

	const unsigned int note = [&]() -> unsigned int
	{
		if (track == &state->music_tracks.dac)
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

			return track->freq;
		}
		else
		{
			// Music/SFX FM/PSG
			unsigned int note = track->freq + track->detune;

			if (modulation && track->IsModulationEnabled())
				note += track->modulation_val;

			return note;
		}
	}();
#ifdef SMPS_SoundTest
	return note | (track->IsSoundTest() ? 0x8000 : 0);
#else
	return note;
#endif
}
