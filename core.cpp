#include "core.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <utility>

using namespace SMPS;

#ifdef __MEGA_DRIVE__
#include "dac/DAC-Driver.h"
#endif

#ifdef BINARY_BLOB
// The data is expected to come directly after this binary blob.
Data data __attribute__ ((section (".driver_data"))) = {};

// RAM is provided in a register.
register State* state asm("a5");
#endif

#include "common.h"

static constexpr unsigned char z80_scf_instruction = 0x37;

template<typename T, std::size_t total_lists, std::size_t total_bytes>
class JaggedArray
{
private:
	std::array<unsigned int, total_lists> offsets;
	std::array<std::remove_cv_t<T>, total_bytes> data;

public:
	template<std::size_t... Ns>
	constexpr JaggedArray(T (&&...lists)[Ns])
	{
		unsigned int offset = 0, i = 0;
		([&]()
		{
			offsets[i++] = offset;
			std::copy(&lists[0], &lists[Ns], &data[offset]);
			offset += Ns;
		}(), ...);
	}

	constexpr auto operator[](this auto &&self, const std::size_t index)
	{
		return &self.data[self.offsets[index]];
	}
};

template<typename T, std::size_t... Ns>
static constexpr auto ToJaggedArray(T (&&...lists)[Ns])
{
	constexpr std::size_t total_lists = sizeof...(lists);
	constexpr std::size_t total_bytes = (... + Ns);

	return JaggedArray<T, total_lists, total_bytes>(std::forward<decltype(lists)>(lists)...);
}

static constexpr auto our_lround(const double frequency)
{
	return static_cast<long>(frequency + 0.5);
};

static FMTrack& GetFMMusicTrackForSFXChannel(const ChannelID channel_id)
{
	switch (channel_id)
	{
		default:
			std::unreachable();

		case ChannelID::FM3:
			return state->tracks.music.FM3();

		case ChannelID::FM4:
			return state->tracks.music.FM4();

		case ChannelID::FM5:
			return state->tracks.music.FM5();
	}
}

static PSGMelodyTrack& GetPSGMusicTrackForSFXChannel(const ChannelID channel_id)
{
	switch (channel_id)
	{
		default:
			std::unreachable();

		case ChannelID::PSG1:
			return state->tracks.music.PSG1();

		case ChannelID::PSG2:
			return state->tracks.music.PSG2();

		case ChannelID::PSG3:
		case ChannelID::PSGNoise:
			return state->tracks.music.PSG3();
	}
}

static Track& GetMusicTrackForSFXChannel(const ChannelID channel_id)
{
	if (IsPSG(channel_id))
		return GetPSGMusicTrackForSFXChannel(channel_id);
	else if (IsFM(channel_id))
		return GetFMMusicTrackForSFXChannel(channel_id);
	else
		std::unreachable();
}

static Track& GetSFXTrackForSFXChannel(const ChannelID channel_id)
{
	State::Tracks::Shared::NonMusicTracks &non_music_tracks = state->tracks.shared.GetNonMusicTracks();

	switch (channel_id)
	{
		default:
			std::unreachable();

		case ChannelID::FM3:
			return non_music_tracks.sfx.FM3();

		case ChannelID::FM4:
			return non_music_tracks.sfx.FM4();

		case ChannelID::FM5:
			return non_music_tracks.sfx.FM5();

		case ChannelID::PSG1:
			return non_music_tracks.sfx.PSG1();

		case ChannelID::PSG2:
			return non_music_tracks.sfx.PSG2();

		case ChannelID::PSG3:
		case ChannelID::PSGNoise:
			return non_music_tracks.sfx.PSG3();
	}
}

///////////////
// Non-Track //
///////////////

static void FMSilenceAll(Z80BusFMSafe &z80_bus)
{
	// Send key-off to all channels.
	for (unsigned int i = 0; i < 3; ++i)
	{
		z80_bus.WriteFMI(0x28, 0 + i);
		z80_bus.WriteFMI(0x28, 4 + i);
	}

	// Set all operators of all channels to maximum volume attenuation.
	unsigned int address = 0x40;

	for (unsigned int i = 0; i < 3; ++i)
	{
		for (unsigned int j = 0; j < 4; ++j)
		{
			z80_bus.WriteFMI(address, 0x7F);
			z80_bus.WriteFMII(address, 0x7F);

			// Next TL operator.
			address += 4;
		}

		// Move onto first TL operator of next channel.
		address -= 0xF;
	}
}

static void SilencePSGChannel(const ChannelID channel_id)
{
	// Set channel to maximum attenuation.
	ClownMDSDK::MainCPU::PSG::Write(GetPSGChannelID(channel_id) | 0x1F);
}

static void PSGSilenceAll()
{
	// Set all channels to maximum attenuation.
	SilencePSGChannel(ChannelID::PSG1);
	SilencePSGChannel(ChannelID::PSG2);
	SilencePSGChannel(ChannelID::PSG3);
	SilencePSGChannel(ChannelID::PSGNoise);
}

static void SilenceAll(Z80BusFMSafe &z80_bus)
{
#ifdef __MEGA_DRIVE__
	z80_bus.RAM(zRequestFlag) = z80_scf_instruction;
	// 'Stop PCM channel' command value
	z80_bus.RAM(zRequestChannel1) = 2;
	z80_bus.RAM(zRequestChannel2) = 2;
#endif

	FMSilenceAll(z80_bus);
	PSGSilenceAll();
#ifdef SMPS_EnablePWM
	PWMSilenceAll();
#endif
}

static void StopAllSound()
{
	state->variables = {};
	state->tracks.Iterate(
		[](Track &track)
		{
			track = {};
		}
	);

	// Disable music speed-up.
	state->music_tempo_modifier_master = 0;

	Z80BusFMSafe::Lock(
		[](Z80BusFMSafe &z80_bus)
		{
			// FM3/FM6 normal mode, disable timers.
			z80_bus.WriteFMI(0x27, 0);

			SilenceAll(z80_bus);
		}
	);
}

static const DACSample& GetDACSampleMetadata(const unsigned int id)
{
	return data.dac.list[id];
}

static void SendDACSampleRequest(ClownMDSDK::MainCPU::Z80::Bus &bus, const unsigned int channel, const DACSample &sample)
{
#ifdef __MEGA_DRIVE__
	// TODO: Assembly?
	const unsigned int offset = channel != 0 ? zRequestChannel2 : zRequestChannel1;
	bus.RAM(zRequestFlag) = z80_scf_instruction;
	// 'Play sample' command value
	bus.RAM(offset + 0) = 1;
	// Copy the sample's metadata
	bus.RAM(offset + 1) = sample.bank_offset.upper;
	bus.RAM(offset + 2) = sample.bank_offset.lower;
	bus.RAM(offset + 3) = sample.playback_increment.upper;
	bus.RAM(offset + 4) = sample.playback_increment.lower;
	bus.RAM(offset + 5) = sample.bank_index;
#endif
}

static void WriteDACVolumeAttenuation(ClownMDSDK::MainCPU::Z80::Bus &z80_bus, const unsigned int channel, const unsigned int volume_attenuation)
{
#ifdef __MEGA_DRIVE__
	z80_bus.RAM(channel != 0 ? zSample2Volume : zSample1Volume) = volume_attenuation | zSampleLookup >> 8;
#endif
}

static void WriteDACVolumeAttenuation(const unsigned int channel, const unsigned int volume_attenuation)
{
	ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
		[&](ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
		{
			WriteDACVolumeAttenuation(z80_bus, channel, volume_attenuation);
		}
	);
}

static void PlayDACSFX(const unsigned int dac_channel, const unsigned int dac_index)
{
	state->tracks.music.dac.SetFM6Overridden(true);
	state->tracks.music.FM6().SetFM6Overridden(true);

	const auto &sample = GetDACSampleMetadata(dac_index);

	Z80BusFMSafe::Lock(
		[&](Z80BusFMSafe &z80_bus)
		{
			// Enable DAC.
			z80_bus.WriteFMI(0x2B, 0x80);

			// Force L/R panning.
			z80_bus.WriteFMII(0xB6, 0xC0);

			SendDACSampleRequest(z80_bus, dac_channel, sample);

			// This is a DAC SFX: set to full volume
			WriteDACVolumeAttenuation(z80_bus, dac_channel, 0);
		}
	);
}

/////////////
// Generic //
/////////////

void YM2612Track::WriteFMIorII(Z80BusFMSafe &z80_bus, const unsigned char port, const unsigned char value)
{
	const unsigned char complete_port = port | GetFMSlot(channel_id);

	switch (GetFMPort(channel_id))
	{
		case 0:
			z80_bus.WriteFMI(complete_port, value);
			break;
		case 1:
			z80_bus.WriteFMII(complete_port, value);
			break;
		default:
			std::unreachable();
	}
}

void Track::cfJumpTo()
{
	data_pointer += ReadUnalignedSignedWord(data_pointer) + 1;
}

