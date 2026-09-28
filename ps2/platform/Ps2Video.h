#pragma once

#include "Ps2Platform.h"

#include "OpenTESArena/src/Math/Vector2.h"

// Display geometry for the PS2 TV output.
//
// Engine-facing ("logical") space is square-pixel 4:3: 640x448 (NTSC) / 640x512 (PAL). The GS draws into
// interlaced *field* buffers of half height (640x224 / 640x256), so logical Y is halved when rasterizing. This keeps
// VRAM use low (16-bit color + 16-bit Z at 640x224 = 840 KiB total for double-buffered color + depth) while still
// giving Arena's 320x200 UI a 2x horizontal scale for readable text on a TV.
namespace Ps2Video
{
	inline bool isPal()
	{
		return Ps2Platform::getVideoStandard() == Ps2Platform::VideoStandard::PAL;
	}

	inline Int2 getLogicalDimensions()
	{
		return isPal() ? Int2(640, 512) : Int2(640, 448);
	}

	inline Int2 getFramebufferDimensions()
	{
		const Int2 logical = getLogicalDimensions();
		return Int2(logical.x, logical.y / 2);
	}

	inline int getRefreshRate()
	{
		return isPal() ? 50 : 60;
	}

	inline const char *getModeName()
	{
		return isPal() ? "PAL 640x512i (field)" : "NTSC 640x448i (field)";
	}
}
