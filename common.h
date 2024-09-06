#ifndef COMMON_H
#define COMMON_H

#include <type_traits>

inline constexpr unsigned int WordFromBytes(const unsigned char byte1, const unsigned char byte2)
{
	const auto Fallback = [&]() constexpr
	{
		return static_cast<unsigned int>(byte1) << 8 | byte2;
	};

#ifdef __MEGA_DRIVE__
	if (std::is_constant_evaluated())
	{
		return Fallback();
	}
	else
	{
		unsigned int value;
		asm(
				"move.b	%1,-(%%sp)\n"
			"	move.w	(%%sp)+,%0\n"
			"	move.b	%2,%0"
			: "=&d" (value)
			: "idQm" (byte1), "idQm" (byte2)
			: "cc"
		);
		return value;
	}
#else
	return Fallback();
#endif	
}

inline constexpr unsigned int ReadWord(const unsigned char* const pointer)
{
	return WordFromBytes(pointer[0], pointer[1]);
}

inline constexpr unsigned int ReadUnalignedUnsignedWord(const unsigned char* const pointer)
{
	return ReadWord(pointer);
}

inline constexpr unsigned int ReadAlignedUnsignedWord(const unsigned char* const pointer)
{
#ifdef __MEGA_DRIVE__
	if (std::is_constant_evaluated())
	{
		return ReadWord(pointer);
	}
	else
	{
		unsigned int value;
		asm(
			"move.w	%1,%0"
			: "=&dQm" (value)
			: "Qm" (*pointer)
			: "cc"
		);
		return value;
	}
#else
	return ReadWord(pointer);
#endif
}

template<decltype(ReadUnalignedUnsignedWord) read_word>
inline constexpr signed int ReadSignedWord(const unsigned char* const pointer)
{
	const unsigned int value = read_word(pointer);

	return (value & 0x7FFF) - (value & 0x8000);
}

inline constexpr signed int ReadUnalignedSignedWord(const unsigned char* const pointer)
{
	return ReadSignedWord<ReadUnalignedUnsignedWord>(pointer);
}

inline constexpr signed int ReadAlignedSignedWord(const unsigned char* const pointer)
{
	return ReadSignedWord<ReadAlignedUnsignedWord>(pointer);
}

#ifdef BINARY_BLOB
	#define STARTING_FUNCTION __attribute__((visibility("default"))) __attribute__((section(".text.start")))
#else
	#define STARTING_FUNCTION
#endif

#endif // COMMON_H