void Track::FinishTrackUpdate()
{
	duration_timeout = saved_duration;

	if (IsHeld())
		return;

#ifdef SMPS_SoundTest
	SetSoundTest(true);
#endif

	note_timeout = note_timeout_master;

	Visit(
		[](PSGTrack &track)
		{
			track.vol_env_index = 0;
		},
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) {},
	#endif
		[](DACTrack&) {},
		[](FMTrack&) {}
	);

#ifdef SMPS_EnableModulationEnvelopes
	mod_env_index = 0;
	mod_env_sens = 0;
#endif

	if (!IsModulationEnabled())
		return;

	if (!IsModulationInZ80Mode() && IsHeld())
		return;

	ReloadModulationData();
}

void Track::SetDuration(const unsigned int duration)
{
	saved_duration = duration_timeout = duration * tempo_divider;
}

bool Track::CoordFlag(const unsigned int flag)
{
	if (flag == 0xFE) // cfHoldNote
	{
		SetHeld(true);
		return true;
	}

	switch (*data_pointer++)
	{
		case 0: // cfPanningAMSFMS
		{
			const unsigned int value = *data_pointer++;

			Visit(
				[&value](YM2612Track &track)
				{
					track.ams_fms_pan = (track.ams_fms_pan & 0x37) | value;

					if (track.IsOverridden() || track.IsFM6Overridden())
						return;

					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.WriteFMIorII(z80_bus, 0xB4, track.ams_fms_pan);
						}
					);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](PSGTrack&) {}
			);

			break;
		}

		case 1: // cfDetune
			detune = *data_pointer++;
			break;

		case 2: // cfSetCommunication
			state->variables.communication_byte = *data_pointer++;
			break;

		case 3: // cfJumpReturn
			data_pointer = go_sub_stack[stack_pointer++];
			break;

		case 4: // cfFadeInToPrevious
			// We must handle this later to prevent the DAC channel from desynchronising.
			state->variables.fade_to_previous_pending = true;
			return false;

		case 5: // cfSetTempoDivider
			tempo_divider = *data_pointer++;
			break;

		case 6: // cfChangeFMVolume
		{
			const unsigned int value = *data_pointer++;

			Visit(
				[](PSGTrack&) {},
#ifdef SMPS_EnablePWM
				[&value](PWMTrack &track)
				{
					track.volume_attenuation += value;
				},
#endif
				[&value](DACTrack &track)
				{
					track.volume_attenuation += value;

					ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
						[&](ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
						{
							track.ApplyVolumeAttenuation(z80_bus);
						}
					);
				},
				[&value](FMTrack &track)
				{
					track.volume_attenuation += value;

					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.SendVoiceTL(z80_bus);
						}
					);
				}
			);

			break;
		}

#ifdef SMPS_EnableSpecSFX
		case 7: // cfStopSpecialFM4
			SetHeld(false);
			SetPlaying(false);

			Visit(
				[](FMTrack &track)
				{
					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.NoteOff(z80_bus);

							if (state->tracks.shared.GetNonMusicTracks().sfx.FM4().IsPlaying())
								return;

							FMTrack &music_fm4 = state->tracks.music.FM4();
							music_fm4.SetOverridden(false);
							music_fm4.SetResting(true);

							music_fm4.SetVoice(z80_bus);
						}
					);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {},
				[](PSGTrack&) {}
			);

			return false;
#endif

		case 8: // cfNoteTimeout
			note_timeout = note_timeout_master = *data_pointer++;
			break;

		case 9: // cfChangeTransposition
			transpose += *data_pointer++;
			break;

		case 0xA: // cfSetTempo
			state->variables.main_tempo = state->variables.main_tempo_timeout = *data_pointer++;
			break;

		case 0xB: // cfSetTempoDividerAll
		{
			const unsigned char value = *data_pointer++;

			state->tracks.music.Iterate(
				[&value](Track &track)
				{
					track.tempo_divider = value;
				}
			);

			break;
		}

		case 0xC: // cfChangePSGVolume
			volume_attenuation += *data_pointer++;
			break;

		case 0xD: // cfSetVoice
			voice_index = *data_pointer++;

			Visit(
				[](FMTrack &track)
				{
					if (track.IsOverridden())
						return;

					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.SilenceChannel(z80_bus);
							track.SetVoice(z80_bus);
						}
					);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {},
				[](PSGTrack&) {}
			);

			break;

		case 0xE: // cfModulationSMPS68k
			modulation_ptr = reinterpret_cast<const ModulationSettings*>(data_pointer);
			data_pointer += 4;
			ReloadModulationData();
			[[fallthrough]];
		case 0xF: // cfEnableModulation
			SetModulationEnabled(true);
			break;

		case 0x18: // cfSilenceStopTrack
			Visit(
				[](FMTrack &track)
				{
					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.SilenceChannel(z80_bus);
						}
					);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {},
				[](PSGTrack&) {}
			);
			[[fallthrough]];
#ifndef SMPS_EnableSpecSFX
		case 7: // cfStopSpecialFM4
#endif
		case 0x10: // cfStopTrack
		{
			SetHeld(false);
			SetPlaying(false);

			Visit(
				[](FMTrack &track)
				{
					track.NoteOff();
				},
				[](PSGTrack &track)
				{
					track.NoteOff();
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {}
			);

			if (IsMusic())
				return false;

			State::Tracks::Shared::NonMusicTracks &non_music_tracks = state->tracks.shared.GetNonMusicTracks();

			state->variables.sndprio = 0;

			Visit(
				[&non_music_tracks](FMTrack &track)
				{
				#ifdef SMPS_EnableSpecSFX
					auto &special_fm4 = non_music_tracks.special_sfx.fm4;
				#endif

					FMTrack &other_track =
					#ifdef SMPS_EnableSpecSFX
						track.channel_id == ChannelID::FM4 && special_fm4.IsPlaying() ? special_fm4 :
					#endif
						GetFMMusicTrackForSFXChannel(track.channel_id);

					if (!other_track.IsPlaying())
						return;

					other_track.SetOverridden(false);
					other_track.SetResting(true);

					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							other_track.SetVoice(z80_bus);
						}
					);
				},
				[&non_music_tracks](PSGMelodyTrack &track)
				{
					PSGTrack &other_track = GetPSGMusicTrackForSFXChannel(track.channel_id);

					other_track.SetOverridden(false);
					other_track.SetResting(true);
				},
				[&non_music_tracks](PSG3MelodyTrack&)
				{
				#ifdef SMPS_EnableSpecSFX
					auto &special_psg3 = non_music_tracks.special_sfx.psg3;
				#endif

					PSG3MelodyTrack &other_track =
					#ifdef SMPS_EnableSpecSFX
						special_psg3.IsPlaying() ? special_psg3 :
					#endif
						state->tracks.music.PSG3();

					other_track.SetOverridden(false);
					other_track.SetResting(true);

					if (other_track.channel_id == ChannelID::PSGNoise)
						ClownMDSDK::MainCPU::PSG::Write(other_track.psg_noise);
				},
			#ifdef SMPS_EnablePSGNoiseDrums
				[&non_music_tracks](PSGDrumTrack &track)
				{
					std::unreachable();
				},
			#endif
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {}
			);

			return false;
		}

		case 0x11: // cfSetPSGNoise
		{
			const auto psg_noise = *data_pointer++;

			Visit(
				[&psg_noise](PSG3MelodyTrack &track)
				{
					track.psg_noise = psg_noise;

					const bool exit_noise_mode = psg_noise == 0;

					track.channel_id      = exit_noise_mode ? ChannelID::PSG3 : ChannelID::PSGNoise;
					const unsigned int value = exit_noise_mode ? 0xFF : track.psg_noise;

					if (!track.IsOverridden())
						ClownMDSDK::MainCPU::PSG::Write(value);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](DACTrack&) {},
				[](FMTrack&) {},
				[](PSGTrack&) {}
			);

			break;
		}

		case 0x12: // cfDisableModulation
			SetModulationEnabled(false);
			break;

		case 0x13: // cfSetPSGTone
		{
			const unsigned char value = *data_pointer++;

			if (!IsPSG(channel_id))
				break;

			voice_index = value;
			break;
		}

		case 0x14: // cfJumpTo
			cfJumpTo();
			break;

		case 0x15: // cfRepeatAtPos
		{
			auto &loop_counter = loop_counters[data_pointer[0]];
			const auto repeat_count = data_pointer[1];
			data_pointer += 2;

			if (loop_counter == 0)
				loop_counter = repeat_count;

			if (--loop_counter != 0)
				cfJumpTo();
			else
				data_pointer += 2;

			break;
		}

		case 0x16: // cfJumpToGosub
		{
			const signed int offset = ReadUnalignedSignedWord(data_pointer);
			data_pointer += 2;

			go_sub_stack[--stack_pointer] = data_pointer;
			data_pointer += offset;
			break;
		}

		case 0x17: // cfChanFMCommand
			Visit(
				[](YM2612Track &track)
				{
					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.WriteFMIorII(z80_bus, track.data_pointer[0], track.data_pointer[1]);
						}
					);
				},
			#ifdef SMPS_EnablePWM
				[](PWMTrack&) {},
			#endif
				[](PSGTrack&) {}
			);
			data_pointer += 2;
			break;

		case 0x19: // cfPlayDACSample
			PlayDACSFX(0, *data_pointer++ - 1);
			break;

		case 0x1A: // cfPlaySound
			state->variables.queue[1] = *data_pointer++;
			break;

		case 0x1B: // cfSetKey
			transpose = *data_pointer++;
			break;

		case 0x1C: // cfSetVolume
		{
			const unsigned int value = *data_pointer++ ^ 0xFF;

			Visit(
				[&value](DACTrack &track)
				{
					track.volume_attenuation = value & 0x7F;

					ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
						[&](ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
						{
							track.ApplyVolumeAttenuation(z80_bus);
						}
					);
				},
				[&value](FMTrack &track)
				{
					track.volume_attenuation = value & 0x7F;

					Z80BusFMSafe::Lock(
						[&](Z80BusFMSafe &z80_bus)
						{
							track.SendVoiceTL(z80_bus);
						}
					);
				},
				[&value](PSGTrack &track)
				{
					track.volume_attenuation = value & 0x78;
				}
			);

			break;
		}

		case 0x1D: // cfNoteTimeoutS3K
			note_timeout = note_timeout_master = *data_pointer++ * tempo_divider; // TODO: Cast to unsigned int?
			break;

		case 0x1E: // cfLoopContinuousSFX
