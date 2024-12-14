#include "core.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>

using namespace SMPS;

#ifdef __MEGA_DRIVE__
#include "dac/DAC-Driver.h"
#endif

#ifdef BINARY_BLOB
register Data* data asm("a5");
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

	template<typename Self>
	constexpr auto operator[](this Self &&self, const std::size_t index)
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

static const auto SFX_BGMChannelRAM = std::to_array<unsigned char>({
	MUSIC_FM3,
	0, // Padding
	MUSIC_FM4,
	MUSIC_FM5,
	MUSIC_PSG1,
	MUSIC_PSG2,
	MUSIC_PSG3, // Plain PSG3
	MUSIC_PSG3, // Noise
});

static const auto SFX_SFXChannelRAM = std::to_array<unsigned char>({
	SFX_FM3,
	0, // Padding
	SFX_FM4,
	SFX_FM5,
	SFX_PSG1,
	SFX_PSG2,
	SFX_PSG3, // Plain PSG3
	SFX_PSG3, // Noise
});

///////////////
// Non-Track //
///////////////

static void FMSilenceAll(FMSafeZ80Bus &z80_bus)
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

static void PSGSilenceAll()
{
	// Set all channels to maximum attenuation.
	ClownMDSDK::PSG::Write(0x80 | 0x1F); // PSG1
	ClownMDSDK::PSG::Write(0xA0 | 0x1F); // PSG2
	ClownMDSDK::PSG::Write(0xC0 | 0x1F); // PSG3
	ClownMDSDK::PSG::Write(0xE0 | 0x1F); // PSG Noise
}

static void SilenceAll(FMSafeZ80Bus &z80_bus)
{
#ifdef __MEGA_DRIVE__
	z80_bus.ram[zRequestFlag] = z80_scf_instruction;
	// 'Stop PCM channel' command value
	z80_bus.ram[zRequestChannel1] = 2;
	z80_bus.ram[zRequestChannel2] = 2;
#endif

	FMSilenceAll(z80_bus);
	PSGSilenceAll();
#ifdef SMPS_EnablePWM
	PWMSilenceAll();
#endif
}

static void StopAllSound()
{
	State &state = data->state;

	state.variables = {};
	for (Track *track = &state.tracks[NON_BACKUP_TRACKS_BEGIN]; track != &state.tracks[NON_BACKUP_TRACKS_END]; ++track)
		*track = {};

	FMSafeZ80Bus z80_bus;

	// FM3/FM6 normal mode, disable timers.
	z80_bus.WriteFMI(0x27, 0);

	SilenceAll(z80_bus);
}

static const DACSample& GetDACSampleMetadata(const unsigned int id)
{
	return data->dac.list[id];
}

static void SendDACSampleRequest(ClownMDSDK::Z80::Bus &bus, const unsigned int channel, const DACSample &sample)
{
#ifdef __MEGA_DRIVE__
	// TODO: Assembly?
	const unsigned int offset = channel != 0 ? zRequestChannel2 : zRequestChannel1;
	bus.ram[zRequestFlag] = z80_scf_instruction;
	// 'Play sample' command value
	bus.ram[offset + 0] = 1;
	// Copy the sample's metadata
	bus.ram[offset + 1] = sample.bank_offset.upper;
	bus.ram[offset + 2] = sample.bank_offset.lower;
	bus.ram[offset + 3] = sample.playback_increment.upper;
	bus.ram[offset + 4] = sample.playback_increment.lower;
	bus.ram[offset + 5] = sample.bank_index;
#endif
}

static void PlayDACSFX(const unsigned int dac_channel, const unsigned int dac_index)
{
	State &state = data->state;

	state.tracks[MUSIC_DAC].SetFM6Overridden(true);
	state.tracks[MUSIC_FM6].SetFM6Overridden(true);

	const auto &sample = GetDACSampleMetadata(dac_index);

	FMSafeZ80Bus z80_bus;

	// Enable DAC.
	z80_bus.WriteFMI(0x2B, 0x80);

	// Force L/R panning.
	z80_bus.WriteFMII(0xB6, 0xC0);

	SendDACSampleRequest(z80_bus, dac_channel, sample);
#ifdef __MEGA_DRIVE__
	// This is a DAC SFX: set to full volume
	z80_bus.ram[dac_channel != 0 ? zSample2Volume : zSample1Volume] = zSampleLookup >> 8;
#endif
}

static void WriteDACVolume(ClownMDSDK::Z80::Bus &z80_bus, const unsigned int volume)
{
#ifdef __MEGA_DRIVE__
	z80_bus.ram[zSample1Volume] = volume | zSampleLookup >> 8;
#endif
}

static void WriteDACVolume(const unsigned int volume)
{
	ClownMDSDK::Z80::Bus z80_bus;
	WriteDACVolume(z80_bus, volume);
}

/////////////
// Generic //
/////////////

void Track::WriteFMIorII(FMSafeZ80Bus &z80_bus, const unsigned char port, const unsigned char value)
{
	const unsigned char complete_port = port | (voice_control & 3);

	if ((voice_control & (1 << 2)) == 0)
		z80_bus.WriteFMI(complete_port, value);
	else
		z80_bus.WriteFMII(complete_port, value);
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

	if (IsPSG())
		vol_env_index = 0;

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
	State &state = data->state;

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

			if (IsPSG())
				break;

			ams_fms_pan = (ams_fms_pan & 0x37) | value;

			if (IsOverridden() || IsFM6Overridden())
				break;

			FMSafeZ80Bus z80_bus;
			WriteFMIorII(z80_bus, 0xB4, ams_fms_pan);
			break;
		}

		case 1: // cfDetune
			detune = *data_pointer++;
			break;

		case 2: // cfSetCommunication
			state.variables.communication_byte = *data_pointer++;
			break;

		case 3: // cfJumpReturn
			data_pointer = go_sub_stack[stack_pointer++];
			break;

		case 4: // cfFadeInToPrevious
			// We must handle this later to prevent the DAC channel from desynchronising.
			state.variables.fade_to_previous_pending = true;
			return false;

		case 5: // cfSetTempoDivider
			tempo_divider = *data_pointer++;
			break;

		case 6: // cfChangeFMVolume
		{
			const unsigned int value = *data_pointer++;

			if (IsPSG())
				break;

			volume += value;

#ifdef SMPS_EnablePWM
			if (IsPWM())
				break;
#endif

			if (IsDAC())
			{
				ClownMDSDK::Z80::Bus z80_bus;
				SetDACVolume(z80_bus);
			}
			else
			{
				FMSafeZ80Bus z80_bus;
				SendVoiceTL(z80_bus);
			}

			break;
		}

