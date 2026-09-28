// PS2 implementation of OpenTESArena's Platform namespace (replaces OpenTESArena/src/Utilities/Platform.cpp).

#include "Ps2Platform.h"

#include "OpenTESArena/src/Utilities/Platform.h"

std::string Platform::getPlatform()
{
	return "PlayStation 2";
}

std::string Platform::getBasePath()
{
	return Ps2Platform::getBasePath();
}

std::vector<std::string> Platform::getSteamArenaPaths()
{
	return std::vector<std::string>(); // No Steam/GOG installs on a console; Arena data is user-supplied under data/.
}

std::vector<std::string> Platform::getGogArenaPaths()
{
	return std::vector<std::string>();
}

std::string Platform::getOptionsPath()
{
	// Options live next to the ELF so the whole install is self-contained on USB. On read-only media this still
	// resolves; writing changes simply fails (logged) until memory card saves are implemented.
	return Ps2Platform::getBasePath() + "options/";
}

std::string Platform::getScreenshotPath()
{
	return Ps2Platform::getBasePath() + "screenshots/";
}

std::string Platform::getLogPath()
{
	return Ps2Platform::getBasePath() + "log/";
}

double Platform::getDefaultDPI()
{
	return 96.0;
}

int Platform::getThreadCount()
{
	return 1; // Single EE core. Rendering/physics never spawn worker threads on PS2.
}

int Platform::getCacheLineSize()
{
	return 64; // R5900 D-cache line size.
}

bool Platform::hasSSE()
{
	return false;
}

bool Platform::hasAVX()
{
	return false;
}
