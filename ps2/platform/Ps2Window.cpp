// PS2 implementation of the windowing-system half of OpenTESArena's Window (the shared letterbox/coordinate math is in
// OpenTESArena/src/Rendering/WindowCommon.cpp).
//
// The "window" is the TV picture. It is exposed to the engine as a square-pixel 4:3 space (640x448 NTSC, 640x512
// PAL) so Arena's 320x200 UI maps with correct CRT aspect. The GS render backend maps this space onto the actual
// interlaced display buffers.

#include "Ps2Platform.h"
#include "Ps2Video.h"
#include "ps2/input/Ps2Input.h"

#include "OpenTESArena/src/Math/Vector2.h"
#include "OpenTESArena/src/Rendering/Window.h"
#include "OpenTESArena/src/UI/Surface.h"

#include "components/debug/Debug.h"

Window::Window()
{
	this->window = nullptr;
	this->additionalFlags = 0;
	this->letterboxMode = 0;
	this->fullGameWindow = false;
}

Window::~Window()
{
	this->window = nullptr;
}

bool Window::init(int width, int height, RenderWindowMode windowMode, uint32_t additionalFlags, int letterboxMode, bool fullGameWindow)
{
	static_cast<void>(width);
	static_cast<void>(height);
	static_cast<void>(windowMode);

	const Int2 dims = Ps2Video::getLogicalDimensions();
	DebugLogFormat("Initializing PS2 display window (%dx%d logical, %s).", dims.x, dims.y, Ps2Video::getModeName());

	this->additionalFlags = additionalFlags;
	this->displayModes.clear();
	this->displayModes.emplace_back(RenderDisplayMode(dims.x, dims.y, Ps2Video::getRefreshRate()));

	// Arena's UI is 320x200 shown on a 4:3 CRT; the TV is always 4:3 here, so force the 4:3 letterbox.
	this->letterboxMode = 1;
	this->fullGameWindow = fullGameWindow;
	static_cast<void>(letterboxMode);
	return true;
}

Int2 Window::getLogicalDimensions() const
{
	return Ps2Video::getLogicalDimensions();
}

Int2 Window::getPixelDimensions() const
{
	return Ps2Video::getLogicalDimensions();
}

void Window::setMode(RenderWindowMode mode)
{
	static_cast<void>(mode); // Always fullscreen TV output.
}

void Window::setIcon(const Surface &icon)
{
	static_cast<void>(icon);
}

void Window::setTitle(const char *title)
{
	static_cast<void>(title);
}

void Window::warpMouse(int logicalX, int logicalY)
{
	Ps2Input::warpCursor(logicalX, logicalY);
}