#ifdef SMPS_EnableSpecSFX
		case 7: // cfStopSpecialFM4
		{
			SetHeld(false);
			SetPlaying(false);

			FMSafeZ80Bus z80_bus;

			FMNoteOff(z80_bus);

			if (state.tracks[SFX_FM4].IsPlaying())
				break;

			Track &music_fm4 = state.tracks[MUSIC_FM4];
			music_fm4.SetOverridden(false);
			music_fm4.SetResting(true);

			music_fm4.SetVoice(z80_bus);

			return false;
		}
#endif

		case 8: // cfNoteTimeout
			note_timeout = note_timeout_master = *data_pointer++;
			break;

		case 9: // cfChangeTransposition
			transpose += *data_pointer++;
			break;

		case 0xA: // cfSetTempo
			state.variables.main_tempo = state.variables.main_tempo_timeout = *data_pointer++;
			break;

		case 0xB: // cfSetTempoDividerAll
		{
			const unsigned char value = *data_pointer++;

			for (Track *track = &state.tracks[MUSIC_BEGIN]; track != &state.tracks[MUSIC_END]; ++track)
				track->tempo_divider = value;

			break;
		}

		case 0xC: // cfChangePSGVolume
			volume += *data_pointer++;
			break;

		case 0xD: // cfSetVoice
			voice_index = *data_pointer++;

			if (IsPSG())
				break;

			if (IsOverridden())
				break;

			{
				FMSafeZ80Bus z80_bus;

				FMSilenceChannel(z80_bus);
				SetVoice(z80_bus);
			}
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
			if (!IsPSG())
			{
				FMSafeZ80Bus z80_bus;
				FMSilenceChannel(z80_bus);
			}
			[[fallthrough]];
#ifndef SMPS_EnableSpecSFX
		case 7: // cfStopSpecialFM4
#endif
		case 0x10: // cfStopTrack
			SetHeld(false);
			SetPlaying(false);

			if (IsDAC())
				return false;
#ifdef SMPS_EnablePWM
			if (IsPWM())
				return false;
#endif
			if (IsPSG())
				PSGNoteOff();
			else
				FMNoteOff();

			if (IsMusic())
				return false;

			state.variables.sndprio = 0;

			if (!IsPSG())
			{
				Track *track;
#ifdef SMPS_EnableSpecSFX
				auto &special_fm4 = state.tracks[SPECIAL_SFX_FM4];

				if (voice_control == 4 && special_fm4.IsPlaying())
				{
					track = &special_fm4;
				}
				else
#endif
				{
					track = &state.tracks[SFX_BGMChannelRAM[voice_control - 2]];

					if (!track->IsPlaying())
						return false;
				}

				track->SetOverridden(false);
				track->SetResting(true);

				FMSafeZ80Bus z80_bus;
				track->SetVoice(z80_bus);
			}
			else
			{
				Track *track;
#ifdef SMPS_EnableSpecSFX
				auto &special_psg3 = state.tracks[SPECIAL_SFX_PSG3];

				if ((voice_control == 0xC0 || voice_control == 0xE0) && special_psg3.IsPlaying())
					track = &special_psg3;
				else
#endif
					track = &state.tracks[SFX_BGMChannelRAM[voice_control >> 5]]; // TODO: Cast to unsigned integer?

				track->SetOverridden(false);
				track->SetResting(true);

				if (track->voice_control == 0xE0)
					ClownMDSDK::PSG::Write(track->psg_noise);
			}

			return false;

		case 0x11: // cfSetPSGNoise
		{
			psg_noise = *data_pointer++;

			unsigned int value = psg_noise;
			voice_control = 0xE0;

			if (value == 0)
			{
				value = 0xFF;
				voice_control = 0xC0;
			}

			if (!IsOverridden())
				ClownMDSDK::PSG::Write(value);

			break;
		}

		case 0x12: // cfDisableModulation
			SetModulationEnabled(false);
			break;

		case 0x13: // cfSetPSGTone
		{
			const unsigned char value = *data_pointer++;

			if (!IsPSG())
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
			{
				FMSafeZ80Bus z80_bus;
				WriteFMIorII(z80_bus, data_pointer[0], data_pointer[1]);
			}
			data_pointer += 2;
			break;

		case 0x19: // cfPlayDACSample
			PlayDACSFX(0, *data_pointer++ - 1);
			break;

		case 0x1A: // cfPlaySound
			state.variables.queue[1] = *data_pointer++;
			break;

		case 0x1B: // cfSetKey
			transpose = *data_pointer++;
			break;

		case 0x1C: // cfSetVolume
		{
			const unsigned int value = *data_pointer++ ^ 0xFF;

			if (IsPSG())
			{
				volume = value & 0x78;
			}
			else
			{
				volume = value & 0x7F;

				FMSafeZ80Bus z80_bus;
				SendVoiceTL(z80_bus);
			}

			break;
		}

		case 0x1D: // cfNoteTimeoutS3K
			note_timeout = note_timeout_master = *data_pointer++ * tempo_divider; // TODO: Cast to unsigned int?
			break;

		case 0x1E: // cfLoopContinuousSFX
#ifdef SMPS_EnableContSFX
			if (!state.continuous_sfx)
			{
				state.variables.current_contsfx = 0;
				data_pointer += 2;
			}
			else
			{
				if (--state.variables.contsfx_channels == 0)
					state.continuous_sfx = false;

				cfJumpTo();
			}
#else
			data_pointer += 2;
#endif
			break;

		case 0x1F: // cfClearPush
#ifdef SMPS_PushSFXBehaviour
			state.push_playing = false;
#endif
			break;

		case 0x20: // cfSendFMI
			{
				FMSafeZ80Bus z80_bus;
				z80_bus.WriteFMI(data_pointer[0], data_pointer[1]);
			}
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
	}

	return true;
}

bool Track::NoteTimeoutUpdate()
{
	if (note_timeout == 0 || --note_timeout != 0)
		return true;

	SetResting(true);

	if (IsPSG())
		PSGNoteOff();
	else
		FMNoteOff();

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

					if (IsPSG())
						PSGNoteOff();
					else
						FMNoteOff();

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
	return this < &data->state.tracks[SFX_BEGIN];
}

