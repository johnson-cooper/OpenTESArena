// Global operator new/delete replacements for the PS2 build: allocation instrumentation.
//
// - Counts allocations/bytes per frame (heap churn in the main loop is visible in the periodic [PS2][mem] line).
// - Logs every large allocation with its caller address, so memory spikes can be attributed with the link map
//   (build/opentesarena-ps2.map) or `mips64r5900el-ps2-elf-addr2line`.
// - Failures go through the new-handler (Ps2Platform logs stats and shows the fatal screen); never silent.

#include <cstdio>
#include <cstdlib>
#include <new>

#include "Ps2Memory.h"

namespace
{
	Ps2Memory::FrameStats g_frameStats;
	size_t g_largeAllocationThreshold = 256 * 1024;

	void *AllocateOrHandle(std::size_t size, void *caller)
	{
		if (size == 0)
		{
			size = 1;
		}

		if (size >= g_largeAllocationThreshold)
		{
			std::printf("[PS2][alloc] %u KiB from %p\n", static_cast<unsigned>(size / 1024), caller);
		}

		while (true)
		{
			void *p = std::malloc(size);
			if (p != nullptr)
			{
				g_frameStats.allocations++;
				g_frameStats.bytes += size;
				return p;
			}

			std::printf("[PS2][alloc] FAILED %u bytes from %p\n", static_cast<unsigned>(size), caller);
			std::new_handler handler = std::get_new_handler();
			if (handler == nullptr)
			{
				std::abort();
			}

			handler();
		}
	}
}

void Ps2Memory::setLargeAllocationThreshold(size_t bytes)
{
	g_largeAllocationThreshold = bytes;
}

Ps2Memory::FrameStats Ps2Memory::takeFrameStats()
{
	const FrameStats stats = g_frameStats;
	g_frameStats = FrameStats();
	return stats;
}

void *operator new(std::size_t size)
{
	return AllocateOrHandle(size, __builtin_return_address(0));
}

void *operator new[](std::size_t size)
{
	return AllocateOrHandle(size, __builtin_return_address(0));
}

void *operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	if (size >= g_largeAllocationThreshold)
	{
		std::printf("[PS2][alloc] %u KiB (nothrow) from %p\n", static_cast<unsigned>(size / 1024), __builtin_return_address(0));
	}

	void *p = std::malloc((size == 0) ? 1 : size);
	if (p != nullptr)
	{
		g_frameStats.allocations++;
		g_frameStats.bytes += size;
	}

	return p;
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
	return operator new(size, tag);
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t&) noexcept { std::free(p); }
