#pragma once

#include <cstddef>
#include <cstdint>

// Allocation instrumentation (see Ps2Memory.cpp, which replaces global operator new/delete on PS2).
namespace Ps2Memory
{
	struct FrameStats
	{
		uint32_t allocations = 0;
		uint64_t bytes = 0;
	};

	// Allocations at or above this size are logged with their caller address.
	void setLargeAllocationThreshold(size_t bytes);

	// Returns and resets the counters accumulated since the previous call.
	FrameStats takeFrameStats();
}