#ifdef SMPS_EnableContSFX
			if (!state->continuous_sfx)
			{
				state->variables.current_contsfx = 0;
				data_pointer += 2;
			}
			else
			{
				if (--state->variables.contsfx_channels == 0)
					state->continuous_sfx = false;

				cfJumpTo();
			}
#else
			data_pointer += 2;
#endif
			break;

		case 0x1F: // cfClearPush
#ifdef SMPS_PushSFXBehaviour
			state->push_playing = false;
#endif
			break;

		case 0x20: // cfSendFMI
			Z80BusFMSafe::Lock(
				[&](Z80BusFMSafe &z80_bus)
				{
					z80_bus.WriteFMI(data_pointer[0], data_pointer[1]);
				}
			);
			data_pointer += 2;
			break;

		case 0x21: // cfModulationSMPSZ80
#ifdef SMPS_EnableModulationEnvelopes
			modulation_ctrl = 0;
#endif
			SetModulationEnabled(true);
			SetModulationInZ80Mode(true);

			modulation_ptr = reinterpret_cast<const ModulationSettings*>(data_pointer);
			data_pointer += 4;
			break;

		case 0x22: // cfSetModulation
#ifdef SMPS_EnableModulationEnvelopes
			modulation_ctrl = *data_pointer++;
#else
			++data_pointer;
#endif
			break;

		case 0x23: // cfChangePSGDrumVolume
#ifdef SMPS_EnablePSGNoiseDrums
			state->variables.psg_drum_volume = (state->variables.psg_drum_volume + *data_pointer++) & (0xF << 3);
#else
			++data_pointer;
#endif
			break;
	}

	return true;
}

bool Track::NoteTimeoutUpdate()
{
	if (note_timeout == 0 || --note_timeout != 0)
		return true;

	SetResting(true);

	Visit(
		[](FMTrack &track)
		{
			track.NoteOff();
		},
		[](PSGTrack &track)
		{
			track.NoteOff();
		},
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) {},
	#endif
		[](DACTrack&) {}
	);

	return false;
}

#ifdef SMPS_EnableModulationEnvelopes
static constexpr signed char MODENV_RESET      = static_cast<signed char>(0x80);
static constexpr signed char MODENV_HOLD       = static_cast<signed char>(0x81);
static constexpr signed char MODENV_SET        = static_cast<signed char>(0x82);
static constexpr signed char MODENV_STOP       = static_cast<signed char>(0x83);
static constexpr signed char MODENV_MULTIPLIER = static_cast<signed char>(0x84);

static constexpr auto modulation_envelopes = ToJaggedArray<signed char>(
	{
		MODENV_MULTIPLIER,   1,   1,   2,   1,   0,  -1,  -2,  -3,  -4,  -3,  -2,  -1,
		MODENV_HOLD
	},
	{
		MODENV_MULTIPLIER,   1,   0,   1,   2,   1,   0,  -1,  -2,  -3,  -4,  -3,  -2,  -1,
		MODENV_HOLD
	},
	{
		MODENV_MULTIPLIER,   1,   0,   0,   0,   0,0x13,0x26,0x39,0x4C,0x5F,0x72,0x7F,0x72,
		MODENV_HOLD
	},
	{
		MODENV_MULTIPLIER,   1,   1,   2,   3,   2,   1,   0,  -1,  -2,  -3,  -2,  -1,   0,
		MODENV_SET, 2
	},
	{
		MODENV_MULTIPLIER,   1,   0,   0,   1,   3,   1,   0,  -1,  -3,  -1,   0,
		MODENV_SET, 4
	},
	{
		MODENV_MULTIPLIER,   1,   0,   0,   0,   0,   0,  10,  20,  30,  20,  10,   0, -10, -20, -30, -20, -10,
		MODENV_SET, 6
	},
	{
		MODENV_MULTIPLIER,   1,   0,   0,   0,   0,  22,  44,  66,  44,  22,   0, -22, -44, -66, -44, -22,
		MODENV_SET, 5
	},
	{
		MODENV_MULTIPLIER,   1,   1,   2,   3,   4,   3,   2,   1,   0,  -1,  -2,  -3,  -4,  -3,  -2,  -1,   0,
		MODENV_SET, 3
	}
);
#endif

std::optional<unsigned int> Track::DoModulationEnvelope()
{
	unsigned int accumulator = 0;

#ifdef SMPS_EnableModulationEnvelopes
	const unsigned int modulation_index = modulation_ctrl & 0x3F;

	if (modulation_index != 0)
	{
		const signed char* const envelope = modulation_envelopes[modulation_index - 1];

		for (;;)
		{
			const auto value = envelope[mod_env_index++];

			switch (value)
			{
				case MODENV_RESET:
					mod_env_index = 0;
					continue;

				case MODENV_HOLD:
					mod_env_index -= 2;
					continue;

				case MODENV_SET:
					mod_env_index = static_cast<unsigned char>(envelope[mod_env_index]);
					continue;

				case MODENV_STOP:
					SetResting(true);

					Visit(
						[](FMTrack &track)
						{
							track.NoteOff();
						},
						[](PSGTrack &track)
						{
							track.NoteOff();
						},
					#ifdef SMPS_EnablePWM
						[](PWMTrack&) {},
					#endif
						[](DACTrack&) {}
					);

					return std::nullopt;

				case MODENV_MULTIPLIER:
					mod_env_sens = static_cast<unsigned char>(envelope[mod_env_index++]);
					continue;
			}

			accumulator = value * mod_env_sens;
			break;
		}
	}
#endif

	accumulator += detune;
	accumulator += freq;

	if (IsModulationEnabled())
		accumulator += modulation_val;

	return accumulator;
}

bool Track::IsMusic() const
{
	// TODO: Replace this with something saner.
	return static_cast<const void*>(this) < static_cast<const void*>(&state->tracks.shared);
}

bool Track::IsPSG3MelodyTrack() const
{
	return GetPSGChannelID(channel_id) >= 0xC0
#ifdef SMPS_EnablePSGNoiseDrums
		&& !IsPSGDrumTrack()
#endif
		;
}

#ifdef SMPS_EnablePSGNoiseDrums
bool Track::IsPSGDrumTrack() const
{
	return this == &state->tracks.music.psg_drum;
}
#endif

void Track::ReloadModulationData()
{
	modulation = *modulation_ptr;
	modulation.steps /= 2; // TODO: Cast to unsigned integer?
	modulation_val = 0;
}

void Track::DoModulation()
{
	if (!IsModulationEnabled())
		return;

	if (!IsModulationInZ80Mode())
	{
		// SMPS 68k modulation.
		if (modulation.wait != 0)
		{
			--modulation.wait;
			return;
		}

		if (--modulation.speed != 0)
			return;

		modulation.speed = modulation_ptr->speed;

		if (modulation.steps == 0)
		{
			modulation.steps = modulation_ptr->steps;
			modulation.delta = -modulation.delta;
			return;
		}

		--modulation.steps;
		modulation_val += modulation.delta;
	}
	else
	{
		// SMPS Z80 modulation.
		if (--modulation.wait != 0)
			return;

		++modulation.wait;

		if (--modulation.speed == 0)
		{
			modulation.speed = modulation_ptr->speed;
			modulation_val += modulation.delta;
		}

		if (--modulation.steps != 0)
			return;

		modulation.steps = modulation_ptr->steps;
		modulation.delta = -modulation.delta;
	}
}

std::optional<unsigned char> Track::GetNoteOrDuration()
{
	unsigned char value;

	while ((value = *data_pointer++) >= 0xFE)
		if (!CoordFlag(value))
			return std::nullopt;

	return value;
}

/////////
// DAC //
/////////

