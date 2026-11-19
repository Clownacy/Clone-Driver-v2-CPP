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
//#define SMPS_EnablePSGNoiseDrums

namespace SMPS
{
	template<typename... Ts>
	struct VisitHelper : Ts... { using Ts::operator()...; };

	// Deduction guide.
	template<typename... Ts>
	VisitHelper(Ts...) -> VisitHelper<Ts...>;

	inline auto LockZ80BusFMSafe(const auto &callback)
	{
		return ClownMDSDK::MainCPU::Z80::Bus::LockInterruptSafe(
			[&](ClownMDSDK::MainCPU::Z80::Bus &bus)
			{
				class FMRestorer
				{
				private:
					ClownMDSDK::MainCPU::Z80::Bus &bus;

				public:
					FMRestorer(ClownMDSDK::MainCPU::Z80::Bus &bus)
						: bus(bus)
					{}

			#ifdef __MEGA_DRIVE__
					~FMRestorer()
					{
						bus.WaitUntilFMReady();

						asm(
							"move.b	#0x2A,%0"
							: "=Qm" (ClownMDSDK::MainCPU::FM::Unsafe::a0)
						);
					}
			#endif
				};

				FMRestorer fm_restorer(bus);

				return callback(bus);
			}
		);
	}

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
		unsigned char tempo_divider; // TODO: Move this to global variables for music tracks.
		signed char transpose;
		unsigned char volume_attenuation;
		unsigned char voice_index;
		const unsigned char *data_pointer;

		unsigned short freq;
		signed char detune;

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

		union
		{
			std::array<const unsigned char*, 3> go_sub_stack;
			std::array<unsigned char, sizeof(go_sub_stack)> loop_counters;
		};

		// Generic
		void cfJumpTo();
		void FinishTrackUpdate();
		void SetDuration(unsigned int duration);
		[[nodiscard]] bool CoordFlag(unsigned int value);
		[[nodiscard]] bool NoteTimeoutUpdate();
		[[nodiscard]] std::optional<unsigned int> DoModulationEnvelope();
		[[nodiscard]] bool IsMusic() const;
		void ReloadModulationData();
		void DoModulation();
		[[nodiscard]] std::optional<unsigned char> GetNoteOrDuration();

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

