#pragma once

#include <type_traits>

// PlayStation 2 compiler workaround.
//
// The EE toolchain (GCC, R5900, n32 ABI with -msingle-float) crashes with "maximum number of generated reload insns"
// when a struct with a `double` member is passed or returned by value in registers: the n32 ABI assigns such fields
// to 64-bit FPRs, which don't exist on the single-precision R5900 FPU. Giving double-based math types a user-provided
// copy constructor makes them non-trivial for the purposes of calls, so the C++ ABI passes them by invisible
// reference instead. Behavior is otherwise identical. Float instantiations stay trivially copyable (conditionally
// trivial special members, C++20).
//
// Expands to nothing on every other platform.
#if defined(__PS2__)
#define OTA_PLATFORM_ABI_COPYABLE(Type, T) \
	constexpr Type(const Type&) requires (!std::is_same_v<T, double>) = default; \
	constexpr Type(const Type &other) requires std::is_same_v<T, double> { *this = other; } \
	constexpr Type &operator=(const Type&) = default;
#else
#define OTA_PLATFORM_ABI_COPYABLE(Type, T)
#endif

// Same, for non-template types that always contain doubles.
#if defined(__PS2__)
#define OTA_PLATFORM_ABI_COPYABLE_DOUBLE(Type) \
	Type(const Type &other) { *this = other; } \
	Type &operator=(const Type&) = default;
#else
#define OTA_PLATFORM_ABI_COPYABLE_DOUBLE(Type)
#endif