void DACTrack::UpdateSample()
{
	if (freq == 0x80)
		return;

	const auto &sample = GetDACSampleMetadata(freq - 0x81);

	Z80BusFMSafe::Lock(
		[&](Z80BusFMSafe &z80_bus)
		{
			if (state->tracks.music.dac.IsFM6Overridden())
			{
				z80_bus.WriteFMI(0x2B, 0x80);
				z80_bus.WriteFMII(0xB6, ams_fms_pan);
			}

			SendDACSampleRequest(z80_bus, 0, sample);
		}
	);

	if (state->tracks.music.dac.IsFM6Overridden())
	{
		state->tracks.music.dac.SetFM6Overridden(false);
		state->tracks.music.FM6().SetFM6Overridden(true);
	}

#ifdef SMPS_SoundTest
	SetSoundTest(true);
#endif
}

bool DACTrack::DoNext()
{
	const auto note_or_duration = GetNoteOrDuration();

	if (!note_or_duration.has_value())
		return false;

	unsigned char value = *note_or_duration;

	if (value < 0x80)
	{
		SetDuration(value);
	}
	else
	{
		freq = value;

		value = *data_pointer++;

		if (value < 0x80)
		{
			SetDuration(value);
		}
		else
		{
			--data_pointer;
			duration_timeout = saved_duration;
		}
	}

	return true;
}

void DACTrack::UpdateTrackInternal(const bool begin_new_note)
{
	if (!begin_new_note)
		return;

	if (!DoNext())
		return;

	UpdateSample();
}

void DACTrack::ApplyVolumeAttenuation(ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
{
	WriteDACVolumeAttenuation(z80_bus, 0, std::min(0x7F, state->variables.fadein_counter * 4 + volume_attenuation) / 8);
}

////////
// FM //
////////

const Voice* FMTrack::GetVoices()
{
	return Visit(
		[](FMMusicTrack&) -> const Voice*
		{
			return state->variables.music_voices;
		},
		[](FMSFXTrack &track) -> const Voice*
		{
			return track.voices;
		},
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) -> const Voice* { std::unreachable(); },
	#endif
		[](DACTrack&) -> const Voice* { std::unreachable(); },
		[](PSGTrack&) -> const Voice* { std::unreachable(); }
	);
}

void FMTrack::SendVoiceTLCommon(Z80BusFMSafe &z80_bus, const Voice &voice, const bool force_upload)
{
	const unsigned int base_volume = std::min(0x7FU, volume_attenuation + static_cast<unsigned int>(state->variables.fadein_counter) * 2);

	for (unsigned int i = 0; i < 4; ++i)
	{
		const unsigned int value = voice[1 + 4 + i];

		if (force_upload || value >= 0x80)
			WriteFMIorII(z80_bus, 0x40 + i * 4, value < 0x80 ? value : std::min(0xFFU, value + base_volume));
	}
}

void FMTrack::SendVoiceTL(Z80BusFMSafe &z80_bus)
{
	if (IsOverridden())
		return;

	SendVoiceTLCommon(z80_bus, GetVoice(voice_index), false);
}

void FMTrack::SetVoice(Z80BusFMSafe &z80_bus)
{
	const Voice &voice = GetVoice(voice_index);

	WriteFMIorII(z80_bus, 0xB0, voice[0]);

	for (unsigned int i = 0; i < 4; ++i)
		WriteFMIorII(z80_bus, 0x30 + i * 4, voice[1 + i]);

	SendVoiceTLCommon(z80_bus, voice, true);

	for (unsigned int i = 0; i < 4 * 4; ++i)
		WriteFMIorII(z80_bus, 0x50 + i * 4, voice[1 + 4 + 4 + i]);

	WriteFMIorII(z80_bus, 0xB4, ams_fms_pan);
}

void FMTrack::SilenceChannel(Z80BusFMSafe &z80_bus)
{
	const auto write_regs = [this, &z80_bus](const unsigned int port, const unsigned int value)
	{
		for (unsigned int i = 0; i < 4; ++i)
			WriteFMIorII(z80_bus, port + i * 4, value);
	};

	write_regs(0x40, 0x7F);
	write_regs(0x80, 0x0F);
	SendNoteOff(z80_bus);
}

void FMTrack::SendNoteOff(Z80BusFMSafe &z80_bus)
{
	z80_bus.WriteFMI(0x28, GetFMChannelID(channel_id));
}

void FMTrack::NoteOff(Z80BusFMSafe &z80_bus)
{
	if (IsOverridden() || IsHeld())
		return;

	SendNoteOff(z80_bus);
}

void FMTrack::NoteOff()
{
	if (IsOverridden() || IsHeld())
		return;

	Z80BusFMSafe::Lock(
		[&](Z80BusFMSafe &z80_bus)
		{
			SendNoteOff(z80_bus);
		}
	);
}

void FMTrack::UpdateTrackInternal(const bool begin_new_note)
{
	if (begin_new_note)
	{
		SetHeld(false);

		if (!DoNext())
			return;

		if (IsResting())
			return;

		DoModulation();

		if (!PrepareNote())
			return;

		NoteOn();
	}
	else
	{
		if (IsResting())
			return;

		if (!NoteTimeoutUpdate())
			return;

		DoModulation();

		if (!UpdateFreq())
			return;
	}
}

bool FMTrack::UpdateFreq()
{
#ifdef SMPS_EnableModulationEnvelopes
	if (modulation_ctrl == 0)
#else
	if (!IsModulationEnabled())
#endif
		return true;

	if (!PrepareNote())
		return false;

	return true;
}

bool FMTrack::PrepareNote()
{
	if (IsOverridden())
		return true;

	const auto value = DoModulationEnvelope();

	if (!value.has_value())
		return false;

	Z80BusFMSafe::Lock(
		[&](Z80BusFMSafe &z80_bus)
		{
			WriteFMIorII(z80_bus, 0xA4, *value >> 8);
			WriteFMIorII(z80_bus, 0xA0, *value & 0xFF);
		}
	);

	return true;
}

bool FMTrack::DoNext()
{
	SetResting(false);

	const auto note_or_duration = GetNoteOrDuration();

	if (!note_or_duration.has_value())
		return false;

	unsigned char value = *note_or_duration;

	NoteOff();

	assert(value >= 0x80 || freq != 0);

	if (value < 0x80)
	{
		SetDuration(value);

		if (freq == 0)
		    SetResting(true);
	}
	else
	{
		SetFreq(value);

		value = *data_pointer++;

		if (value < 0x80)
			SetDuration(value);
		else
			--data_pointer;
	}

	FinishTrackUpdate();
	return true;
}

void FMTrack::NoteOn()
{
	if (IsOverridden())
		return;

	Z80BusFMSafe::Lock(
		[&](Z80BusFMSafe &z80_bus)
		{
			if (channel_id == ChannelID::FM6 && IsFM6Overridden())
			{
				z80_bus.WriteFMI(0x2B, 0);
				z80_bus.WriteFMII(0xB6, ams_fms_pan);
			}

			z80_bus.WriteFMI(0x28, 0xF0 | GetFMChannelID(channel_id));
		}
	);

	if (channel_id == ChannelID::FM6 && IsFM6Overridden())
	{
		state->tracks.music.FM6().SetFM6Overridden(false);
		state->tracks.music.dac.SetFM6Overridden(true);
	}
}

static constexpr std::array FMFrequencies = []() constexpr
{
	static constexpr auto MakeFMFrequency = [](const double frequency) constexpr
	{
		return our_lround(frequency * 0x400 * 0x400 * 2 / ClownMDSDK::MainCPU::FM::sample_rate);
	};
	
	// The first note is B, the last note is B-flat.
	constexpr auto fm_base_frequencies = std::to_array<double>({15.39, 16.35, 17.34, 18.36, 19.45, 20.64, 21.84, 23.13, 24.51, 25.98, 27.53, 29.15});
	constexpr unsigned int total_fm_octaves = 8;

	std::array<unsigned short, fm_base_frequencies.size() * total_fm_octaves> frequencies;

	for (std::size_t i = 0; i < frequencies.size(); ++i)
		frequencies[i] = MakeFMFrequency(fm_base_frequencies[i % fm_base_frequencies.size()]) + 0x800 * (i / fm_base_frequencies.size());

	return frequencies;
}();

void FMTrack::SetFreq(const unsigned char note)
{
	if (note == 0x80)
	{
		SetResting(true);
		freq = 0;
		return;
	}

	const unsigned char index = note - 0x80 + transpose;

	assert(index < 12*8);

	freq = FMFrequencies[index];
}

/////////
// PSG //
/////////

void PSGMelodyTrack::UpdateTrackInternal(const bool begin_new_note)
{
	if (begin_new_note)
	{
		SetHeld(false);

		if (!DoNext())
			return;

		if (IsResting())
			return;

		DoModulation();

		if (!DoNoteOn())
			return;

		if (!DoVolFX())
			return;
	}
	else
	{
		if (IsResting())
			return;

		if (!NoteTimeoutUpdate())
			return;

		if (!UpdateVolFX())
			return;

		DoModulation();

		if (!UpdateFreq())
			return;
	}
}