void Track::ReloadModulationData()
{
	modulation.wait  = modulation_ptr->wait;
	modulation.speed = modulation_ptr->speed;
	modulation.delta = modulation_ptr->delta;
	modulation.steps = modulation_ptr->steps / 2; // TODO: Cast to unsigned integer?
	modulation_val = 0;
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

void Track::DACUpdateSample()
{
	if (saved_dac == 0x80)
		return;

	State &state = data->state;

	const auto &sample = GetDACSampleMetadata(saved_dac - 0x81);

	{
		FMSafeZ80Bus z80_bus;

		if (state.tracks[MUSIC_DAC].IsFM6Overridden())
		{
			z80_bus.WriteFMI(0x2B, 0x80);
			z80_bus.WriteFMII(0xB6, ams_fms_pan);
		}

		SendDACSampleRequest(z80_bus, 0, sample);
	}

	if (state.tracks[MUSIC_DAC].IsFM6Overridden())
	{
		state.tracks[MUSIC_DAC].SetFM6Overridden(false);
		state.tracks[MUSIC_FM6].SetFM6Overridden(true);
	}

#ifdef SMPS_SoundTest
	SetSoundTest(true);
#endif
}

bool Track::DACDoNext()
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
		saved_dac = value;

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

void Track::DACUpdateTrack()
{
#ifdef SMPS_SoundTest
	SetSoundTest(false);
#endif

	if (--duration_timeout != 0)
		return;

	if (!DACDoNext())
		return;

	DACUpdateSample();
}

void Track::SetDACVolume(ClownMDSDK::Z80::Bus &z80_bus)
{
	State &state = data->state;
	
	WriteDACVolume(z80_bus, std::min(0x7F, state.variables.fadein_counter * 4 + volume) / 8);
}

////////
// FM //
////////

void Track::SendVoiceTLCommon(FMSafeZ80Bus &z80_bus, const Voice &voice, const bool force_upload)
{
	const unsigned int base_volume = std::min(0x7FU, volume + static_cast<unsigned int>(data->state.variables.fadein_counter) * 2);

	for (unsigned int i = 0; i < 4; ++i)
	{
		const unsigned int value = voice[1 + 4 + i];

		if (force_upload || value >= 0x80)
			WriteFMIorII(z80_bus, 0x40 + i * 4, value < 0x80 ? value : std::min(0xFFU, value + base_volume));
	}
}

void Track::SendVoiceTL(FMSafeZ80Bus &z80_bus)
{
	if (IsOverridden())
		return;

	SendVoiceTLCommon(z80_bus, voice_ptr[voice_index], false);
}

void Track::SetVoice(FMSafeZ80Bus &z80_bus)
{
	const Voice &voice = voice_ptr[voice_index];

	WriteFMIorII(z80_bus, 0xB0, voice[0]);

	for (unsigned int i = 0; i < 4; ++i)
		WriteFMIorII(z80_bus, 0x30 + i * 4, voice[1 + i]);

	SendVoiceTLCommon(z80_bus, voice, true);

	for (unsigned int i = 0; i < 4 * 4; ++i)
		WriteFMIorII(z80_bus, 0x50 + i * 4, voice[1 + 4 + 4 + i]);

	WriteFMIorII(z80_bus, 0xB4, ams_fms_pan);
}

void Track::FMSilenceChannel(FMSafeZ80Bus &z80_bus)
{
	const auto write_regs = [this, &z80_bus](const unsigned int port, const unsigned int value)
	{
		for (unsigned int i = 0; i < 4; ++i)
			WriteFMIorII(z80_bus, port + i * 4, value);
	};

	write_regs(0x40, 0x7F);
	write_regs(0x80, 0x0F);
	SendFMNoteOff(z80_bus);
}

void Track::SendFMNoteOff(FMSafeZ80Bus &z80_bus)
{
	z80_bus.WriteFMI(0x28, voice_control);
}

void Track::FMNoteOff(FMSafeZ80Bus &z80_bus)
{
	if (IsOverridden() || IsHeld())
		return;

	SendFMNoteOff(z80_bus);
}

void Track::FMNoteOff()
{
	if (IsOverridden() || IsHeld())
		return;

	FMSafeZ80Bus z80_bus;
	SendFMNoteOff(z80_bus);
}

void Track::FMUpdateTrack()
{
#ifdef SMPS_SoundTest
	SetSoundTest(false);
#endif

	if (--duration_timeout == 0)
	{
		SetHeld(false);

		if (!FMDoNext())
			return;

		if (IsResting())
			return;

		DoModulation();

		if (!FMPrepareNote())
			return;

		FMNoteOn();
	}
	else
	{
		if (IsResting())
			return;

		if (!NoteTimeoutUpdate())
			return;

		DoModulation();

		if (!FMUpdateFreq())
			return;
	}
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

bool Track::FMUpdateFreq()
{
#ifdef SMPS_EnableModulationEnvelopes
	if (modulation_ctrl == 0)
#else
	if (!IsModulationEnabled())
#endif
		return true;

	if (!FMPrepareNote())
		return false;

	return true;
}

bool Track::FMPrepareNote()
{
	if (IsOverridden())
		return true;

	const auto value = DoModulationEnvelope();

	if (!value.has_value())
		return false;

	FMSafeZ80Bus z80_bus;
	WriteFMIorII(z80_bus, 0xA4, *value >> 8);
	WriteFMIorII(z80_bus, 0xA0, *value & 0xFF);

	return true;
}

bool Track::FMDoNext()
{
	SetResting(false);

	const auto note_or_duration = GetNoteOrDuration();

	if (!note_or_duration.has_value())
		return false;

	unsigned char value = *note_or_duration;

	FMNoteOff();

	assert(value >= 0x80 || freq != 0);

	if (value < 0x80)
	{
		SetDuration(value);

		if (freq == 0)
		    SetResting(true);
	}
	else
	{
		FMSetFreq(value);

		value = *data_pointer++;

		if (value < 0x80)
			SetDuration(value);
		else
			--data_pointer;
	}

	FinishTrackUpdate();
	return true;
}

void Track::FMNoteOn()
{
	State &state = data->state;

	if (IsOverridden())
		return;

	{
		FMSafeZ80Bus z80_bus;

		if (voice_control == 6 && IsFM6Overridden())
		{
			z80_bus.WriteFMI(0x2B, 0);
			z80_bus.WriteFMII(0xB6, ams_fms_pan);
		}

		z80_bus.WriteFMI(0x28, 0xF0 | voice_control);
	}

	if (voice_control == 6 && IsFM6Overridden())
	{
		state.tracks[MUSIC_FM6].SetFM6Overridden(false);
		state.tracks[MUSIC_DAC].SetFM6Overridden(true);
	}
}

static constexpr std::array FMFrequencies = []() constexpr
{
	static constexpr auto MakeFMFrequency = [](const double frequency) constexpr
	{
		return our_lround(frequency * 0x400 * 0x400 * 2 / ClownMDSDK::FM::sample_rate);
	};
	
	// The first note is B, the last note is B-flat.
	constexpr auto fm_base_frequencies = std::to_array<double>({15.39, 16.35, 17.34, 18.36, 19.45, 20.64, 21.84, 23.13, 24.51, 25.98, 27.53, 29.15});
	constexpr unsigned int total_fm_octaves = 8;

	std::array<unsigned short, fm_base_frequencies.size() * total_fm_octaves> frequencies;

	for (std::size_t i = 0; i < frequencies.size(); ++i)
		frequencies[i] = MakeFMFrequency(fm_base_frequencies[i % fm_base_frequencies.size()]) + 0x800 * (i / fm_base_frequencies.size());

	return frequencies;
}();

void Track::FMSetFreq(const unsigned char note)
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

void Track::PSGUpdateTrack()
{
#ifdef SMPS_SoundTest
	SetSoundTest(false);
#endif

	if (--duration_timeout == 0)
	{
		SetHeld(false);

		if (!PSGDoNext())
			return;

		if (IsResting())
			return;

		DoModulation();

		if (!PSGDoNoteOn())
			return;

		if (!PSGDoVolFX())
			return;
	}
	else
	{
		if (IsResting())
			return;

		if (!NoteTimeoutUpdate())
			return;

		if (!PSGUpdateVolFX())
			return;

		DoModulation();

		if (!PSGUpdateFreq())
			return;
	}
}

bool Track::PSGUpdateFreq()
{
#ifdef SMPS_EnableModulationEnvelopes
	if (modulation_ctrl == 0)
#else
	if (!IsModulationEnabled())
#endif
		return true;

	return PSGDoNoteOn();
}

bool Track::PSGDoNoteOn()
{
	if (IsOverridden())
		return true;

	const auto value = DoModulationEnvelope();

	if (!value.has_value())
		return false;

	const unsigned int channel = voice_control == 0xE0 ? 0xC0 : voice_control;

	ClownMDSDK::PSG::Write((*value & 0xF) | channel);
	ClownMDSDK::PSG::Write((*value & 0x3F0) >> 4);

	return true;
}

bool Track::PSGDoNext()
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
		PSGSetFreq(value);

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
			return std::min<T>(0x3FF, our_lround(ClownMDSDK::PSG::sample_rate / (frequency * 2)));
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

void Track::PSGSetFreq(const unsigned char note)
{
	if (note < 0x81)
	{
		SetResting(true);
		freq = -1;
		PSGNoteOff();
		return;
	}

	const unsigned char index = note - 0x81 + transpose;

	assert(index < 12 * 7);

	freq = PSGFrequencies[index];
}

bool Track::PSGUpdateVolFX()
{
	if (voice_index == 0)
		return true;

	if (!PSGDoVolFX())
		return false;

	return true;
}

bool Track::PSGDoVolFX()
{
	unsigned int psg_volume = volume;

	if (voice_index != 0)
	{
		const auto envelope = data->psg.list[voice_index - 1];

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
					PSGNoteOff();
					return false;
			}

			psg_volume += value << 3;
			psg_volume = std::min(0xFU << 3, psg_volume);
			break;
		}
	}

	SetPSGVolume(psg_volume);
	return true;
}

