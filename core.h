#ifndef CLONE_DRIVER_CORE_H
#define CLONE_DRIVER_CORE_H

#include <array>
#include <optional>

#include <clownmdsdk.h>

//#define SMPS_EnablePWM
#define SMPS_SoundTest
#define SMPS_EnableModulationEnvelopes
#define SMPS_EnableSpecSFX
#define SMPS_GloopSFXBehaviour
#define SMPS_EnableSpinDashSFX
#define SMPS_PushSFXBehaviour
#define SMPS_EnableContSFX
#define SMPS_EnableUniversalVoiceBank

namespace SMPS
{
	class FMSafeZ80Bus : public ClownMDSDK::MainCPU::Z80::Bus
	{
	public:
#ifdef __MEGA_DRIVE__
		~FMSafeZ80Bus()
		{
			WaitUntilFMReady();

			asm(
				"move.b	#0x2A,%0"
				: "=Qm" (ClownMDSDK::MainCPU::FM::Unsafe::a0)
			);
		}
#endif
	};

	using Voice = std::array<unsigned char, 25>;

	struct ModulationSettings
	{
		unsigned char wait;
		unsigned char speed;
		signed char delta;
		unsigned char steps;
	};

	struct Track
	{
		unsigned char playback_control;
		unsigned char voice_control;
		union
		{
			unsigned char vol_env_index;
			unsigned char ams_fms_pan;
		};
		unsigned char tempo_divider; // TODO: Move this to global variables for music tracks.
		signed char transpose;
		unsigned char volume;
		unsigned char voice_index;
		const unsigned char *data_pointer;
		union
		{
			unsigned char saved_dac;
			unsigned short freq;
		};
		unsigned char stack_pointer;
		unsigned char duration_timeout;
		unsigned char saved_duration;
		unsigned char note_timeout;
		unsigned char note_timeout_master;
		const ModulationSettings *modulation_ptr;
		ModulationSettings modulation;
		unsigned short modulation_val;
	#ifdef SMPS_EnableModulationEnvelopes
		unsigned char modulation_ctrl;
		unsigned char mod_env_index;
		unsigned char mod_env_sens;
	#endif
		signed char detune;
		unsigned char psg_noise;
		const Voice *voice_ptr;

		union
		{
			std::array<unsigned char, 3 * 4> loop_counters;
			std::array<const unsigned char*, 3> go_sub_stack;
		};

		// Generic
		void WriteFMIorII(FMSafeZ80Bus &z80_bus, unsigned char port, unsigned char value);
		void cfJumpTo();
		void FinishTrackUpdate();
		void SetDuration(unsigned int duration);
		[[nodiscard]] bool CoordFlag(unsigned int value);
		[[nodiscard]] bool NoteTimeoutUpdate();
		[[nodiscard]] std::optional<unsigned int> DoModulationEnvelope();
		[[nodiscard]] bool IsMusic() const;
		void ReloadModulationData();
		[[nodiscard]] std::optional<unsigned char> GetNoteOrDuration();

		// DAC
		void DACUpdateSample();
		[[nodiscard]] bool DACDoNext();
		void DACUpdateTrack();
		void SetDACVolume(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);

		// FM
		void SendVoiceTLCommon(FMSafeZ80Bus &z80_bus, const Voice &voice, bool force_upload);
		void SendVoiceTL(FMSafeZ80Bus &z80_bus);
		void SetVoice(FMSafeZ80Bus &z80_bus);
		void FMSilenceChannel(FMSafeZ80Bus &z80_bus);
		void SendFMNoteOff(FMSafeZ80Bus &z80_bus);
		void FMNoteOff(FMSafeZ80Bus &z80_bus);
		void FMNoteOff();
		void FMUpdateTrack();
		void DoModulation();
		[[nodiscard]] bool FMUpdateFreq();
		[[nodiscard]] bool FMPrepareNote();
		[[nodiscard]] bool FMDoNext();
		void FMNoteOn();
		void FMSetFreq(unsigned char note);

		// PSG
		void PSGUpdateTrack();
		[[nodiscard]] bool PSGUpdateFreq();
		[[nodiscard]] bool PSGDoNoteOn();
		[[nodiscard]] bool PSGDoNext();
		void PSGSetFreq(unsigned char note);
		[[nodiscard]] bool PSGUpdateVolFX();
		[[nodiscard]] bool PSGDoVolFX();
		void SetPSGVolume(unsigned int psg_volume);
		void SendPSGNoteOff();
		void PSGNoteOff();

		// PSG Noise
		void PSGNoiseUpdateTrack();
		[[nodiscard]] bool PSGNoiseDoNext();
		void PSGNoiseSetDrumNote(unsigned char note);