bool PSGTrack::UpdateFreq()
{
#ifdef SMPS_EnableModulationEnvelopes
	if (modulation_ctrl == 0)
#else
	if (!IsModulationEnabled())
#endif
		return true;

	return DoNoteOn();
}

bool PSGTrack::DoNoteOn()
{
	if (IsOverridden())
		return true;

	const auto value = DoModulationEnvelope();

	if (!value.has_value())
		return false;

	const ChannelID channel = channel_id == ChannelID::PSGNoise ? ChannelID::PSG3 : channel_id;

	ClownMDSDK::MainCPU::PSG::Write((*value & 0xF) | GetPSGChannelID(channel));
	ClownMDSDK::MainCPU::PSG::Write((*value & 0x3F0) >> 4);

	return true;
}

bool PSGTrack::DoNext()
{
	SetResting(false);

	const auto note_or_duration = GetNoteOrDuration();

	if (!note_or_duration.has_value())
		return false;

	unsigned char value = *note_or_duration;

	assert(value >= 0x80 || freq != static_cast<decltype(freq)>(-1));

	if (value < 0x80)
	{
		SetDuration(value);

		if (freq == static_cast<decltype(freq)>(-1))
		    SetResting(true);
	}
	else
	{
		SetFreq(value);

		value = *data_pointer++;

		if (value < 0x80)
			SetDuration(value);
		else
			--data_pointer;
	}

	FinishTrackUpdate();
	return true;
}

template<typename T, std::size_t N>
static constexpr auto MakePSGFrequencies(double (&&frequencies)[N])
{
	return [&frequencies]<std::size_t... I>(std::index_sequence<I...>) constexpr -> std::array<T, N>
	{
		static constexpr auto MakePSGFrequency = [](const double frequency) constexpr -> T
		{
			return std::min<T>(0x3FF, our_lround(ClownMDSDK::MainCPU::PSG::sample_rate / (frequency * 2)));
		};

		return {{MakePSGFrequency(frequencies[I])...}};
	}(std::make_index_sequence<N>{});
};

static constexpr auto PSGFrequencies = MakePSGFrequencies<unsigned int>({
	// This is the expanded PSG frequency table from Sonic 3 & Knuckles' driver.
	// 7 octaves, each one begins with C and ends with B.
	 109.34,    109.34,    109.34,    109.34,    109.34,    109.34,    109.34,    109.34,    109.34,    110.20,    116.76,    123.73,
	 130.98,    138.78,    146.99,    155.79,    165.22,    174.78,    185.19,    196.24,    207.91,    220.63,    233.52,    247.47,
	 261.96,    277.56,    293.59,    311.58,    329.97,    349.56,    370.39,    392.49,    415.83,    440.39,    468.03,    494.95,
	 522.71,    556.51,    588.73,    621.44,    661.89,    699.12,    740.79,    782.24,    828.59,    880.79,    932.17,    989.91,
	1045.42,   1107.52,   1177.47,   1242.89,   1316.00,   1398.25,   1491.47,   1575.50,   1669.55,   1747.82,   1864.34,   1962.46,
	2071.49,   2193.34,   2330.42,   2485.78,   2601.40,   2796.51,   2943.69,   3107.23,   3290.01,   3495.64,   3608.40,   3857.25,
	4142.98,   4302.32,   4660.85,   4863.50,   5084.56,   5326.69,   5887.39,   6214.47,   6580.02,   6991.28, 223721.56, 223721.56
});

void PSGTrack::SetFreq(const unsigned char note)
{
	if (note < 0x81)
	{
		SetResting(true);
		freq = -1;
		NoteOff();
		return;
	}

	const unsigned char index = note - 0x81 + transpose;

	assert(index < 12 * 7);

	freq = PSGFrequencies[index];
}

bool PSGTrack::UpdateVolFX()
{
	if (voice_index == 0)
		return true;

	if (!DoVolFX())
		return false;

	return true;
}

bool PSGTrack::DoVolFX()
{
	unsigned int psg_volume_attenuation = volume_attenuation;

	if (voice_index != 0)
	{
		const auto envelope = data.psg.list[voice_index - 1];

		for (;;)
		{
			const unsigned int value = envelope[vol_env_index++];

			switch (value)
			{
				case 0x80: // Reset
					vol_env_index = 0;
					continue;

				case 0x81: // Hold
					vol_env_index -= 2;
					continue;

				case 0x82: // Set
					vol_env_index = envelope[vol_env_index];
					continue;

				case 0x83: // Stop
					SetResting(true);
					NoteOff();
					return false;
			}

			psg_volume_attenuation += value << 3;
			psg_volume_attenuation = std::min(0xFU << 3, psg_volume_attenuation);
			break;
		}
	}

	SetVolumeAttenuation(psg_volume_attenuation);
	return true;
}

void PSGTrack::SetVolumeAttenuation(const unsigned int psg_volume_attenuation)
{
	if (IsResting() || IsOverridden())
		return;

	if (IsHeld() && note_timeout_master != 0 && note_timeout == 0)
		return;

	ClownMDSDK::MainCPU::PSG::Write(0x10 | GetPSGChannelID(channel_id) | std::min(0xFU, (psg_volume_attenuation + state->variables.fadein_counter * 4) >> 3));
}

void PSGTrack::SendNoteOff()
{
	SilencePSGChannel(channel_id);

	// Since the PSG3 track controls the noise channel, mute them both.
	if (channel_id == ChannelID::PSG3)
		SilencePSGChannel(ChannelID::PSGNoise);
}

void PSGTrack::NoteOff()
{
	if (IsOverridden())
		return;

	SendNoteOff();
}

////////////////////
// PSG Noise Drum //
////////////////////

#ifdef SMPS_EnablePSGNoiseDrums
void PSGDrumTrack::SetDrumNote(const unsigned char note)
{
	if (note < 0x81)
	{
		NoteOff();
		return;
	}

	auto &drum = data.psg_noise_drums.list[note - 0x81];

#ifdef SMPS_SoundTest
	freq = note;
#endif

	voice_index = drum.volume_envelope;
	volume_attenuation = state->variables.psg_drum_volume + (drum.volume_attenuation << 3);

	if (IsOverridden())
		return;

	ClownMDSDK::MainCPU::PSG::Write(drum.noise_mode);
}

bool PSGDrumTrack::DoNext()
{
	const auto note_or_duration = GetNoteOrDuration();

	if (!note_or_duration.has_value())
		return false;

	unsigned char value = *note_or_duration;

	if (value < 0x80)
	{
		SetDuration(value);
	}
	else
	{
		SetDrumNote(value);

		value = *data_pointer++;

		if (value < 0x80)
		{
			SetDuration(value);
		}
		else
		{
			--data_pointer;
			duration_timeout = saved_duration;
		}
	}

	FinishTrackUpdate();
	return true;
}

void PSGDrumTrack::UpdateTrackInternal(const bool begin_new_note)
{
	if (begin_new_note)
	{
		SetResting(false);

		if (!DoNext())
			return;
	}
	else
	{
		if (!UpdateVolFX())
			return;
	}
}
#endif

///////////
// Other //
///////////

static void InitMusicPlayback()
{
	// Save several values.
	const auto sndprio = state->variables.sndprio;
	const auto speedup = state->variables.speedup;
	const auto fadein_counter = state->variables.fadein_counter;
	const auto queue = state->variables.queue;

	// Clear variables.
	state->variables = {};

	// Restore the values saved above.
	state->variables.sndprio = sndprio;
	state->variables.speedup = speedup;
	state->variables.fadein_counter = fadein_counter;
	state->variables.queue = queue;

	// Clear music track RAM.
	state->tracks.music.Iterate(
		[](Track &track)
		{
			track = {};
		}
	);

	// Reset DAC volume.
	WriteDACVolumeAttenuation(0, 0);

	// InitMusicPlayback, and Sound_PlayBGM for that matter,
	// don't do a very good job of setting up the music tracks.
	// Tracks that aren't defined in a music file's header don't have
	// their channels defined, meaning .sendfmnoteoff won't silence
	// hardware properly. In combination with removing the below
	// calls to FMSilenceAll/PSGSilenceAll, this will cause hanging
	// notes.
	// To fix this, we'll just forcefully set all channels, here:
	static constexpr auto ChannelInitBytes = std::to_array<ChannelID>({
		ChannelID::DAC, ChannelID::FM1, ChannelID::FM2, ChannelID::FM3, ChannelID::FM4, ChannelID::FM5, ChannelID::FM6,
		ChannelID::PSG1, ChannelID::PSG2, ChannelID::PSG3,
	#ifdef SMPS_EnablePSGNoiseDrums
		ChannelID::PSGNoise,
	#endif
	#ifdef SMPS_EnablePWM
		ChannelID::PWM1, ChannelID::PWM2, ChannelID::PWM3, ChannelID::PWM4
	#endif
	});

	auto it = std::begin(ChannelInitBytes);
	state->tracks.music.Iterate(
		[&it](Track &track)
		{
			track.channel_id = *it++;
		}
	);
}