void Track::SetPSGVolume(const unsigned int psg_volume)
{
	if (IsResting() || IsOverridden())
		return;

	if (IsHeld() && note_timeout_master != 0 && note_timeout == 0)
		return;

	ClownMDSDK::PSG::Write(0x10 | voice_control | std::min(0xFU, (psg_volume + data->state.variables.fadein_counter * 4) >> 3));
}

void Track::SendPSGNoteOff()
{
	ClownMDSDK::PSG::Write(voice_control | 0x1F);

	if (voice_control == 0xC0)
		ClownMDSDK::PSG::Write(0xFF);
}

void Track::PSGNoteOff()
{
	if (IsOverridden())
		return;

	SendPSGNoteOff();
}

static void InitMusicPlayback()
{
	State &state = data->state;

	// Save several values.
	const auto sndprio = state.variables.sndprio;
	const auto playing_1up = state.variables.playing_1up;
	const auto speedup = state.variables.speedup;
	const auto fadein_counter = state.variables.fadein_counter;
	const auto queue = state.variables.queue;

	// Clear variables.
	state.variables = {};

	// Restore the values saved above.
	state.variables.sndprio = sndprio;
	state.variables.playing_1up = playing_1up;
	state.variables.speedup = speedup;
	state.variables.fadein_counter = fadein_counter;
	state.variables.queue = queue;

	// Clear music track RAM.
	for (Track *track = &state.tracks[MUSIC_BEGIN]; track != &state.tracks[MUSIC_END]; ++track)
		*track = {};

	// Reset DAC volume.
	WriteDACVolume(0);

	// InitMusicPlayback, and Sound_PlayBGM for that matter,
	// don't do a very good job of setting up the music tracks.
	// Tracks that aren't defined in a music file's header don't have
	// their channels defined, meaning .sendfmnoteoff won't silence
	// hardware properly. In combination with removing the below
	// calls to FMSilenceAll/PSGSilenceAll, this will cause hanging
	// notes.
	// To fix this, we'll just forcefully set all channels, here:
	for (unsigned int i = 0; i < MUSIC_TRACK_COUNT; ++i)
	{
		static constexpr std::array<unsigned char, MUSIC_TRACK_COUNT> ChannelInitBytes = {
			6 | 0x10, 0, 1, 2, 4, 5, 6,
			0x80, 0xA0, 0xC0,
		#ifdef SMPS_EnablePWM
			0 | 8, 2 | 8, 4 | 8, 6 | 8
		#endif
		};

		Track &track = state.tracks[MUSIC_BEGIN + i];
		track.voice_control = ChannelInitBytes[i];
	}
}

