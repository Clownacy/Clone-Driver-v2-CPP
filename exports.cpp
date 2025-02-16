#include <array>
#include <limits>

#include "core.h"
#include "dac/DAC-Driver.h"

template<std::size_t N>
class Number {};

template<std::size_t N>
class Text : public std::array<char, N>
{
public:
	Text() = default;

	constexpr Text(const std::array<char, N> &array)
		: std::array<char, N>::array(array)
	{
	}

	template<typename T2>
	constexpr Text(T2 (&string)[N + 1])
		: Text([]<std::size_t... I>(T2 (&a)[N + 1], std::index_sequence<I...>) constexpr -> std::array<std::remove_cv_t<T2>, N>
		{
			return {{a[I]...}};
		}(string, std::make_index_sequence<N>{}))
	{
	}

	template<std::size_t N2>
	constexpr Text(Number<N2>)
	{
		unsigned int number = N2;
		for (unsigned int i = 0; i < N; ++i)
		{
			(*this)[this->size() - i - 1] = '0' + number % 10;
			number /= 10;
		}
	}

	constexpr Text(const char character)
		: std::array<char, N>::array{character}
	{
	}

	template<std::size_t N2>
	constexpr auto operator+(const Text<N2> &array)
	{
		Text<this->size() + N2> combined_array;

		std::copy(this->begin(), this->end(), combined_array.begin());
		std::copy(array.begin(), array.end(), combined_array.begin() + this->size());

		return combined_array;
	}
};

template<std::size_t N>
static constexpr auto MakeText(Number<N> number)
{
	static constexpr auto Log10 = [](unsigned int digit) constexpr -> unsigned int
	{
		if (digit == 0)
			return 1;

		unsigned int result = 0;
		while (digit != 0)
		{
			++result;
			digit /= 10;
		}
		return result;
	};

	return Text<Log10(N)>(number);
}

template<std::size_t N>
static constexpr auto MakeText()
{
	return MakeText(Number<N>());
}

template<typename T, std::size_t N>
static constexpr auto MakeText(const std::array<T, N> &array)
{
	return Text(array);
}

static constexpr auto MakeText(const char character)
{
	return Text<1>(character);
}

template<typename T, std::size_t N>
static constexpr auto MakeText(T (&string)[N])
{
	return Text<N - 1>(string);
}

template<std::size_t N>
static constexpr auto MakeText(const Text<N> &text)
{
	return text;
}

template<typename... T>
static constexpr auto CombineText(T &&...arrays)
{
	return (... + MakeText(arrays));
}

__attribute__((section(".exports"))) __attribute__((visibility("default"))) auto text = []()
{
	return CombineText(
		"SMPS_PAUSE_OFFSET = ", MakeText<offsetof(SMPS::State, pause)>(), '\n',
		"SMPS_QUEUE_OFFSET = ", MakeText<offsetof(SMPS::State, variables) + offsetof(SMPS::Variables, queue)>(), '\n',
		"SMPS_COMMUNICATION_BYTE_OFFSET = ", MakeText<offsetof(SMPS::State, variables) + offsetof(SMPS::Variables, communication_byte)>(), '\n',
		"SMPS_RAM_SIZE = ", MakeText<sizeof(SMPS::State)>(), '\n',
		"SMPS_DAC_BATCH_SIZE = ", MakeText<zBatchSize>(), '\n',
		"SMPS_DAC_SAMPLE_RATE = ", MakeText<zDriverSampleRate>(), '\n',
		"SMPS_NON_BACKUP_TRACK_COUNT = ", MakeText<SMPS::NON_BACKUP_TRACK_COUNT>(), '\n',
		"SMPS_MUSIC_FM_DAC_TRACK_COUNT = ", MakeText<SMPS::MUSIC_FM_DAC_TRACK_COUNT>(), '\n',
		"SMPS_MUSIC_FM_TRACK_COUNT = ", MakeText<SMPS::MUSIC_FM_TRACK_COUNT>(), '\n',
		"SMPS_MUSIC_PSG_TRACK_COUNT = ", MakeText<SMPS::MUSIC_PSG_TRACK_COUNT>(), '\n',
		"SMPS_SFX_FM_TRACK_COUNT = ", MakeText<SMPS::SFX_FM_TRACK_COUNT>(), '\n',
		"SMPS_SFX_PSG_TRACK_COUNT = ", MakeText<SMPS::SFX_PSG_TRACK_COUNT>(), '\n',
#ifdef SMPS_EnableSpecSFX
		"SMPS_SPECIAL_SFX_FM_TRACK_COUNT = ", MakeText<SMPS::SPECIAL_SFX_FM_TRACK_COUNT>(), '\n',
		"SMPS_SPECIAL_SFX_PSG_TRACK_COUNT = ", MakeText<SMPS::SPECIAL_SFX_PSG_TRACK_COUNT>(), '\n',
#endif
		"SMPS_MUSIC_TRACK_COUNT = ", MakeText<SMPS::MUSIC_TRACK_COUNT>(), '\n',
		"SMPS_SFX_TRACK_COUNT = ", MakeText<SMPS::SFX_TRACK_COUNT>(), '\n',
#ifdef SMPS_EnableSpecSFX
		"SMPS_SPECIAL_SFX_TRACK_COUNT = ", MakeText<SMPS::SPECIAL_SFX_TRACK_COUNT>(), '\n',
#endif
#ifdef SMPS_EnablePWM
		"SMPS_FEATURE_PWM = 1\n"
#else
		"SMPS_FEATURE_PWM = 0\n"
#endif
#ifdef SMPS_SoundTest
		"SMPS_FEATURE_SOUND_TEST = 1\n"
#else
		"SMPS_FEATURE_SOUND_TEST = 0\n"
#endif
#ifdef SMPS_EnableModulationEnvelopes
		"SMPS_FEATURE_MODULATION_ENVELOPES = 1\n"
#else
		"SMPS_FEATURE_MODULATION_ENVELOPES = 0\n"
#endif
#ifdef SMPS_EnableSpecSFX
		"SMPS_FEATURE_BACKGROUND_SFX = 1\n"
#else
		"SMPS_FEATURE_BACKGROUND_SFX = 0\n"
#endif
#ifdef SMPS_GloopSFXBehaviour
		"SMPS_FEATURE_GLOOP_SFX = 1\n"
#else
		"SMPS_FEATURE_GLOOP_SFX = 0\n"
#endif
#ifdef SMPS_EnableSpinDashSFX
		"SMPS_FEATURE_SPIN_DASH_SFX = 1\n"
#else
		"SMPS_FEATURE_SPIN_DASH_SFX = 0\n"
#endif
#ifdef SMPS_PushSFXBehaviour
		"SMPS_FEATURE_PUSH_SFX = 1\n"
#else
		"SMPS_FEATURE_PUSH_SFX = 0\n"
#endif
#ifdef SMPS_EnableContSFX
		"SMPS_FEATURE_CONTINUOUS_SFX = 1\n"
#else
		"SMPS_FEATURE_CONTINUOUS_SFX = 0\n"
#endif
#ifdef SMPS_EnableUniversalVoiceBank
		"SMPS_FEATURE_UNIVERSAL_VOICE_BANK = 1\n"
#else
		"SMPS_FEATURE_UNIVERSAL_VOICE_BANK = 0\n"
#endif
#ifdef SMPS_EnablePSGNoiseDrums
		"SMPS_FEATURE_PSG_NOISE_DRUMS = 1\n"
#else
		"SMPS_FEATURE_PSG_NOISE_DRUMS = 0\n"
#endif
	);
}();