static void Sound_PlayBGM(const unsigned int id)
{
	const Music &music = data.music.list[id];

	if (music.extra_life_jingle)
	{
		if (!state->tracks.shared.IsMusicBackupActive())
		{
			state->tracks.shared.CreateMusicBackup(state->tracks.music, state->variables);
			state->variables.sndprio = 0;
		}
	}
	else
	{
		state->variables.fadein_counter = 0;
		state->variables.fadeout_counter = 0;

		if (state->tracks.shared.IsMusicBackupActive())
			state->tracks.shared.DestroyMusicBackup();
	}

	InitMusicPlayback();

	const unsigned char* const header = music.data;
	state->variables.speeduptempo = music.speed_up_tempo;

	if (music.slower_tempo_on_pal)
		state->variables.force_pal_tempo = true;

	if (music.ignore_speedup)
		state->variables.ignore_speedup = true;

	const auto voices_offset = ReadAlignedSignedWord(&header[0]);
	state->variables.music_voices =
#ifdef SMPS_EnableUniversalVoiceBank
		voices_offset == 0
		? data.universal_voice_bank
		:
#endif
		reinterpret_cast<const Voice*>(&header[voices_offset]);

	const unsigned int total_dac_and_fm_tracks = header[2];
	const unsigned int total_psg_tracks = header[3];
#ifdef SMPS_EnablePWM
	const unsigned int total_pwm_tracks = header[4];
#endif
	const unsigned char tempo_divider = header[6];
	const unsigned char tempo_mod = header[7];
	const unsigned char *track_header = &header[8];

	state->variables.tempo_mod = tempo_mod;
	state->variables.main_tempo = state->variables.speedup ? state->variables.speeduptempo : state->variables.tempo_mod;
	state->variables.main_tempo_timeout = 0;
	state->variables.pal_audio_countdown = 5;

	const auto &DoDACOrFM = [&](YM2612Track &track)
	{
		track.SetPlaying(true);
		track.tempo_divider = tempo_divider;
		track.stack_pointer = track.go_sub_stack.size();
		track.ams_fms_pan = 0xC0;
		track.duration_timeout = 1;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track.transpose = track_header[2];
		track.volume_attenuation = track_header[3];

		track_header += 4;
	};

	for (unsigned int i = 0; i < total_dac_and_fm_tracks; ++i)
	{
		if (i == 0)
			DoDACOrFM(state->tracks.music.dac);
		else
			DoDACOrFM(state->tracks.music.fm[i - 1]);
	}

	state->tracks.music.dac.SetFM6Overridden(true);
	state->tracks.music.FM6().SetFM6Overridden(true);

	for (unsigned int i = 0; i < total_psg_tracks; ++i)
	{
		Track &track = state->tracks.music.PSG(i);

		track.SetPlaying(true);
		track.tempo_divider = tempo_divider;
		track.stack_pointer = track.go_sub_stack.size();
		track.duration_timeout = 1;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track.transpose = track_header[2];
		track.volume_attenuation = track_header[3];
#ifdef SMPS_EnableModulationEnvelopes
		track.modulation_ctrl = track_header[4];
#endif
		track.voice_index = track_header[5];

		track_header += 6;
	}

#ifdef SMPS_EnablePWM
	for (unsigned int i = 0; i < total_pwm_tracks; ++i)
	{
		Track &track = state->tracks.music.pwm[i];

		track.SetPlaying(true);
		track.tempo_divider = tempo_divider;
		track.stack_pointer = track.go_sub_stack.size();
		track.duration_timeout = 1;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track.transpose = track_header[2];
		track.volume_attenuation = track_header[3];

		track_header += 4;
	}
#endif

	state->tracks.shared.VisitNonMusicTracks(
		[](auto &non_music_tracks)
		{
			non_music_tracks.sfx.Iterate(
				[](Track &track)
				{
					if (track.IsPlaying())
						GetMusicTrackForSFXChannel(track.channel_id).SetOverridden(true);
				}
			);

		#ifdef SMPS_EnableSpecSFX
			if (non_music_tracks.special_sfx.fm4.IsPlaying())
				state->tracks.music.FM4().SetOverridden(true);

			if (non_music_tracks.special_sfx.psg3.IsPlaying())
				state->tracks.music.PSG3().SetOverridden(true);
		#endif
		}
	);

	state->tracks.music.IterateWithZ80Bus(VisitHelper(
		[](FMTrack &track, Z80BusFMSafe &z80_bus)
		{
			if (!track.IsOverridden())
				track.SilenceChannel(z80_bus);

		},
		[](PSGTrack &track)
		{
			track.NoteOff();
		},
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) {},
	#endif
		[](DACTrack&, Z80BusFMSafe&) {}
	));

#ifdef SMPS_EnablePWM
	PWMSilenceAll();
#endif
}

static void Sound_PlaySFX(const unsigned int id)
{
	if (state->tracks.shared.IsMusicBackupActive())
	{
		state->variables.sndprio = 0;
		return;
	}

#ifdef SMPS_EnableSpinDashSFX
	state->spindash_lastsound = false;
#endif

	const SFX &sfx = data.sfx.list[id];

	const unsigned char* const header = sfx.data;

	const auto voices_offset = ReadAlignedSignedWord(&header[0]);
	const Voice* const voices =
#ifdef SMPS_EnableUniversalVoiceBank
		voices_offset == 0
		? data.universal_voice_bank
		:
#endif
		reinterpret_cast<const Voice*>(&header[voices_offset]);

	const unsigned char tempo_divider = header[2];
	const unsigned int total_tracks = header[3];
	const unsigned char *track_header = &header[4];

	switch (sfx.type)
	{
		case SFX::Type::Normal:
			break;

	#ifdef SMPS_PushSFXBehaviour
		case SFX::Type::BlockPush:
			if (state->push_playing)
				return;

			state->push_playing = true;
			break;
	#endif

	#ifdef SMPS_GloopSFXBehaviour
		case SFX::Type::Gloop:
			state->gloop_toggle = !state->gloop_toggle;

			if (!state->gloop_toggle)
				return;

			break;
	#endif

	#ifdef SMPS_EnableSpinDashSFX
		case SFX::Type::SpinDashRev:
		{
			const unsigned int pitch = state->spindash_timer == 0 ? 0 : state->spindash_pitch + 1;

			if (pitch < 12)
				state->spindash_pitch = pitch;

			state->spindash_lastsound = true;
			state->spindash_timer = 60;
			break;
		}
	#endif

	#ifdef SMPS_EnableContSFX
		case SFX::Type::Continuous:
			if (id == state->variables.current_contsfx)
			{
				state->continuous_sfx = true;
				state->variables.contsfx_channels = total_tracks;
				return;
			}
			else
			{
				state->continuous_sfx = false;
				state->variables.current_contsfx = id;
			}

			break;
	#endif
	}

	for (unsigned int i = 0; i < total_tracks; ++i)
	{
		const ChannelID channel_id = static_cast<ChannelID>(track_header[1]);

		GetMusicTrackForSFXChannel(channel_id).SetOverridden(true);

		if (IsPSG(channel_id))
		{
			SilencePSGChannel(channel_id);

			if (channel_id == ChannelID::PSG3)
				SilencePSGChannel(ChannelID::PSGNoise);
		}

		auto &track = GetSFXTrackForSFXChannel(channel_id);

		track = {};

		track.playback_control = track_header[0];
		track.channel_id = channel_id;
		track.tempo_divider = tempo_divider;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[2])];
		track.transpose = track_header[4];
		track.volume_attenuation = track_header[5];
		track.duration_timeout = 1;

#ifdef SMPS_EnableSpinDashSFX
		if (state->spindash_lastsound)
			track.transpose += state->spindash_pitch;
#endif

		track.stack_pointer = track.go_sub_stack.size();

		track.Visit(
			[](DACTrack &dac_track)
			{
				dac_track.ams_fms_pan = 0xC0;
			},
			[&voices](FMSFXTrack &fm_track)
			{
				fm_track.ams_fms_pan = 0xC0;
				fm_track.voices = voices;
			},
			[&voices](FMMusicTrack&)
			{
				std::unreachable();
			},
		#ifdef SMPS_EnablePWM
			[](PWMTrack&) {},
		#endif
			[](PSGTrack&) {}
		);

		track_header += 6;
	}

#ifdef SMPS_EnableSpecSFX
	State::Tracks::Shared::NonMusicTracks &non_music_tracks = state->tracks.shared.GetNonMusicTracks();

	if (non_music_tracks.sfx.FM4().IsPlaying())
		non_music_tracks.special_sfx.fm4.SetOverridden(true);

	if (non_music_tracks.sfx.PSG3().IsPlaying())
		non_music_tracks.special_sfx.psg3.SetOverridden(true);
#endif
}