	#define BIT_METHOD_IS(VARIABLE, BIT, NAME) [[nodiscard]] bool Is##NAME() const {return ((VARIABLE) & 1 << (BIT)) != 0;}
	#define BIT_METHODS(VARIABLE, BIT, NAME) \
		BIT_METHOD_IS(VARIABLE, BIT, NAME) \
	\
		void Set##NAME(const bool enable) \
		{ \
			if (enable) \
				(VARIABLE) |= 1 << (BIT); \
			else \
				(VARIABLE) &= ~(1 << (BIT)); \
		}

		BIT_METHOD_IS(voice_control, 7, PSG)
		BIT_METHOD_IS(voice_control, 4, DAC)
	#ifdef SMPS_EnablePWM
		BIT_METHOD_IS(voice_control, 3, PWM)
	#endif

		BIT_METHODS(playback_control, 7, Playing)
		BIT_METHODS(playback_control, 6, FM6Overridden)
		BIT_METHODS(playback_control, 4, Held)
		BIT_METHODS(playback_control, 2, Overridden)
		BIT_METHODS(playback_control, 1, Resting)
	#ifdef SMPS_SoundTest
		BIT_METHODS(playback_control, 0, SoundTest)
	#endif

	#ifdef SMPS_EnableModulationEnvelopes
		BIT_METHODS(modulation_ctrl, 7, ModulationEnabled)
		BIT_METHODS(modulation_ctrl, 6, ModulationInZ80Mode)
	#else
		BIT_METHODS(playback_control, 3, ModulationEnabled)
		BIT_METHODS(playback_control, 5, ModulationInZ80Mode)
	#endif
	};

	//};

	enum
	{
		MUSIC_BEGIN,

		MUSIC_FM_DAC_BEGIN = MUSIC_BEGIN,
		MUSIC_DAC = MUSIC_FM_DAC_BEGIN,

		MUSIC_FM_BEGIN,
		MUSIC_FM1 = MUSIC_FM_BEGIN,
		MUSIC_FM2,
		MUSIC_FM3,
		MUSIC_FM4,
		MUSIC_FM5,
		MUSIC_FM6,
		MUSIC_FM_END,
		MUSIC_FM_TRACK_COUNT = MUSIC_FM_END - MUSIC_FM_BEGIN,

		MUSIC_FM_DAC_END = MUSIC_FM_END,
		MUSIC_FM_DAC_TRACK_COUNT = MUSIC_FM_DAC_END - MUSIC_FM_DAC_BEGIN,

		MUSIC_PSG_BEGIN = MUSIC_FM_DAC_END,
		MUSIC_PSG1 = MUSIC_PSG_BEGIN,
		MUSIC_PSG2,
		MUSIC_PSG3,
		MUSIC_PSG_END,
		MUSIC_PSG_TRACK_COUNT = MUSIC_PSG_END - MUSIC_PSG_BEGIN,

		MUSIC_PSG_NOISE = MUSIC_PSG_END,
		MUSIC_END,
		MUSIC_TRACK_COUNT = MUSIC_END - MUSIC_BEGIN,

		BACKUP_BEGIN = MUSIC_END,
		BACKUP_DAC = BACKUP_BEGIN,
		BACKUP_FM1,
		BACKUP_FM2,
		BACKUP_FM3,
		BACKUP_FM4,
		BACKUP_FM5,
		BACKUP_FM6,
		BACKUP_PSG1,
		BACKUP_PSG2,
		BACKUP_PSG3,
		BACKUP_PSG_NOISE,
		BACKUP_END,
		BACKUP_TRACK_COUNT = BACKUP_END - BACKUP_BEGIN,

		TOTAL_TRACKS = BACKUP_END,
	};

	enum
	{
		NON_BACKUP_TRACKS_BEGIN = BACKUP_BEGIN,

		SFX_BEGIN = NON_BACKUP_TRACKS_BEGIN,

		SFX_FM_BEGIN = SFX_BEGIN,
		SFX_FM3 = SFX_FM_BEGIN,
		SFX_FM4,
		SFX_FM5,
		SFX_FM_END,
		SFX_FM_TRACK_COUNT = SFX_FM_END - SFX_FM_BEGIN,

		SFX_PSG_BEGIN = SFX_FM_END,
		SFX_PSG1 = SFX_PSG_BEGIN,
		SFX_PSG2,
		SFX_PSG3,
		SFX_PSG_END,
		SFX_PSG_TRACK_COUNT = SFX_PSG_END - SFX_PSG_BEGIN,

		SFX_END = SFX_PSG_END,
		SFX_TRACK_COUNT = SFX_END - SFX_BEGIN,

	#ifdef SMPS_EnableSpecSFX
		SPECIAL_SFX_BEGIN = SFX_END,

		SPECIAL_SFX_FM_BEGIN = SPECIAL_SFX_BEGIN,
		SPECIAL_SFX_FM4 = SPECIAL_SFX_FM_BEGIN,
		SPECIAL_SFX_FM_END,
		SPECIAL_SFX_FM_TRACK_COUNT = SPECIAL_SFX_FM_END - SPECIAL_SFX_FM_BEGIN,