static void Sound_PlayBGM(const unsigned int id)
{
	State &state = data->state;

	const Music &music = data->music.list[id];

	if (music.extra_life_jingle)
	{
		if (!state.variables.playing_1up)
		{
			state.variables.playing_1up = true;

			for (Track *track = &state.tracks[MUSIC_BEGIN]; track != &state.tracks[MUSIC_END]; ++track)
			{
				// Back-up the 'is playing' flag to the 'is overridden' flag.
				track->SetOverridden(track->IsPlaying());

				// Disable the track, so that nothing updates it whilst it is backed-up.
				track->SetPlaying(false);
			}

			state.variables.sndprio = 0;

			std::copy(&state.tracks[MUSIC_BEGIN], &state.tracks[MUSIC_END], &state.tracks[BACKUP_BEGIN]);
			state.variables_backup = state.variables;
		}
	}
	else
	{
		state.variables.fadein_counter = 0;
		state.variables.fadeout_counter = 0;
		state.variables.playing_1up = false;
	}

	InitMusicPlayback();

	const unsigned char* const header = music.data;
	state.variables.speeduptempo = music.speed_up_tempo;

	if (music.slower_tempo_on_pal)
		state.variables.force_pal_tempo = true;

	const auto voice_offset = ReadAlignedSignedWord(&header[0]);
	const Voice* const voices =
#ifdef SMPS_EnableUniversalVoiceBank
		voice_offset == 0
		? &data->universal_voice_bank
		:
#endif
		reinterpret_cast<const Voice*>(&header[voice_offset]);

	const unsigned int total_dac_and_fm_tracks = header[2];
	const unsigned int total_psg_tracks = header[3];
#ifdef SMPS_EnablePWM
	const unsigned int total_pwm_tracks = header[4];
#endif
	const unsigned char tempo_divider = header[6];
	const unsigned char tempo_mod = header[7];
	const unsigned char *track_header = &header[8];

	state.variables.tempo_mod = tempo_mod;
	state.variables.main_tempo = state.variables.speedup ? state.variables.speeduptempo : state.variables.tempo_mod;
	state.variables.main_tempo_timeout = 0;
	state.variables.pal_audio_countdown = 5;

	for (Track *track = &state.tracks[MUSIC_FM_DAC_BEGIN]; track != &state.tracks[MUSIC_FM_DAC_BEGIN + total_dac_and_fm_tracks]; ++track)
	{
		track->SetPlaying(true);
		track->tempo_divider = tempo_divider;
		track->stack_pointer = track->go_sub_stack.size();
		track->ams_fms_pan = 0xC0;
		track->duration_timeout = 1;
		track->data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track->transpose = track_header[2];
		track->volume = track_header[3];
		track->voice_ptr = voices;

		track_header += 4;
	}

	state.tracks[MUSIC_DAC].SetFM6Overridden(true);
	state.tracks[MUSIC_FM6].SetFM6Overridden(true);

	for (Track *track = &state.tracks[MUSIC_PSG_BEGIN]; track != &state.tracks[MUSIC_PSG_BEGIN + total_psg_tracks]; ++track)
	{
		track->SetPlaying(true);
		track->tempo_divider = tempo_divider;
		track->stack_pointer = track->go_sub_stack.size();
		track->duration_timeout = 1;
		track->data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track->transpose = track_header[2];
		track->volume = track_header[3];
#ifdef SMPS_EnableModulationEnvelopes
		track->modulation_ctrl = track_header[4];
#endif
		track->voice_index = track_header[5];

		track_header += 6;
	}

#ifdef SMPS_EnablePWM
	for (Track *track = &state.tracks[MUSIC_PWM_BEGIN]; track != &state.tracks[MUSIC_PWM_BEGIN + total_pwm_tracks]; ++track)
	{
		track->SetPlaying(true);
		track->tempo_divider = tempo_divider;
		track->stack_pointer = track->go_sub_stack.size();
		track->duration_timeout = 1;
		track->data_pointer = &header[ReadAlignedSignedWord(&track_header[0])];
		track->transpose = track_header[2];
		track->volume = track_header[3];

		track_header += 4;
	}
#endif

	for (Track *track = &state.tracks[SFX_BEGIN]; track != &state.tracks[SFX_END]; ++track)
		if (track->IsPlaying())
			state.tracks[SFX_BGMChannelRAM[track->IsPSG() ? track->voice_control >> 5 : track->voice_control - 2]].SetOverridden(true);

#ifdef SMPS_EnableSpecSFX
	if (state.tracks[SPECIAL_SFX_FM4].IsPlaying())
		state.tracks[MUSIC_FM4].SetOverridden(true);

	if (state.tracks[SPECIAL_SFX_PSG3].IsPlaying())
		state.tracks[MUSIC_PSG3].SetOverridden(true);
#endif

	{
		FMSafeZ80Bus z80_bus;

		for (Track *track = &state.tracks[MUSIC_FM_BEGIN]; track != &state.tracks[MUSIC_FM_END]; ++track)
			if (!track->IsOverridden())
				track->FMSilenceChannel(z80_bus);
	}

	for (Track *track = &state.tracks[MUSIC_PSG_BEGIN]; track != &state.tracks[MUSIC_PSG_END]; ++track)
		track->PSGNoteOff();

#ifdef SMPS_EnablePWM
	PWMSilenceAll();
#endif
}

static void Sound_PlaySFX(const unsigned int id)
{
	State &state = data->state;

	if (state.variables.playing_1up)
	{
		state.variables.sndprio = 0;
		return;
	}

#ifdef SMPS_EnableSpinDashSFX
	state.spindash_lastsound = false;
#endif

	const SFX &sfx = data->sfx.list[id];

	const unsigned char* const header = sfx.data;

	const Voice* const voices = reinterpret_cast<const Voice*>(&header[ReadAlignedSignedWord(&header[0])]);
	const unsigned char tempo_divider = header[2];
	const unsigned int total_tracks = header[3];
	const unsigned char *track_header = &header[4];

	switch (sfx.type)
	{
		case SFX::Type::Normal:
			break;

	#ifdef SMPS_PushSFXBehaviour
		case SFX::Type::BlockPush:
			if (state.push_playing)
				return;

			state.push_playing = true;
			break;
	#endif

	#ifdef SMPS_GloopSFXBehaviour
		case SFX::Type::Gloop:
			state.gloop_toggle = !state.gloop_toggle;

			if (!state.gloop_toggle)
				return;

			break;
	#endif

	#ifdef SMPS_EnableSpinDashSFX
		case SFX::Type::SpinDashRev:
		{
			const unsigned int pitch = state.spindash_timer == 0 ? 0 : state.spindash_pitch + 1;

			if (pitch < 12)
				state.spindash_pitch = pitch;

			state.spindash_lastsound = true;
			state.spindash_timer = 60;
			break;
		}
	#endif

	#ifdef SMPS_EnableContSFX
		case SFX::Type::Continuous:
			if (id == state.variables.current_contsfx)
			{
				state.continuous_sfx = true;
				state.variables.contsfx_channels = total_tracks;
				return;
			}
			else
			{
				state.continuous_sfx = false;
				state.variables.current_contsfx = id;
			}

			break;
	#endif
	}

	for (unsigned int i = 0; i < total_tracks; ++i)
	{
		const unsigned char voice_control = track_header[1];
		unsigned int track_lookup_index;

		if (voice_control < 0x80)
		{
			// FM
			track_lookup_index = voice_control - 2;
			state.tracks[SFX_BGMChannelRAM[track_lookup_index]].SetOverridden(true);
		}
		else
		{
			// PSG
			track_lookup_index = voice_control >> 5; // TODO: Cast to unsigned integer?
			state.tracks[SFX_BGMChannelRAM[track_lookup_index]].SetOverridden(true);

			// Silence PSG channel.
			ClownMDSDK::PSG::Write(voice_control | 0x1F);

			// Silence PSG noise channel if this is PSG3.
			if (voice_control == 0xC0)
				ClownMDSDK::PSG::Write(0xE0 | 0x1F);
		}

		auto &track = state.tracks[SFX_SFXChannelRAM[track_lookup_index]];

		track = {};

		track.playback_control = track_header[0];
		track.voice_control = voice_control;
		track.tempo_divider = tempo_divider;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[2])];
		track.transpose = track_header[4];
		track.volume = track_header[5];
		track.duration_timeout = 1;

