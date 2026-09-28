#pragma once

// PS2: OpenAL is not available. AudioManager.h only needs these handle typedefs; on PS2 an "ALuint source"
// is a Ps2AudioManager voice handle (see ps2/audio/Ps2AudioManager.cpp).
#include <cstdint>

using ALuint = uint32_t;
using ALint = int32_t;
using ALenum = int32_t;
using ALfloat = float;
using ALboolean = char;
