#ifndef CLONE_DRIVER_CORE_H
#define CLONE_DRIVER_CORE_H

#include <array>
#include <cassert>
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

	class Z80BusFMSafe : public ClownMDSDK::MainCPU::Z80::Bus
	{
	public:
		using ClownMDSDK::MainCPU::Z80::Bus::Bus;

	#ifdef __MEGA_DRIVE__
		~Z80BusFMSafe()
		{
			WaitUntilFMReady();

			asm(
				"move.b	#0x2A,%0"
				: "=Qm" (ClownMDSDK::MainCPU::FM::Unsafe::a0)
			);
		}
	#endif

		static decltype(auto) Lock(const auto &callback, const bool wait_for_bus = true)
		{
			return ClownMDSDK::MainCPU::M68k::DisableInterruptsTemporarily(
				[&]()
				{
					Z80BusFMSafe z80_bus(wait_for_bus);

					return callback(z80_bus);
				}
			);
		}
	};

	using Voice = std::array<unsigned char, 25>;

	struct ModulationSettings
	{
		unsigned char wait;
		unsigned char speed;
		signed char delta;
		unsigned char steps;
	};

	enum class ChannelID : unsigned char
	{
		FM1 = 0,
		FM2 = 1,
		FM3 = 2,
		FM4 = 4,
		FM5 = 5,
		FM6 = 6,

	#ifdef SMPS_EnablePWM
		PWM1 = 8 | 0,
		PWM2 = 8 | 2,
		PWM3 = 8 | 4,
		PWM4 = 8 | 6,
	#endif

		DAC = 0x10 | 6,

		PSG1 = 0x80,
		PSG2 = 0xA0,
		PSG3 = 0xC0,
		PSGNoise = 0xE0,
	};

	inline bool IsFM(const ChannelID id)
	{
		return std::to_underlying(id) < 0x08;
	}

#ifdef SMPS_EnablePWM
	inline bool IsPWM(const ChannelID id)
	{
		return (std::to_underlying(id) & 0x08) != 0;
	}
#endif

	inline bool IsDAC(const ChannelID id)
	{
		return (std::to_underlying(id) & 0x10) != 0;
	}

	inline bool IsPSG(const ChannelID id)
	{
		return (std::to_underlying(id) & 0x80) != 0;
	}

	inline unsigned int GetFMChannelID(const ChannelID id)
	{
		assert(IsFM(id));
		return std::to_underlying(id);
	}

	inline unsigned int GetFMPort(const ChannelID id)
	{
		return (GetFMChannelID(id) & 4) != 0;
	}

	inline unsigned int GetFMSlot(const ChannelID id)
	{
		return GetFMChannelID(id) & 3;
	}

	inline unsigned int GetPSGChannelID(const ChannelID id)
	{
		assert(IsPSG(id));
		return std::to_underlying(id);
	}

	struct Track
	{
		unsigned char playback_control;
		ChannelID voice_control;
		unsigned char tempo_divider;
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
		void UpdateTrack(this auto &self)
		{
			if (!self.IsPlaying())
				return;

		#ifdef SMPS_SoundTest
			self.SetSoundTest(false);
		#endif

			const bool begin_new_note = --self.duration_timeout == 0;

			self.UpdateTrackInternal(begin_new_note);
		}
		void cfJumpTo();
		void FinishTrackUpdate();
		void SetDuration(unsigned int duration);
		[[nodiscard]] bool CoordFlag(unsigned int value);
		[[nodiscard]] bool NoteTimeoutUpdate();
		[[nodiscard]] std::optional<unsigned int> DoModulationEnvelope();
		[[nodiscard]] bool IsMusic() const;
		[[nodiscard]] bool IsPSG3MelodyTrack() const;
	#ifdef SMPS_EnablePSGNoiseDrums
		[[nodiscard]] bool IsPSGDrumTrack() const;
	#endif
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
		decltype(auto) Visit(Ts &&...visitors);
	};

	struct YM2612Track : Track
	{
		unsigned char ams_fms_pan;

		void WriteFMIorII(Z80BusFMSafe &z80_bus, unsigned char port, unsigned char value);
	};

	struct DACTrack : YM2612Track
	{
		void UpdateSample();
		[[nodiscard]] bool DoNext();
		void UpdateTrackInternal(bool begin_new_note);
		void ApplyVolumeAttenuation(ClownMDSDK::MainCPU::Z80::Bus &z80_bus);
	};

	struct FMTrack : YM2612Track
	{
		const Voice* GetVoices();
		const Voice& GetVoice(const unsigned int index) { return GetVoices()[index]; }
		void SendVoiceTLCommon(Z80BusFMSafe &z80_bus, const Voice &voice, bool force_upload);
		void SendVoiceTL(Z80BusFMSafe &z80_bus);
		void SetVoice(Z80BusFMSafe &z80_bus);
		void SilenceChannel(Z80BusFMSafe &z80_bus);
		void SendNoteOff(Z80BusFMSafe &z80_bus);
		void NoteOff(Z80BusFMSafe &z80_bus);
		void NoteOff();
		void UpdateTrackInternal(bool begin_new_note);
		[[nodiscard]] bool UpdateFreq();
		[[nodiscard]] bool PrepareNote();
		[[nodiscard]] bool DoNext();
		void NoteOn();
		void SetFreq(unsigned char note);
	};

	struct FMMusicTrack : FMTrack
	{};

	struct FMSFXTrack : FMTrack
	{
		const Voice *voices;
	};

	struct PSGTrack : Track
	{
		unsigned char vol_env_index;

		[[nodiscard]] bool UpdateFreq();
		[[nodiscard]] bool DoNoteOn();
		[[nodiscard]] bool DoNext();
		void SetFreq(unsigned char note);
		[[nodiscard]] bool UpdateVolFX();
		[[nodiscard]] bool DoVolFX();
		void SetVolumeAttenuation(unsigned int psg_volume);
		void SendNoteOff();
		void NoteOff();
	};

	struct PSGMelodyTrack : PSGTrack
	{
		void UpdateTrackInternal(bool begin_new_note);
	};

	struct PSG3MelodyTrack : PSGMelodyTrack
	{
		unsigned char psg_noise;
	};