#ifdef SMPS_EnableSpinDashSFX
		if (state.spindash_lastsound)
			track.transpose += state.spindash_pitch;
#endif

		track.stack_pointer = track.go_sub_stack.size();

		if (voice_control < 0x80)
		{
			track.ams_fms_pan = 0xC0;
			track.voice_ptr = voices;
		}

		track_header += 6;
	}

#ifdef SMPS_EnableSpecSFX
	if (state.tracks[SFX_FM4].IsPlaying())
		state.tracks[SPECIAL_SFX_FM4].SetOverridden(true);

	if (state.tracks[SFX_PSG3].IsPlaying())
		state.tracks[SPECIAL_SFX_PSG3].SetOverridden(true);
#endif
}

#ifdef SMPS_EnableSpecSFX
static void Sound_PlaySpecial(const unsigned int id)
{
	State &state = data->state;

	if (state.variables.playing_1up)
		return;

	const unsigned char* const header = data->background_sfx.list[id].data;

	const Voice* const voices = reinterpret_cast<const Voice*>(&header[ReadAlignedSignedWord(header)]);

	const unsigned char tempo_divider = header[2];
	const unsigned int total_tracks = header[3];
	const unsigned char *track_header = &header[4];

	for (unsigned int i = 0; i < total_tracks; ++i)
	{
		const unsigned char voice_control = track_header[1];

		Track &track = [&]() -> Track&
		{
			if (voice_control < 0x80)
			{
				// FM
				state.tracks[MUSIC_FM4].SetOverridden(true);
				return state.tracks[SPECIAL_SFX_FM4];
			}
			else
			{
				// PSG
				state.tracks[MUSIC_PSG3].SetOverridden(true);
				return state.tracks[SPECIAL_SFX_PSG3];
			}
		}();

		track = {};

		track.playback_control = track_header[0];
		track.voice_control = voice_control;
		track.tempo_divider = tempo_divider;
		track.data_pointer = &header[ReadAlignedSignedWord(&track_header[2])];
		track.transpose = track_header[4];
		track.volume = track_header[5];
		track.duration_timeout = 1;
		track.stack_pointer = track.go_sub_stack.size();

		if (voice_control < 0x80)
		{
			track.ams_fms_pan = 0xC0;
			track.voice_ptr = voices;
		}

		track_header += 6;
	}

	if (state.tracks[SFX_FM4].IsPlaying())
		state.tracks[SPECIAL_SFX_FM4].SetOverridden(true);

	if (state.tracks[SFX_PSG3].IsPlaying())
		state.tracks[SPECIAL_SFX_PSG3].SetOverridden(true);

	if (!state.tracks[SFX_PSG3].IsPlaying())
	{
		ClownMDSDK::PSG::Write(0xC0 | 0x1F);
		ClownMDSDK::PSG::Write(0xE0 | 0x1F);
	}
}
#endif

static void Sound_PlayDAC(const unsigned int id)
{
	PlayDACSFX(1, id);
}

static void PlaySoundID(const unsigned int id)
{
	if (id >= data->music.begin && id < data->music.end)
		Sound_PlayBGM(id - data->music.begin);
	else if (id >= data->sfx.begin && id < data->sfx.end)
		Sound_PlaySFX(id - data->sfx.begin);
#ifdef SMPS_EnableSpecSFX
	else if (id >= data->background_sfx.begin && id < data->background_sfx.end)
		Sound_PlaySpecial(id - data->background_sfx.begin);
#endif
	else if (id >= data->dac.begin && id < data->dac.end)
		Sound_PlayDAC(id - data->dac.begin);

	State &state = data->state;

	switch (id)
	{
		case 1: // StopAllSound
			StopAllSound();
			break;

		case 2: // StopSFX
			state.variables.sndprio = false;

			{
				FMSafeZ80Bus z80_bus;

				for (Track *track = &state.tracks[SFX_FM_BEGIN]; track != &state.tracks[SFX_FM_END]; ++track)
				{
					if (track->IsPlaying())
					{
						track->SetPlaying(false);
						track->SetHeld(false);

						track->FMNoteOff(z80_bus);

						Track &other_track =

						#ifdef SMPS_EnableSpecSFX
							track->voice_control == 4 && state.tracks[SPECIAL_SFX_FM4].IsPlaying()
							? state.tracks[SPECIAL_SFX_FM4]
							:
						#endif
							state.tracks[SFX_BGMChannelRAM[track->voice_control - 2]];

						if (other_track.IsOverridden())
						{
							other_track.SetOverridden(false);
							other_track.SetResting(true);
							other_track.SetVoice(z80_bus);
						}
					}
				}
			}

			for (Track *track = &state.tracks[SFX_PSG_BEGIN]; track != &state.tracks[SFX_PSG_END]; ++track)
			{
				if (track->IsPlaying())
				{
					track->SetPlaying(false);
					track->SetHeld(false);

					track->PSGNoteOff();

					Track &other_track =

					#ifdef SMPS_EnableSpecSFX
						(track->voice_control == 0xC0 || track->voice_control == 0xE0) && state.tracks[SPECIAL_SFX_PSG3].IsPlaying()
						? state.tracks[SPECIAL_SFX_PSG3]
						:
					#endif
						state.tracks[SFX_BGMChannelRAM[track->voice_control >> 5]];

					if (other_track.IsOverridden())
					{
						other_track.SetOverridden(false);
						other_track.SetResting(true);

						if (other_track.voice_control == 0xE0)
							ClownMDSDK::PSG::Write(other_track.psg_noise);
					}
				}
			}

			break;

	#ifdef SMPS_EnableSpecSFX
		case 3: // SpecStopSFX
		{
			Track &background_sfx_fm4 = state.tracks[SPECIAL_SFX_FM4];

			if (background_sfx_fm4.IsPlaying())
			{
				background_sfx_fm4.SetPlaying(false);

				if (!background_sfx_fm4.IsOverridden())
				{
					FMSafeZ80Bus z80_bus;

					background_sfx_fm4.SendFMNoteOff(z80_bus);

					Track &music_fm4 = state.tracks[MUSIC_FM4];
					music_fm4.SetOverridden(false);
					music_fm4.SetResting(true);

					if (music_fm4.IsPlaying())
						music_fm4.SetVoice(z80_bus);
				}
			}

			Track &background_sfx_psg3 = state.tracks[SPECIAL_SFX_PSG3];

			if (background_sfx_psg3.IsPlaying())
			{
				background_sfx_psg3.SetPlaying(false);

				if (!background_sfx_psg3.IsOverridden())
				{
					background_sfx_psg3.SendPSGNoteOff();

					Track &music_psg3 = state.tracks[MUSIC_PSG3];
					music_psg3.SetOverridden(false);
					music_psg3.SetResting(true);

					if (music_psg3.IsPlaying())
						if (music_psg3.voice_control == 0xE0)
							ClownMDSDK::PSG::Write(music_psg3.psg_noise);
				}
			}

			break;
		}
		#endif

		case 4: // StopDACSFX
		{
			ClownMDSDK::Z80::Bus z80_bus;
			z80_bus.ram[zRequestFlag] = z80_scf_instruction;
			// 'Stop PCM channel' command value
			z80_bus.ram[zRequestChannel2] = 2;
			break;
		}

		case 5: // FadeOutMusic
			state.variables.fadeout_delay = 3;
			state.variables.fadeout_counter = 0x28;
			state.variables.speedup = false;
			break;

		case 6: // SpeedUpMusic
			if (state.variables.playing_1up)
			{
				state.variables_backup.main_tempo = state.variables_backup.speeduptempo;
				state.variables_backup.speedup = true;
			}
			else
			{
				state.variables.main_tempo = state.variables.speeduptempo;
				state.variables.speedup = true;
			}
			
			break;

		case 7: // SlowDownMusic
			if (state.variables.playing_1up)
			{
				state.variables_backup.main_tempo = state.variables_backup.tempo_mod;
				state.variables_backup.speedup = false;
			}
			else
			{
				state.variables.main_tempo = state.variables.tempo_mod;
				state.variables.speedup = false;
			}
			
			break;
	}
}

