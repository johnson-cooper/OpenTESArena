#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <exception>
#include <new>
#include <string>

#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <timer.h>
#include <debug.h>
// Only fileXioInit() is used: it switches newlib (fopen/stat/opendir/std::filesystem) onto iomanX devices.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <libpad.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Ps2Memory.h"
#include "Ps2Platform.h"
#include "ps2/input/Ps2Input.h"

// Embedded IOP modules (see embed_irx: in ps2.yaml). Only the ones the boot device needs are loaded.
extern unsigned char iomanx_irx[];
extern unsigned int size_iomanx_irx;
extern unsigned char filexio_irx[];
extern unsigned int size_filexio_irx;
extern unsigned char sio2man_irx[];
extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[];
extern unsigned int size_padman_irx;
extern unsigned char libsd_irx[];
extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;
extern unsigned char usbd_mini_irx[];
extern unsigned int size_usbd_mini_irx;
extern unsigned char bdm_irx[];
extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[];
extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char usbmass_bd_mini_irx[];
extern unsigned int size_usbmass_bd_mini_irx;

extern char _end[]; // Linker symbol: end of .bss (start of heap).
extern char _ftext[];

namespace
{
	Ps2Platform::BootDevice g_bootDevice = Ps2Platform::BootDevice::Unknown;
	std::string g_basePath;
	bool g_basePathWritable = true;
	bool g_padLoaded = false;
	bool g_audioLoaded = false;
	bool g_fileXioActive = false;
	Ps2Platform::VideoStandard g_videoStandard = Ps2Platform::VideoStandard::NTSC;

	size_t g_heapPeakUsed = 0;
	int64_t g_counters[static_cast<int>(Ps2Platform::Counter::COUNT)] = {};

	constexpr size_t EE_RAM_TOTAL = 32u * 1024u * 1024u;

	bool LoadIrx(const char *name, unsigned char *data, unsigned int size, int argLen = 0, const char *args = nullptr)
	{
		int moduleResult = -1;
		const int id = SifExecModuleBuffer(data, size, argLen, args, &moduleResult);
		if ((id < 0) || (moduleResult == 1)) // 1 = NO_RESIDENT_END (module chose to unload).
		{
			std::printf("[PS2] IRX %-16s FAILED (id=%d, result=%d, %u bytes)\n", name, id, moduleResult, size);
			return false;
		}

		std::printf("[PS2] IRX %-16s ok (id=%d, %u bytes)\n", name, id, size);
		return true;
	}

	Ps2Platform::BootDevice DetectBootDevice(const char *path)
	{
		if (path == nullptr)
		{
			return Ps2Platform::BootDevice::Unknown;
		}

		auto startsWith = [path](const char *prefix)
		{
			return strncmp(path, prefix, strlen(prefix)) == 0;
		};

		if (startsWith("host")) return Ps2Platform::BootDevice::Host;
		if (startsWith("mass")) return Ps2Platform::BootDevice::Mass;
		if (startsWith("cdrom") || startsWith("cdfs")) return Ps2Platform::BootDevice::Cdrom;
		if (startsWith("hdd") || startsWith("pfs")) return Ps2Platform::BootDevice::Hdd;
		if (startsWith("mc")) return Ps2Platform::BootDevice::MemoryCard;
		return Ps2Platform::BootDevice::Unknown;
	}

	// "mass:/OpenTESArena/opentesarena-ps2.elf" -> "mass:/OpenTESArena/". Handles "cdrom0:\PATH\FILE.ELF;1".
	std::string DirectoryOf(const char *path)
	{
		std::string s(path);
		for (char &c : s)
		{
			if (c == '\\')
			{
				c = '/';
			}
		}

		const size_t semicolon = s.find(';');
		if (semicolon != std::string::npos)
		{
			s.erase(semicolon);
		}

		const size_t slash = s.rfind('/');
		if (slash != std::string::npos)
		{
			return s.substr(0, slash + 1);
		}

		const size_t colon = s.find(':');
		if (colon != std::string::npos)
		{
			return s.substr(0, colon + 1);
		}

		return std::string();
	}