		template<typename... Ts>
		auto Visit(Ts &&...visitors);
	};

	struct YM2612Track : Track
	{
		unsigned char ams_fms_pan;

		void WriteFMIorII(ClownMDSDK::MainCPU::Z80::Bus &z80_bus, unsigned char port, unsigned char value);
	};

	struct DACTrack : YM2612Track
	{
		void DACUpdateSample();
		[[nodiscard]] bool DACDoNext();
		void DACUpdateTrack();
		void ApplyDACVolumeAttenuation(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
	};

	struct FMTrack : YM2612Track
	{
		const Voice *voice_ptr;

		void SendVoiceTLCommon(ClownMDSDK::MainCPU::Z80::Bus &z80_bus, const Voice &voice, bool force_upload);
		void SendVoiceTL(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
		void SetVoice(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
		void FMSilenceChannel(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
		void SendFMNoteOff(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
		void FMNoteOff(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
		void FMNoteOff();
		void FMUpdateTrack();
		[[nodiscard]] bool FMUpdateFreq();
		[[nodiscard]] bool FMPrepareNote();
		[[nodiscard]] bool FMDoNext();
		void FMNoteOn();
		void FMSetFreq(unsigned char note);
	};

	struct PSGTrack : Track
	{
		unsigned char vol_env_index;
		unsigned char psg_noise;

		// PSG
		void PSGUpdateTrack();
		[[nodiscard]] bool PSGUpdateFreq();
		[[nodiscard]] bool PSGDoNoteOn();
		[[nodiscard]] bool PSGDoNext();
		void PSGSetFreq(unsigned char note);
		[[nodiscard]] bool PSGUpdateVolFX();
		[[nodiscard]] bool PSGDoVolFX();
		void SetPSGVolumeAttenuation(unsigned int psg_volume);
		void SendPSGNoteOff();
		void PSGNoteOff();

#ifdef SMPS_EnablePSGNoiseDrums
		// PSG Noise
		void PSGNoiseUpdateTrack();
		[[nodiscard]] bool PSGNoiseDoNext();
		void PSGNoiseSetDrumNote(unsigned char note);
#endif
	};

#ifdef SMPS_EnablePWM
	struct PWMTrack : Track
	{};
#endif

	template<typename... Ts>
	inline auto Track::Visit(Ts &&...visitors)
	{
		auto visitor = VisitHelper{std::forward<Ts>(visitors)...};

		if (IsDAC())
			return visitor(static_cast<DACTrack&>(*this));
		else if (IsPSG())
			return visitor(static_cast<PSGTrack&>(*this));
#ifdef SMPS_EnablePWM
		else if (IsPWM())
			return visitor(static_cast<PWMTrack&>(*this));
#endif
		else
			return visitor(static_cast<FMTrack&>(*this));
	}

	//};

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
		bool fade_to_previous_pending : 1;
		bool speedup : 1;
		bool force_pal_tempo : 1;
		bool ignore_speedup : 1;

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
	#ifdef SMPS_EnablePSGNoiseDrums
		unsigned char psg_drum_volume;
	#endif
	};

	struct State
	{
		struct MusicTracks
		{
			DACTrack dac;
			std::array<FMTrack, 6> fm;
			std::array<PSGTrack, 3> psg;
		#ifdef SMPS_EnablePSGNoiseDrums
			PSGTrack psg_noise;
		#endif

			FMTrack& FM1()  { return fm[0]; }
			FMTrack& FM2()  { return fm[1]; }
			FMTrack& FM3()  { return fm[2]; }
			FMTrack& FM4()  { return fm[3]; }
			FMTrack& FM5()  { return fm[4]; }
			FMTrack& FM6()  { return fm[5]; }
			PSGTrack& PSG1() { return psg[0]; }
			PSGTrack& PSG2() { return psg[1]; }
			PSGTrack& PSG3() { return psg[2]; }

			void Iterate(const auto &callback)
			{
				callback(dac);
				for (auto &track : fm)
					callback(track);
				for (auto &track : psg)
					callback(track);
			#ifdef SMPS_EnablePSGNoiseDrums
				callback(psg_noise);
			#endif
			}

			// TODO: Add a 'clear' method?
		} music_tracks;

		class Shared
		{
		public:
			struct MusicBackup
			{
				MusicTracks tracks;
				Variables variables;
			};

			struct NonMusicTracks
			{
				struct
				{
					std::array<FMTrack, 3> fm;
					std::array<PSGTrack, 3> psg;

					FMTrack& FM3()  { return fm[0]; }
					FMTrack& FM4()  { return fm[1]; }
					FMTrack& FM5()  { return fm[2]; }
					PSGTrack& PSG1() { return psg[0]; }
					PSGTrack& PSG2() { return psg[1]; }
					PSGTrack& PSG3() { return psg[2]; }

					void Iterate(const auto &callback)
					{
						for (auto &track : fm)
							callback(track);
						for (auto &track : psg)
							callback(track);
					}
				} sfx;

			#ifdef SMPS_EnableSpecSFX
				struct
				{
					FMTrack fm4;
					PSGTrack psg3;

					void Iterate(const auto &callback)
					{
						callback(fm4);
						callback(psg3);
					}
				} special_sfx;
			#endif

				void Iterate(const auto &callback)
				{
					sfx.Iterate(callback);
				#ifdef SMPS_EnableSpecSFX
					special_sfx.Iterate(callback);
				#endif
				}
			};

		private:
			union
			{
				MusicBackup music_backup;
				NonMusicTracks non_music_tracks;
			};
			bool music_backup_active : 1;

		public:
			bool IsMusicBackupActive() const
			{
				return music_backup_active;
			}

			void CreateMusicBackup(const MusicTracks &tracks, const Variables &variables)
			{
				music_backup_active = true;

				music_backup.tracks = tracks;
				music_backup.variables = variables;
			}

			void DestroyMusicBackup()
			{
				// Mark the SFX as no longer playing, since their RAM has been trashed by the music backup.
				non_music_tracks.Iterate(
					[](Track &track)
					{
						track.SetPlaying(false);
					}
				);

				music_backup_active = false;
			}

			template<typename... Ts>
			decltype(auto) Visit(Ts &&...visitors)
			{
				auto visitor = VisitHelper{std::forward<Ts>(visitors)...};

				if (music_backup_active)
					return visitor(music_backup);
				else
					return visitor(non_music_tracks);
			}

			void VisitNonMusicTracks(const auto &callback)
			{
				return Visit(
					[&callback](NonMusicTracks &non_music_tracks)
					{
						callback(non_music_tracks);
					},
					[](auto&) {}
				);
			}

			NonMusicTracks& GetNonMusicTracks()
			{
				return Visit(
					[](NonMusicTracks &non_music_tracks) -> NonMusicTracks&
					{
						return non_music_tracks;
					},
					[](auto&) -> NonMusicTracks&
					{
						std::unreachable();
					}
				);
			}

			MusicBackup& GetMusicBackup()
			{
				return Visit(
					[](MusicBackup &music_backup) -> MusicBackup&
					{
						return music_backup;
					},
					[](auto&) -> MusicBackup&
					{
						std::unreachable();
					}
				);
			}
		} shared;

		void IterateTracks(const auto &callback)
		{
			music_tracks.Iterate(callback);

			shared.VisitNonMusicTracks(
				[&callback](auto &non_music_tracks)
				{
					non_music_tracks.sfx.Iterate(callback);
				#ifdef SMPS_EnableSpecSFX
					non_music_tracks.special_sfx.Iterate(callback);
				#endif
				}
			);
		}

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

		unsigned char pause;

	#ifdef SMPS_EnableSpinDashSFX
		unsigned char spindash_timer;
		unsigned char spindash_pitch;
	#endif

		unsigned char music_tempo_modifier, music_tempo_modifier_master;

		Variables variables;
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
		bool ignore_speedup : 1;
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

#ifdef SMPS_EnableSpecSFX
	struct BackgroundSFX
	{
		const unsigned char *data;
		Priority priority;
	};
#endif

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

#ifdef SMPS_EnablePSGNoiseDrums
	struct PSGNoiseDrum
	{
		unsigned char volume_envelope;
		unsigned char volume_attenuation;
		unsigned char noise_mode;
	};
#endif

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
#ifdef SMPS_EnablePSGNoiseDrums
		struct
		{
			const PSGNoiseDrum *list;
		} psg_noise_drums;
#endif
#ifdef SMPS_EnableUniversalVoiceBank
		const Voice *universal_voice_bank;
#endif
	};

	#ifndef BINARY_BLOB
	extern const Data* const data;
	#endif

	void UpdateDriver();
	unsigned int GetTrackNote(unsigned int track_id, bool modulation);
	void Initialise();
}

#endif // CLONE_DRIVER_CORE_H
