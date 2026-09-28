#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// PlayStation 2 platform services for OpenTESArena: boot/IOP module setup, boot-device detection, paths, timing,
// memory instrumentation and fatal error reporting. Engine code reaches this only through the existing platform
// boundaries (Platform::, Window, the render/audio/input backends and main()).
namespace Ps2Platform
{
	enum class BootDevice
	{
		Unknown,
		Host,       // host: (ps2link / PCSX2 host filesystem), development only.
		Mass,       // mass:/massN: (USB mass storage via BDM).
		Cdrom,      // cdrom0: (optical disc, read-only).
		Hdd,        // hdd0:/pfs: (internal HDD) - reserved, not wired yet.
		MemoryCard  // mc0:/mc1:
	};

	enum class VideoStandard
	{
		NTSC,
		PAL
	};

	// Must be the first call in main(). Resets/configures the IOP, loads only the IRX modules the detected boot
	// device needs, sets up file I/O, the controller and audio drivers, and resolves the base path from argv[0].
	void boot(int argc, char **argv);

	BootDevice getBootDevice();
	const char *getBootDeviceName();

	// Directory the ELF was launched from, with trailing slash (e.g. "mass:/OpenTESArena/"). All data is
	// resolved relative to this unless an absolute device path is configured.
	const std::string &getBasePath();

	// Whether files can be written next to the ELF (false for optical discs).
	bool isBasePathWritable();

	VideoStandard getVideoStandard();

	// True if the IRX for this subsystem loaded successfully during boot.
	bool isPadDriverLoaded();
	bool isAudioDriverLoaded();

	// Monotonic high-resolution time.
	uint64_t getTicks(); // EE bus clock ticks (147.456 MHz / 256 resolution depending on source).
	double getSeconds();

	// --- Memory instrumentation ---
	struct MemoryStats
	{
		size_t eeRamTotal;        // 32 MiB on retail hardware.
		size_t programBytes;      // ELF text+data+bss.
		size_t heapArenaBytes;    // Memory obtained from sbrk.
		size_t heapUsedBytes;     // In-use malloc bytes.
		size_t heapPeakUsedBytes; // Highest heapUsedBytes observed by sampleMemory().
		size_t heapFreeInArena;
		size_t freeBytesEstimate; // Remaining RAM not yet claimed by the heap (approximate; stack at top).
	};

	// Samples malloc statistics; call once per frame to keep the peak accurate.
	void sampleMemory();
	MemoryStats getMemoryStats();
	void logMemoryStats(const char *label);

	// Named subsystem counters shown in the debug overlay (GS VRAM, textures, chunks, audio buffers, ...).
	enum class Counter
	{
		GsVramUsedBytes,
		GsVramPeakBytes,
		GsTextureCount,
		GsTextureUploadBytesFrame,
		ObjectTextureBytes,
		UiTextureBytes,
		AudioBufferBytes,
		PhysicsBodies,
		DrawCalls,
		Triangles,
		COUNT
	};

	void setCounter(Counter counter, int64_t value);
	int64_t getCounter(Counter counter);

	// Displays an error on screen (usable even if the GS renderer failed or crashed) and on TTY, then waits for
	// START on the controller (or forever if no pad driver) and returns.
	void showFatalError(const char *title, const char *message);
}