#ifdef SMPS_EnableSpecSFX
static void Sound_PlaySpecial(const unsigned int id)
{
	if (state->tracks.shared.IsMusicBackupActive())
		return;

	State::Tracks::Shared::NonMusicTracks &non_music_tracks = state->tracks.shared.GetNonMusicTracks();

	const unsigned char* const header = data.background_sfx.list[id].data;

	const auto voices_offset = ReadAlignedSignedWord(&header[0]);
	const Voice* const voices =
#ifdef SMPS_EnableUniversalVoiceBank
		voices_offset == 0
		? data.universal_voice_bank
		:
#endif
		reinterpret_cast<const Voice*>(&header[voices_offset]);


	const unsigned char tempo_divider = header[2];
	const unsigned int total_tracks = header[3];
	const unsigned char *track_header = &header[4];

	for (unsigned int i = 0; i < total_tracks; ++i)
	{
		const ChannelID channel_id = static_cast<ChannelID>(track_header[1]);

		Track &track = [&]() -> Track&
		{
			if (IsFM(channel_id))
			{
				// FM
				state->tracks.music.FM4().SetOverridden(true);
				return non_music_tracks.special_sfx.fm4;
			}
			else if (IsPSG(channel_id))
			{
				// PSG
				state->tracks.music.PSG3().SetOverridden(true);
				return non_music_tracks.special_sfx.psg3;
			}
			else
			{
				std::unreachable();
			}
		}();

		track = {};

		track.playback_control = track_header[0];
		track.channel_id = channel_id;
		track.tempo_divider = tempo_divider;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[2])];
		track.transpose = track_header[4];
		track.volume_attenuation = track_header[5];
		track.duration_timeout = 1;
		track.stack_pointer = track.go_sub_stack.size();

		track.Visit(
			[&voices](DACTrack &dac_track)
			{
				dac_track.ams_fms_pan = 0xC0;
			},
			[&voices](FMSFXTrack &fm_track)
			{
				fm_track.ams_fms_pan = 0xC0;
				fm_track.voices = voices;
			},
			[&voices](FMMusicTrack&)
			{
				std::unreachable();
			},
		#ifdef SMPS_EnablePWM
			[](PWMTrack&) {},
		#endif
			[](PSGTrack&) {}

		);

		track_header += 6;
	}

	if (non_music_tracks.sfx.FM4().IsPlaying())
		non_music_tracks.special_sfx.fm4.SetOverridden(true);

	if (non_music_tracks.sfx.PSG3().IsPlaying())
		non_music_tracks.special_sfx.psg3.SetOverridden(true);

	if (!non_music_tracks.sfx.PSG3().IsPlaying())
	{
		SilencePSGChannel(ChannelID::PSG3);
		SilencePSGChannel(ChannelID::PSGNoise);
	}
}
#endif

static void Sound_PlayDAC(const unsigned int id)
{
	PlayDACSFX(1, id);
}

static void Sound_PlayCommand(const unsigned int id)
{
	switch (id)
	{
		case 1: // StopAllSound
			StopAllSound();
			break;

		case 2: // StopSFX
			state->variables.sndprio = false;

			state->tracks.shared.VisitNonMusicTracks(
				[](auto &non_music_tracks)
				{
					non_music_tracks.sfx.IterateWithZ80Bus(VisitHelper(
						[&non_music_tracks](FMTrack &track, Z80BusFMSafe &z80_bus)
						{
							if (track.IsPlaying())
							{
								track.SetPlaying(false);
								track.SetHeld(false);

								track.NoteOff(z80_bus);

							#ifdef SMPS_EnableSpecSFX
								auto &special_fm4 = non_music_tracks.special_sfx.fm4;
							#endif

								FMTrack &other_track =
								#ifdef SMPS_EnableSpecSFX
									track.channel_id == ChannelID::FM4 && special_fm4.IsPlaying() ? special_fm4 :
								#endif
									GetFMMusicTrackForSFXChannel(track.channel_id);

								if (other_track.IsOverridden())
								{
									other_track.SetOverridden(false);
									other_track.SetResting(true);
									other_track.SetVoice(z80_bus);
								}
							}
						},
						[](PSGMelodyTrack &track)
						{
							if (track.IsPlaying())
							{
								track.SetPlaying(false);
								track.SetHeld(false);

								track.NoteOff();

								PSGMelodyTrack &other_track = GetPSGMusicTrackForSFXChannel(track.channel_id);

								if (other_track.IsOverridden())
								{
									other_track.SetOverridden(false);
									other_track.SetResting(true);
								}
							}
						},
						[&non_music_tracks](PSG3MelodyTrack &track)
						{
							if (track.IsPlaying())
							{
								track.SetPlaying(false);
								track.SetHeld(false);

								track.NoteOff();

							#ifdef SMPS_EnableSpecSFX
								auto &special_psg3 = non_music_tracks.special_sfx.psg3;
							#endif

								PSG3MelodyTrack &other_track =
								#ifdef SMPS_EnableSpecSFX
									special_psg3.IsPlaying() ? special_psg3 :
								#endif
									state->tracks.music.PSG3();

								if (other_track.IsOverridden())
								{
									other_track.SetOverridden(false);
									other_track.SetResting(true);

									if (other_track.channel_id == ChannelID::PSGNoise)
										ClownMDSDK::MainCPU::PSG::Write(other_track.psg_noise);
								}
							}
						},
					#ifdef SMPS_EnablePSGNoiseDrums
						[](PSGDrumTrack&)
						{
							std::unreachable();
						},
					#endif
						[](DACTrack&, Z80BusFMSafe&)
						{
							std::unreachable();
						}
					));
				}
			);

			break;

	#ifdef SMPS_EnableSpecSFX
		case 3: // SpecStopSFX
			state->tracks.shared.VisitNonMusicTracks(
				[](auto &non_music_tracks)
				{
					FMTrack &background_sfx_fm4 = non_music_tracks.special_sfx.fm4;

					if (background_sfx_fm4.IsPlaying())
					{
						background_sfx_fm4.SetPlaying(false);

						if (!background_sfx_fm4.IsOverridden())
						{
							Z80BusFMSafe::Lock(
								[&](Z80BusFMSafe &z80_bus)
								{
									background_sfx_fm4.SendNoteOff(z80_bus);

									FMTrack &music_fm4 = state->tracks.music.FM4();
									music_fm4.SetOverridden(false);
									music_fm4.SetResting(true);

									if (music_fm4.IsPlaying())
										music_fm4.SetVoice(z80_bus);
								}
							);
						}
					}

					PSG3MelodyTrack &background_sfx_psg3 = non_music_tracks.special_sfx.psg3;

					if (background_sfx_psg3.IsPlaying())
					{
						background_sfx_psg3.SetPlaying(false);

						if (!background_sfx_psg3.IsOverridden())
						{
							background_sfx_psg3.SendNoteOff();

							PSG3MelodyTrack &music_psg3 = state->tracks.music.PSG3();
							music_psg3.SetOverridden(false);
							music_psg3.SetResting(true);

							if (music_psg3.IsPlaying())
								if (music_psg3.channel_id == ChannelID::PSGNoise)
									ClownMDSDK::MainCPU::PSG::Write(music_psg3.psg_noise);
						}
					}
				}
			);

			break;
		#endif

		case 4: // StopDACSFX
#ifdef __MEGA_DRIVE__
			ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
				[&](ClownMDSDK::MainCPU::Z80::Bus &z80_bus)
				{
					z80_bus.RAM(zRequestFlag) = z80_scf_instruction;
					// 'Stop PCM channel' command value
					z80_bus.RAM(zRequestChannel2) = 2;
				}
			);
#endif
			break;

		case 5: // FadeOutMusic
			state->variables.fadeout_delay = 3;
			state->variables.fadeout_counter = 0x28;
			state->variables.speedup = false;
			break;

		case 6: // SpeedUpMusic
			state->tracks.shared.Visit(
				[](State::Tracks::Shared::MusicBackup &music_backup)
				{
					music_backup.variables.main_tempo = music_backup.variables.speeduptempo;
					music_backup.variables.speedup = true;
				},
				[](State::Tracks::Shared::NonMusicTracks&)
				{
					state->variables.main_tempo = state->variables.speeduptempo;
					state->variables.speedup = true;
				}
			);
			
			break;

		case 7: // SlowDownMusic
			state->tracks.shared.Visit(
				[](State::Tracks::Shared::MusicBackup &music_backup)
				{
					music_backup.variables.main_tempo = music_backup.variables.tempo_mod;
					music_backup.variables.speedup = false;
				},
				[](State::Tracks::Shared::NonMusicTracks&)
				{
					state->variables.main_tempo = state->variables.speeduptempo;
					state->variables.speedup = false;
				}
			);
			
			break;
	}
}

static void PlaySoundID(const unsigned int id)
{
	if (id >= data.music.begin && id < data.music.end)
		Sound_PlayBGM(id - data.music.begin);
	else if (id >= data.sfx.begin && id < data.sfx.end)
		Sound_PlaySFX(id - data.sfx.begin);
#ifdef SMPS_EnableSpecSFX
	else if (id >= data.background_sfx.begin && id < data.background_sfx.end)
		Sound_PlaySpecial(id - data.background_sfx.begin);
#endif
	else if (id >= data.dac.begin && id < data.dac.end)
		Sound_PlayDAC(id - data.dac.begin);
	else
		Sound_PlayCommand(id);
}