#ifdef SMPS_EnablePSGNoiseDrums
	struct PSGDrumTrack : PSGTrack
	{
		void UpdateTrackInternal(bool begin_new_note);
		[[nodiscard]] bool DoNext();
		void SetDrumNote(unsigned char note);
	};
#endif

#ifdef SMPS_EnablePWM
	struct PWMTrack : Track
	{};
#endif

	template<typename... Ts>
	inline decltype(auto) Track::Visit(Ts &&...visitors)
	{
		auto visitor = VisitHelper{std::forward<Ts>(visitors)...};

		if (IsDAC(voice_control))
		{
			return visitor(static_cast<DACTrack&>(*this));
		}
		else if (IsPSG(voice_control))
		{
#ifdef SMPS_EnablePSGNoiseDrums
			if (IsPSGDrumTrack())
				return visitor(static_cast<PSGDrumTrack&>(*this));
			else
#endif
			if (IsPSG3MelodyTrack())
				return visitor(static_cast<PSG3MelodyTrack&>(*this));
			else
				return visitor(static_cast<PSGMelodyTrack&>(*this));
		}
#ifdef SMPS_EnablePWM
		else if (IsPWM(voice_control))
		{
			return visitor(static_cast<PWMTrack&>(*this));
		}
#endif
		else
		{
			if (IsMusic())
				return visitor(static_cast<FMMusicTrack&>(*this));
			else
				return visitor(static_cast<FMSFXTrack&>(*this));
		}
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
		const Voice *music_voices;
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
		Variables variables;

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

		struct TrackCollection
		{
			void Update(this auto &self)
			{
				self.Iterate(
					[](auto &track)
					{
						track.UpdateTrack();
					}
				);
			}

			void IterateWithZ80Bus(this auto &self, const auto &callback)
			{
				Z80BusFMSafe::Lock(
					[&](Z80BusFMSafe &z80_bus)
					{
						self.Iterate(VisitHelper(
							[&callback, &z80_bus](DACTrack &track)
							{
								callback(track, z80_bus);
							},
							[&callback, &z80_bus](FMTrack &track)
							{
								callback(track, z80_bus);
							},
						#ifdef SMPS_EnablePWM
							[](PWMTrack&) {},
						#endif
						#ifdef SMPS_EnablePSGNoiseDrums
							[](PSGDrumTrack&) {},
						#endif
							[](PSGMelodyTrack&) {},
							[](PSG3MelodyTrack&) {}
						));
					}
				);

				self.Iterate(VisitHelper(
				#ifdef SMPS_EnablePWM
					[&callback](PWMTrack &track)
					{
						callback(track);
					},
				#endif
					[&callback](PSGMelodyTrack &track)
					{
						callback(track);
					},
					[&callback](PSG3MelodyTrack &track)
					{
						callback(track);
					},
				#ifdef SMPS_EnablePSGNoiseDrums
					[&callback](PSGDrumTrack &track)
					{
						callback(track);
					},
				#endif
					[](DACTrack&) {},
					[](FMTrack&) {}
				));
			}
		};

		struct Tracks : TrackCollection
		{
			struct Music : TrackCollection
			{
				DACTrack dac;
				std::array<FMMusicTrack, 6> fm;
				PSGMelodyTrack psg1;
				PSGMelodyTrack psg2;
				PSG3MelodyTrack psg3;
			#ifdef SMPS_EnablePSGNoiseDrums
				PSGDrumTrack psg_drum;
			#endif

				auto& FM1()  { return fm[0]; }
				auto& FM2()  { return fm[1]; }
				auto& FM3()  { return fm[2]; }
				auto& FM4()  { return fm[3]; }
				auto& FM5()  { return fm[4]; }
				auto& FM6()  { return fm[5]; }
				auto& PSG1() { return psg1; }
				auto& PSG2() { return psg2; }
				auto& PSG3() { return psg3; }
				Track& PSG(const unsigned int index)
				{
					switch (index)
					{
						case 0:
							return psg1;
						case 1:
							return psg2;
						case 2:
							return psg3;
					}

					std::unreachable();
				}

				void Iterate(const auto &callback)
				{
					callback(dac);
					for (auto &track : fm)
						callback(track);
					callback(psg1);
					callback(psg2);
					callback(psg3);
				#ifdef SMPS_EnablePSGNoiseDrums
					callback(psg_drum);
				#endif
				}

				// TODO: Add a 'clear' method?
			} music;

			class Shared
			{
			public:
				struct MusicBackup
				{
					Music tracks;
					Variables variables;
				};

				struct NonMusicTracks : TrackCollection
				{
					struct : TrackCollection
					{
						std::array<FMSFXTrack, 3> fm;
						PSGMelodyTrack psg1;
						PSGMelodyTrack psg2;
						PSG3MelodyTrack psg3;

						auto& FM3()  { return fm[0]; }
						auto& FM4()  { return fm[1]; }
						auto& FM5()  { return fm[2]; }
						auto& PSG1() { return psg1; }
						auto& PSG2() { return psg2; }
						auto& PSG3() { return psg3; }

						void Iterate(const auto &callback)
						{
							for (auto &track : fm)
								callback(track);
							callback(psg1);
							callback(psg2);
							callback(psg3);
						}
					} sfx;

				#ifdef SMPS_EnableSpecSFX
					struct : TrackCollection
					{
						FMSFXTrack fm4;
						PSG3MelodyTrack psg3;

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

				void CreateMusicBackup(const Music &tracks, const Variables &variables)
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
						[](MusicBackup&) {}
					);
				}

				void IterateNonMusicTracks(const auto &callback)
				{
					VisitNonMusicTracks(
						[&callback](auto &non_music_tracks)
						{
							non_music_tracks.Iterate(callback);
						}
					);
				}

				void UpdateNonMusicTracks()
				{
					VisitNonMusicTracks(
						[](auto &non_music_tracks)
						{
							non_music_tracks.Update();
						}
					);
				}

				NonMusicTracks& GetNonMusicTracks()
				{
					return Visit(
						[](NonMusicTracks &non_music_tracks) -> NonMusicTracks&
						{
							return non_music_tracks;
						},
						[](MusicBackup&) -> NonMusicTracks&
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
						[](NonMusicTracks&) -> MusicBackup&
						{
							std::unreachable();
						}
					);
				}
			} shared;

			void Iterate(const auto &callback)
			{
				music.Iterate(callback);
				shared.IterateNonMusicTracks(callback);
			}
		} tracks;
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