		SPECIAL_SFX_PSG_BEGIN = SPECIAL_SFX_FM_END,
		SPECIAL_SFX_PSG3 = SPECIAL_SFX_PSG_BEGIN,
		SPECIAL_SFX_PSG_END,
		SPECIAL_SFX_PSG_TRACK_COUNT = SPECIAL_SFX_PSG_END - SPECIAL_SFX_PSG_BEGIN,

		SPECIAL_SFX_END = SPECIAL_SFX_PSG_END,
		SPECIAL_SFX_TRACK_COUNT = SPECIAL_SFX_END - SPECIAL_SFX_BEGIN,
	#endif

		NON_BACKUP_TRACKS_END = SPECIAL_SFX_END,
		NON_BACKUP_TRACK_COUNT = NON_BACKUP_TRACKS_END - NON_BACKUP_TRACKS_BEGIN
	};

	struct Queue : public std::array<unsigned short, 4>
	{
		bool IsAnythingQueued() const
		{
			for (const auto &slot : *this)
				if (slot != 0)
					return true;

			return false;
		}
	};

	struct Variables
	{
		Queue queue;
		unsigned char sndprio;
		unsigned char main_tempo_timeout;
		unsigned char main_tempo;
		bool playing_1up : 1;
		bool fade_to_previous_pending : 1;
		bool speedup : 1;
		bool force_pal_tempo : 1;

		unsigned char fadeout_counter;
		unsigned char fadeout_delay;

		unsigned char fadein_counter;
		unsigned char fadein_delay;

		unsigned char tempo_mod;
		unsigned char speeduptempo;
		unsigned char pal_audio_countdown;
		unsigned char communication_byte;
	#ifdef SMPS_EnableContSFX
		unsigned short current_contsfx;
		unsigned char contsfx_channels;
	#endif
		unsigned char psg_drum_volume;
	};

	struct State
	{
		std::array<Track, TOTAL_TRACKS> tracks;

		Variables variables;
		Variables variables_backup;

		unsigned char pause;
	#ifdef SMPS_GloopSFXBehaviour
		bool gloop_toggle : 1;
	#endif
	#ifdef SMPS_EnableSpinDashSFX
		bool spindash_lastsound : 1;
	#endif
	#ifdef SMPS_PushSFXBehaviour
		bool push_playing : 1;
	#endif
	#ifdef SMPS_EnableContSFX
		bool continuous_sfx : 1;
	#endif
		bool pal : 1;

	#ifdef SMPS_EnableSpinDashSFX
		unsigned char spindash_timer;
		unsigned char spindash_pitch;
	#endif
	};

	struct Priority
	{
		bool do_not_save : 1;
		unsigned char value : 7;
	};

	struct Music
	{
		const unsigned char *data;
		unsigned char speed_up_tempo;
		bool slower_tempo_on_pal : 1;
		bool extra_life_jingle : 1;
	};

	struct SFX
	{
		enum class Type : unsigned char
		{
			Normal,
		#ifdef SMPS_PushSFXBehaviour
			BlockPush,
		#endif
		#ifdef SMPS_GloopSFXBehaviour
			Gloop,
		#endif
		#ifdef SMPS_EnableSpinDashSFX
			SpinDashRev,
		#endif
		#ifdef SMPS_EnableContSFX
			Continuous,
		#endif
		};

		const unsigned char *data;
		Priority priority;
		Type type;
	};

	struct BackgroundSFX
	{
		const unsigned char *data;
		Priority priority;
	};

	struct DACSample
	{
		struct
		{
			unsigned char upper;
			unsigned char lower;
		} bank_offset;
		struct
		{
			unsigned char upper;
			unsigned char lower;
		} playback_increment;
		unsigned char bank_index;
	};

	struct Data
	{
		struct
		{
			const Music *list;
			unsigned short begin, end;
		} music;
		struct
		{
			const SFX *list;
			unsigned short begin, end;
		} sfx;
#ifdef SMPS_EnableSpecSFX
		struct
		{
			const BackgroundSFX *list;
			unsigned short begin, end;
		} background_sfx;
#endif
		struct
		{
			const DACSample *list;
			unsigned short begin, end;
		} dac;
		struct
		{
			const unsigned char* const *list;
		} psg;
#ifdef SMPS_EnableUniversalVoiceBank
		const Voice *universal_voice_bank;
#endif
		State &state;
	};

	#ifndef BINARY_BLOB
	extern const Data* const data;
	#endif

	void UpdateDriver();
	unsigned int GetTrackNote(unsigned int track_id, bool modulation);
	void Initialise();
}

#endif // CLONE_DRIVER_CORE_H