	bool PathExists(const char *path)
	{
		struct stat st;
		return stat(path, &st) == 0;
	}

	// Uncaught exceptions would otherwise abort() straight back to the PS2 browser with nothing on screen.
	[[noreturn]] void OnTerminate()
	{
		char message[512] = "std::terminate() called (uncaught exception or exception during unwinding).";
		if (std::exception_ptr ex = std::current_exception())
		{
			try
			{
				std::rethrow_exception(ex);
			}
			catch (const std::exception &e)
			{
				std::snprintf(message, sizeof(message), "Uncaught exception: %s", e.what());
			}
			catch (...)
			{
				std::snprintf(message, sizeof(message), "Uncaught non-std exception.");
			}
		}

		Ps2Platform::logMemoryStats("terminate");
		Ps2Platform::showFatalError("Fatal error", message);
		std::_Exit(EXIT_FAILURE);
	}

	void OnNewHandlerFailure()
	{
		// Never let an allocation failure silently corrupt memory: report and stop.
		Ps2Platform::logMemoryStats("OUT OF MEMORY");
		Ps2Platform::showFatalError("Out of memory", "An allocation failed (operator new). See TTY/log for memory statistics.");
		std::abort();
	}
}

void Ps2Platform::boot(int argc, char **argv)
{
	const char *argv0 = ((argc > 0) && (argv != nullptr)) ? argv[0] : nullptr;
	g_bootDevice = DetectBootDevice(argv0);
	std::printf("\n[PS2] OpenTESArena PS2 starting. argv[0]=\"%s\" boot device=%s\n", (argv0 != nullptr) ? argv0 : "(null)", getBootDeviceName());

	SifInitRpc(0);

	// Resetting the IOP gives a known module set, but it also removes the host: driver that ps2link/PCSX2 host
	// loading relies on, so host boots keep the current IOP state.
	const bool resetIop = g_bootDevice != BootDevice::Host;
	if (resetIop)
	{
		std::printf("[PS2] Resetting IOP.\n");
		while (!SifIopReset("", 0)) { }
		while (!SifIopSync()) { }
		SifInitRpc(0);
	}

	SifLoadFileInit();
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	// File I/O. With fileXio active, newlib (fopen/stat/opendir/std::filesystem) goes through iomanX devices such as
	// the BDM "mass" device. Host boots stay on the legacy ioman path so host: keeps working.
	if (resetIop)
	{
		const bool iomanxOk = LoadIrx("iomanx", iomanx_irx, size_iomanx_irx);
		const bool fileXioOk = iomanxOk && LoadIrx("filexio", filexio_irx, size_filexio_irx);
		if (fileXioOk && (fileXioInit() >= 0))
		{
			g_fileXioActive = true;
			std::printf("[PS2] fileXio active.\n");
		}
	}

	// Storage: only load what the boot device needs (IOP RAM is 2 MiB).
	if (g_bootDevice == BootDevice::Mass)
	{
		LoadIrx("usbd_mini", usbd_mini_irx, size_usbd_mini_irx);
		LoadIrx("bdm", bdm_irx, size_bdm_irx);
		LoadIrx("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
		LoadIrx("usbmass_bd_mini", usbmass_bd_mini_irx, size_usbmass_bd_mini_irx);
	}

	// Controller.
	g_padLoaded = LoadIrx("sio2man", sio2man_irx, size_sio2man_irx) && LoadIrx("padman", padman_irx, size_padman_irx);

	// Audio (SPU2 via libsd + audsrv).
	g_audioLoaded = LoadIrx("libsd", libsd_irx, size_libsd_irx) && LoadIrx("audsrv", audsrv_irx, size_audsrv_irx);

	// Base path: the directory the ELF launched from.
	if (argv0 != nullptr)
	{
		g_basePath = DirectoryOf(argv0);
	}

	// Some loaders pass a device-less or empty argv[0]; fall back to common locations.
	if (g_basePath.empty() || (g_basePath.find(':') == std::string::npos))
	{
		g_basePath = "mass:/OpenTESArena/";
		g_bootDevice = BootDevice::Mass;
	}

	// Normalize "mass0:" style prefixes to what BDM registers ("mass:" aliases "mass0:").
	if (g_bootDevice == BootDevice::Mass)
	{
		// USB enumeration is asynchronous; wait (bounded) for the device to appear.
		const double startSeconds = getSeconds();
		constexpr double timeoutSeconds = 6.0;
		bool found = false;
		while (true)
		{
			found = PathExists(g_basePath.c_str());
			if (found || ((getSeconds() - startSeconds) > timeoutSeconds))
			{
				break;
			}

			// ~50 ms between probes.
			const double waitUntil = getSeconds() + 0.05;
			while (getSeconds() < waitUntil)
			{
				nopdelay();
			}
		}

		std::printf("[PS2] USB device %s (%s).\n", found ? "ready" : "NOT FOUND", g_basePath.c_str());
	}

	g_basePathWritable = (g_bootDevice != BootDevice::Cdrom);

	// ROM region decides the default video standard (NTSC first; PAL wired but secondary).
	{
		char romver[16] = {};
		FILE *f = std::fopen("rom0:ROMVER", "r");
		if (f != nullptr)
		{
			std::fread(romver, 1, sizeof(romver) - 1, f);
			std::fclose(f);
			if (romver[4] == 'E')
			{
				g_videoStandard = VideoStandard::PAL;
			}
		}

		std::printf("[PS2] ROMVER=%.14s -> %s video.\n", romver, (g_videoStandard == VideoStandard::PAL) ? "PAL" : "NTSC");
	}

	std::set_new_handler(OnNewHandlerFailure);
	std::set_terminate(OnTerminate);
	Ps2Memory::setLargeAllocationThreshold(64 * 1024);

	Ps2Input::init();

	std::printf("[PS2] Base path: %s (writable: %s)\n", g_basePath.c_str(), g_basePathWritable ? "yes" : "no");

	// Development aid: a "debug-wait.txt" next to the ELF pauses boot so a debugger (PCSX2 DebugServer, ps2link) can
	// attach and set breakpoints before the engine starts.
	if (PathExists((g_basePath + "debug-wait.txt").c_str()))
	{
		std::printf("[PS2] debug-wait.txt found: waiting 10 seconds for a debugger...\n");
		const double waitUntil = getSeconds() + 10.0;
		while (getSeconds() < waitUntil)
		{
			nopdelay();
		}
	}
	logMemoryStats("boot");
}

Ps2Platform::BootDevice Ps2Platform::getBootDevice()
{
	return g_bootDevice;
}

const char *Ps2Platform::getBootDeviceName()
{
	switch (g_bootDevice)
	{
	case BootDevice::Host: return "host";
	case BootDevice::Mass: return "usb-mass";
	case BootDevice::Cdrom: return "cdrom";
	case BootDevice::Hdd: return "hdd";
	case BootDevice::MemoryCard: return "memcard";
	default: return "unknown";
	}
}

const std::string &Ps2Platform::getBasePath()
{
	return g_basePath;
}

bool Ps2Platform::isBasePathWritable()
{
	return g_basePathWritable;
}

Ps2Platform::VideoStandard Ps2Platform::getVideoStandard()
{
	return g_videoStandard;
}

bool Ps2Platform::isPadDriverLoaded()
{
	return g_padLoaded;
}

bool Ps2Platform::isAudioDriverLoaded()
{
	return g_audioLoaded;
}

uint64_t Ps2Platform::getTicks()
{
	return GetTimerSystemTime(); // Bus clock (147.456 MHz) based.
}

double Ps2Platform::getSeconds()
{
	return static_cast<double>(GetTimerSystemTime()) / static_cast<double>(kBUSCLK);
}

void Ps2Platform::sampleMemory()
{
	const struct mallinfo mi = mallinfo();
	const size_t used = static_cast<size_t>(mi.uordblks);
	if (used > g_heapPeakUsed)
	{
		g_heapPeakUsed = used;
	}
}

Ps2Platform::MemoryStats Ps2Platform::getMemoryStats()
{
	sampleMemory();
	const struct mallinfo mi = mallinfo();

	MemoryStats stats;
	stats.eeRamTotal = EE_RAM_TOTAL;
	stats.programBytes = static_cast<size_t>(reinterpret_cast<uintptr_t>(_end));
	stats.heapArenaBytes = static_cast<size_t>(mi.arena);
	stats.heapUsedBytes = static_cast<size_t>(mi.uordblks);
	stats.heapPeakUsedBytes = g_heapPeakUsed;
	stats.heapFreeInArena = static_cast<size_t>(mi.fordblks);

	const size_t claimed = stats.programBytes + stats.heapArenaBytes;
	stats.freeBytesEstimate = (claimed < EE_RAM_TOTAL) ? (EE_RAM_TOTAL - claimed) : 0;
	return stats;
}

void Ps2Platform::logMemoryStats(const char *label)
{
	const MemoryStats s = getMemoryStats();
	std::printf("[PS2][mem] %s: program(end)=%uK heapArena=%uK used=%uK peak=%uK freeInArena=%uK unclaimed~%uK | vram=%uK tex=%u\n",
		label, static_cast<unsigned>(s.programBytes / 1024), static_cast<unsigned>(s.heapArenaBytes / 1024),
		static_cast<unsigned>(s.heapUsedBytes / 1024), static_cast<unsigned>(s.heapPeakUsedBytes / 1024),
		static_cast<unsigned>(s.heapFreeInArena / 1024), static_cast<unsigned>(s.freeBytesEstimate / 1024),
		static_cast<unsigned>(getCounter(Counter::GsVramUsedBytes) / 1024), static_cast<unsigned>(getCounter(Counter::GsTextureCount)));
}

void Ps2Platform::setCounter(Counter counter, int64_t value)
{
	g_counters[static_cast<int>(counter)] = value;
}

int64_t Ps2Platform::getCounter(Counter counter)
{
	return g_counters[static_cast<int>(counter)];
}

void Ps2Platform::showFatalError(const char *title, const char *message)
{
	std::printf("\n[PS2] FATAL: %s\n%s\n", title, message);

	// Re-initialize the GS into a simple text console regardless of renderer state, so a crash on real hardware
	// never ends as a blank screen.
	init_scr();
	scr_clear();
	scr_printf("\n  OpenTESArena (PS2) - %s\n\n", title);

	// Wrap long messages for the ~70 column debug console.
	const char *p = message;
	char line[72];
	while (*p != '\0')
	{
		int n = 0;
		while ((p[n] != '\0') && (p[n] != '\n') && (n < 68))
		{
			n++;
		}

		std::memcpy(line, p, n);
		line[n] = '\0';
		scr_printf("  %s\n", line);
		p += n;
		if (*p == '\n')
		{
			p++;
		}
	}

	const MemoryStats s = getMemoryStats();
	scr_printf("\n  Heap used %uK (peak %uK), unclaimed ~%uK\n", static_cast<unsigned>(s.heapUsedBytes / 1024),
		static_cast<unsigned>(s.heapPeakUsedBytes / 1024), static_cast<unsigned>(s.freeBytesEstimate / 1024));
	scr_printf("  Boot device: %s  Base: %s\n", getBootDeviceName(), g_basePath.c_str());

	if (!g_padLoaded)
	{
		scr_printf("\n  (No controller driver - halted.)\n");
		SleepThread();
		return;
	}

	scr_printf("\n  Press START to exit.\n");

	// Raw pad read (opens the port if the input backend never initialized).
	while (true)
	{
		uint16_t pressed = 0;
		if (Ps2Input::readButtonsRaw(&pressed) && ((pressed & PAD_START) != 0))
		{
			return;
		}

		for (int i = 0; i < 1000; i++)
		{
			nopdelay();
		}
	}
}