static void CycleSoundQueue()
{
	for (auto &queue_slot : state->variables.queue)
	{
		const unsigned int id = queue_slot;
		queue_slot = 0;

		if (id == 0)
			continue;

		const std::optional<Priority> priority = [&]() -> std::optional<Priority>
		{
			if (id >= data.sfx.begin && id < data.sfx.end)
				return data.sfx.list[id - data.sfx.begin].priority;
		#ifdef SMPS_EnableSpecSFX
			else if (id >= data.background_sfx.begin && id < data.background_sfx.end)
				return data.background_sfx.list[id - data.background_sfx.begin].priority;
		#endif
			else
				return std::nullopt;
		}();

		if (priority.has_value() && !priority->do_not_save)
		{
			if (priority->value < state->variables.sndprio)
				continue;

			state->variables.sndprio = priority->value;
		}

		PlaySoundID(id);
	}
}

static void TempoWait()
{
	const unsigned int tempo_sum = state->variables.main_tempo + state->variables.main_tempo_timeout;
	state->variables.main_tempo_timeout = tempo_sum % 0x100;

	if (tempo_sum >= 0x100)
	{
		state->tracks.music.Iterate(
			[](Track &track)
			{
				++track.duration_timeout;
			}
		);
	}
}

static void UpdateMusic();

static void DoMusicTempoSpeedup()
{
	// Handle the tempo modifier, as used by Sonic 3 & Knuckles.
	// TODO: Backing-up and other stuff from the S&K driver.
	if (!state->variables.ignore_speedup && state->music_tempo_modifier_master != 0 && state->music_tempo_modifier-- == 0)
	{
		state->music_tempo_modifier = state->music_tempo_modifier_master;
		UpdateMusic();
	}
}

static void UpdateMusic()
{
	state->tracks.music.Update();

	TempoWait();

	// Do the speed-up that S&K's driver does in its music update function.
	DoMusicTempoSpeedup();

	if (state->variables.fade_to_previous_pending)
	{
		// Restore music backup.
		const auto &music_backup = state->tracks.shared.GetMusicBackup();
		state->tracks.music = music_backup.tracks;
		state->variables = music_backup.variables;
		state->tracks.shared.DestroyMusicBackup();

		// Begin fade-in.
		state->variables.fadein_counter = 0x28;

		// Mark music tracks as resting and update the sound hardware to suit them.
		state->tracks.music.IterateWithZ80Bus(VisitHelper(
			[](DACTrack &track, Z80BusFMSafe &z80_bus)
			{
				if (track.IsPlaying())
					track.ApplyVolumeAttenuation(z80_bus);
			},
			[](FMTrack &track, Z80BusFMSafe &z80_bus)
			{
				if (track.IsPlaying())
				{
					track.SetResting(true);
					track.SetVoice(z80_bus);
				}
			},
			[](PSGTrack &track)
			{
				if (track.IsPlaying())
				{
					track.SetResting(true);
					track.NoteOff();
				}
			},
			[](PSG3MelodyTrack &track)
			{
				if (track.IsPlaying())
				{
					track.SetResting(true);
					track.NoteOff();

					if (track.channel_id == ChannelID::PSGNoise)
						ClownMDSDK::MainCPU::PSG::Write(track.psg_noise);
				}
			},
		#ifdef SMPS_EnablePWM
			[](PWMTrack&) {},
		#endif
			[](DACTrack&) {},
			[](FMTrack&) {}
		));

		state->tracks.music.dac.SetFM6Overridden(true);
		state->tracks.music.FM6().SetFM6Overridden(true);
	}
}

static void DoFadeIn()
{
	if (state->variables.fadein_delay != 0)
	{
		--state->variables.fadein_delay;
		return;
	}

	--state->variables.fadein_counter;
	state->variables.fadein_delay = 2;

	state->tracks.IterateWithZ80Bus(VisitHelper(
		[](DACTrack &track, Z80BusFMSafe &z80_bus)
		{
			if (track.IsPlaying())
				track.ApplyVolumeAttenuation(z80_bus);
		},
		[](FMTrack &track, Z80BusFMSafe &z80_bus)
		{
			if (track.IsPlaying())
				track.SendVoiceTL(z80_bus);
		},
		[](PSGTrack &track)
		{
			if (track.IsPlaying())
			{
				// If a volume envelope is active, then it will handle updating the volume for us.
				// Doing it manually will just conflict with it.
				if (track.voice_index == 0)
					track.SetVolumeAttenuation(track.volume_attenuation);
			}
		},
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) {},
	#endif
		[](DACTrack&) {},
		[](FMTrack&) {}
	));
}

static void DoFadeOut()
{
	if (state->variables.fadeout_delay != 0)
	{
		--state->variables.fadeout_delay;
		return;
	}

	if (--state->variables.fadeout_counter == 0)
	{
		StopAllSound();
		return;
	}

	state->variables.fadeout_delay = 3;

	state->tracks.music.IterateWithZ80Bus(VisitHelper(
		[](DACTrack &track, Z80BusFMSafe &z80_bus)
		{
			if (track.IsPlaying())
			{
				track.volume_attenuation += 4;

				if (track.volume_attenuation >= 0x80)
					track.SetPlaying(false);
				else
					track.ApplyVolumeAttenuation(z80_bus);
			}
		},
		[](FMTrack &track, Z80BusFMSafe &z80_bus)
		{
			if (track.IsPlaying())
			{
				++track.volume_attenuation;

				if (track.volume_attenuation >= 0x80)
					track.SetPlaying(false);
				else
					track.SendVoiceTL(z80_bus);
			}
		},
		[](PSGMelodyTrack &track)
		{
			if (track.IsPlaying())
			{
				track.volume_attenuation += 4;

				if (track.volume_attenuation >= 0x80)
				{
					track.SetPlaying(false);
				}
				else
				{
					// If a volume envelope is active, then it will handle updating the volume for us.
					// Doing it manually will just conflict with it.
					if (track.voice_index == 0)
						track.SetVolumeAttenuation(track.volume_attenuation);
				}
			}
		},
	#ifdef SMPS_EnablePSGNoiseDrums
		[](PSGDrumTrack &track)
		{
			if (track.IsPlaying())
			{
				state->variables.psg_drum_volume += 4;

				if (state->variables.psg_drum_volume >= 0x80)
					track.SetPlaying(false);
			}
		},
	#endif
	#ifdef SMPS_EnablePWM
		[](PWMTrack&) {},
	#endif
		[](DACTrack&) {},
		[](FMTrack&) {}
	));
}

static void HandlePause()
{
	if (state->pause < 0x80)
	{
		if (state->pause == 2)
			return;

		state->pause = 2;

		Z80BusFMSafe::Lock(
			[](Z80BusFMSafe &z80_bus)
			{
				SilenceAll(z80_bus);
			}
		);
	}
	else
	{
		state->pause = 0;

		Z80BusFMSafe::Lock(
			[&](Z80BusFMSafe &z80_bus)
			{
				state->tracks.Iterate(VisitHelper(
					[&z80_bus](DACTrack &track)
					{
						// Apply DAC panning if necessary.
						if (track.IsPlaying() && !track.IsOverridden() && !track.IsFM6Overridden())
							z80_bus.WriteFMII(0xB6, track.ams_fms_pan);
					},
					[&z80_bus](FMTrack &track)
					{
						if (track.IsPlaying() && !track.IsOverridden())
							track.SetVoice(z80_bus);
					},
				#ifdef SMPS_EnablePWM
					[](PWMTrack&) {},
				#endif
					[](PSGTrack&) {}
				));

#ifdef __MEGA_DRIVE__
				z80_bus.RAM(zRequestFlag) = z80_scf_instruction;
				// 'Play sample' command value
				z80_bus.RAM(zRequestChannel1) = 1;
				z80_bus.RAM(zRequestChannel2) = 1;
#endif
			}
		);
	}
}

STARTING_FUNCTION void SMPS::UpdateDriver()
{
	if (state->pause != 0)
	{
		HandlePause();
		return;
	}

	if (state->variables.fadeout_counter != 0)
		DoFadeOut();

	if (state->variables.fadein_counter != 0)
		DoFadeIn();

	if (state->variables.queue.IsAnythingQueued())
		CycleSoundQueue();

#ifdef SMPS_EnableSpinDashSFX
	if (state->spindash_timer != 0)
		--state->spindash_timer;
#endif

	// Do the speed-up that S&K's driver does in its SFX update function.
	DoMusicTempoSpeedup();

	// Update the music twice on some frames so that music is the same speed on NTSC and PAL consoles.
	if (state->pal && !state->variables.force_pal_tempo && --state->variables.pal_audio_countdown == 0)
	{
		state->variables.pal_audio_countdown = 5;
		UpdateMusic();
	}

	UpdateMusic();

	state->tracks.shared.UpdateNonMusicTracks();
}

// TODO: Proper sign extension considering alternate integer sizes!
// TODO: The packing of pointers in the track struct!
// TODO: Check sign-ness of track values.
// TODO: Check if variable accumulations need bitmasks.
// TODO: Clean-up 'IsPSG' calls.
// TODO: Make IsPlaying and all that into proper 1-bit booleans.
// TODO: Constants for DAC driver commands.