static void CycleSoundQueue()
{
	State &state = data->state;

	for (auto &queue_slot : state.variables.queue)
	{
		const unsigned int id = queue_slot;
		queue_slot = 0;

		if (id == 0)
			continue;

		const std::optional<Priority> priority = [&]() -> std::optional<Priority>
		{
			if (id >= data->sfx.begin && id < data->sfx.end)
				return data->sfx.list[id - data->sfx.begin].priority;
		#ifdef SMPS_EnableSpecSFX
			else if (id >= data->background_sfx.begin && id < data->background_sfx.end)
				return data->background_sfx.list[id - data->background_sfx.begin].priority;
		#endif
			else
				return std::nullopt;
		}();

		if (priority.has_value() && !priority->do_not_save)
		{
			if (priority->value < state.variables.sndprio)
				continue;

			state.variables.sndprio = priority->value;
		}

		PlaySoundID(id);
	}
}

static void TempoWait()
{
	State &state = data->state;
	const unsigned int tempo_sum = state.variables.main_tempo + state.variables.main_tempo_timeout;
	state.variables.main_tempo_timeout = tempo_sum % 0x100;

	if (tempo_sum >= 0x100)
		for (Track *track = &state.tracks[MUSIC_BEGIN]; track != &state.tracks[MUSIC_END]; ++track)
			++track->duration_timeout;
}

static void UpdateMusic()
{
	State &state = data->state;

	if (state.tracks[MUSIC_DAC].IsPlaying())
		state.tracks[MUSIC_DAC].DACUpdateTrack();

	for (Track *track = &state.tracks[MUSIC_FM_BEGIN]; track != &state.tracks[MUSIC_FM_END]; ++track)
		if (track->IsPlaying())
			track->FMUpdateTrack();

	for (Track *track = &state.tracks[MUSIC_PSG_BEGIN]; track != &state.tracks[MUSIC_PSG_END]; ++track)
		if (track->IsPlaying())
			track->PSGUpdateTrack();

	TempoWait();

	if (state.variables.fade_to_previous_pending)
	{
		std::copy(&state.tracks[BACKUP_BEGIN], &state.tracks[BACKUP_END], &state.tracks[MUSIC_BEGIN]);
		state.variables = state.variables_backup;

		for (Track *track = &state.tracks[MUSIC_BEGIN]; track != &state.tracks[MUSIC_END]; ++track)
		{
			// Back-up the 'is playing' flag to the 'is overridden' flag.
			track->SetPlaying(track->IsOverridden());

			// Disable the track, so that nothing updates it whilst it is backed-up.
			track->SetOverridden(false);
		}

		state.variables.fadein_counter = 0x28;

		Track* const dac_track = &state.tracks[MUSIC_DAC];

		{
			FMSafeZ80Bus z80_bus;

			if (dac_track->IsPlaying())
				dac_track->SetDACVolume(z80_bus);

			for (Track *fm_track = &state.tracks[MUSIC_FM_BEGIN]; fm_track != &state.tracks[MUSIC_FM_END]; ++fm_track)
			{
				if (fm_track->IsPlaying())
				{
					fm_track->SetResting(true);
					fm_track->SetVoice(z80_bus);
				}
			}
		}

		for (Track *psg_track = &state.tracks[MUSIC_PSG_BEGIN]; psg_track != &state.tracks[MUSIC_PSG_END]; ++psg_track)
		{
			if (psg_track->IsPlaying())
			{
				psg_track->SetResting(true);
				psg_track->PSGNoteOff();

				if (psg_track->voice_control == 0xE0)
					ClownMDSDK::PSG::Write(psg_track->psg_noise);
			}
		}

		state.tracks[MUSIC_DAC].SetFM6Overridden(true);
		state.tracks[MUSIC_FM6].SetFM6Overridden(true);

		state.variables.playing_1up = false;
	}
}

static void DoFadeIn()
{
	State &state = data->state;

	if (state.variables.fadein_delay != 0)
	{
		--state.variables.fadein_delay;
		return;
	}

	--state.variables.fadein_counter;
	state.variables.fadein_delay = 2;

	Track* const dac_track = &state.tracks[MUSIC_DAC];

	FMSafeZ80Bus z80_bus;

	if (dac_track->IsPlaying())
		dac_track->SetDACVolume(z80_bus);

	const auto FMLoop = [&](const unsigned int first_track_index, const unsigned int last_track_index)
	{
		for (Track *track = &state.tracks[first_track_index]; track != &state.tracks[last_track_index]; ++track)
			if (track->IsPlaying())
				track->SendVoiceTL(z80_bus);
	};

	const auto PSGLoop = [&](const unsigned int first_track_index, const unsigned int last_track_index)
	{
		for (Track *track = &state.tracks[first_track_index]; track != &state.tracks[last_track_index]; ++track)
		{
			if (track->IsPlaying())
			{
				// If a volume envelope is active, then it will handle updating the volume for us.
				// Doing it manually will just conflict with it.
				if (track->voice_index == 0)
					track->SetPSGVolume(track->volume);
			}
		}
	};

	FMLoop(MUSIC_FM_BEGIN, MUSIC_FM_END);
	PSGLoop(MUSIC_PSG_BEGIN, MUSIC_PSG_END);
	FMLoop(SFX_FM_BEGIN, SFX_FM_END);
	PSGLoop(SFX_PSG_BEGIN, SFX_PSG_END);
#ifdef SMPS_EnableSpecSFX
	FMLoop(SPECIAL_SFX_FM_BEGIN, SPECIAL_SFX_FM_END);
	PSGLoop(SPECIAL_SFX_PSG_BEGIN, SPECIAL_SFX_PSG_END);
#endif
}

static void DoFadeOut()
{
	State &state = data->state;

	if (state.variables.fadeout_delay != 0)
	{
		--state.variables.fadeout_delay;
		return;
	}

	if (--state.variables.fadeout_counter == 0)
	{
		StopAllSound();
		return;
	}

	state.variables.fadeout_delay = 3;

	Track* const dac_track = &state.tracks[MUSIC_DAC];

	{
		FMSafeZ80Bus z80_bus;

		if (dac_track->IsPlaying())
		{
			dac_track->volume += 4;

			if (dac_track->volume >= 0x80)
				dac_track->SetPlaying(false);
			else
				dac_track->SetDACVolume(z80_bus);
		}

		for (Track *fm_track = &state.tracks[MUSIC_FM_BEGIN]; fm_track != &state.tracks[MUSIC_FM_END]; ++fm_track)
		{
			if (fm_track->IsPlaying())
			{
				++fm_track->volume;

				if (fm_track->volume >= 0x80)
					fm_track->SetPlaying(false);
				else
					fm_track->SendVoiceTL(z80_bus);
			}
		}
	}

	for (Track *psg_track = &state.tracks[MUSIC_PSG_BEGIN]; psg_track != &state.tracks[MUSIC_PSG_END]; ++psg_track)
	{
		if (psg_track->IsPlaying())
		{
			psg_track->volume += 4;

			if (psg_track->volume >= 0x80)
			{
				psg_track->SetPlaying(false);
			}
			else
			{
				// If a volume envelope is active, then it will handle updating the volume for us.
				// Doing it manually will just conflict with it.
				if (psg_track->voice_index == 0)
					psg_track->SetPSGVolume(psg_track->volume);
			}
		}
	}
}

static void HandlePause()
{
	State &state = data->state;

	if (state.pause < 0x80)
	{
		if (state.pause == 2)
			return;

		state.pause = 2;

		FMSafeZ80Bus z80_bus;
		SilenceAll(z80_bus);
	}
	else
	{
		const auto RestoreFMTrackVoices = [&](FMSafeZ80Bus &z80_bus, const unsigned int begin, const unsigned int end)
		{
			for (Track *track = &state.tracks[begin]; track != &state.tracks[end]; ++track)
				if (track->IsPlaying() && !track->IsOverridden())
					track->SetVoice(z80_bus);
		};

		state.pause = 0;

		FMSafeZ80Bus z80_bus;

		RestoreFMTrackVoices(z80_bus, MUSIC_FM_BEGIN, MUSIC_FM_END);
		RestoreFMTrackVoices(z80_bus, SFX_FM_BEGIN, SFX_FM_END);
#ifdef SMPS_EnableSpecSFX
		RestoreFMTrackVoices(z80_bus, SPECIAL_SFX_FM_BEGIN, SPECIAL_SFX_FM_END);
#endif

		// Apply DAC panning if necessary (RestoreFMTrackVoices already reapplied FM6's panning).
		auto &dac_track = state.tracks[MUSIC_DAC];
		if (dac_track.IsPlaying() && !dac_track.IsOverridden() && !dac_track.IsFM6Overridden())
			z80_bus.WriteFMII(0xB6, dac_track.ams_fms_pan);

#ifdef __MEGA_DRIVE__
		z80_bus.ram[zRequestFlag] = z80_scf_instruction;
		// 'Play sample' command value
		z80_bus.ram[zRequestChannel1] = 1;
		z80_bus.ram[zRequestChannel2] = 1;
#endif
	}
}

STARTING_FUNCTION void SMPS::UpdateDriver()
{
	State &state = data->state;

	if (state.pause != 0)
	{
		HandlePause();
		return;
	}

	if (state.variables.fadeout_counter != 0)
		DoFadeOut();

	if (state.variables.fadein_counter != 0)
		DoFadeIn();

	if (state.variables.queue.IsAnythingQueued())
		CycleSoundQueue();

#ifdef SMPS_EnableSpinDashSFX
	if (state.spindash_timer != 0)
		--state.spindash_timer;
#endif

	// Update the music twice on some frames so that music is the same speed on NTSC and PAL consoles.
	if (state.pal && !state.variables.force_pal_tempo && --state.variables.pal_audio_countdown == 0)
	{
		state.variables.pal_audio_countdown = 5;
		UpdateMusic();
	}

	UpdateMusic();

	for (Track *track = &state.tracks[SFX_FM_BEGIN]; track != &state.tracks[SFX_FM_END]; ++track)
		if (track->IsPlaying())
			track->FMUpdateTrack();

	for (Track *track = &state.tracks[SFX_PSG_BEGIN]; track != &state.tracks[SFX_PSG_END]; ++track)
		if (track->IsPlaying())
			track->PSGUpdateTrack();

#ifdef SMPS_EnableSpecSFX
	if (state.tracks[SPECIAL_SFX_FM4].IsPlaying())
		state.tracks[SPECIAL_SFX_FM4].FMUpdateTrack();

	if (state.tracks[SPECIAL_SFX_PSG3].IsPlaying())
		state.tracks[SPECIAL_SFX_PSG3].PSGUpdateTrack();
#endif
}

// TODO: Proper sign extension considering alternate integer sizes!
// TODO: The packing of pointers in the track struct!
// TODO: Move the music voice pointer back into the state struct so only SFX have it in their track RAM!
// TODO: Check sign-ness of track values.
// TODO: Check if variable accumulations need bitmasks.
// TODO: Allow SFX to use universal voice bank too.
// TODO: Clean-up 'IsPSG' calls.
// TODO: Make IsPlaying and all that into proper 1-bit booleans.
// TODO: Constants for DAC driver commands.
