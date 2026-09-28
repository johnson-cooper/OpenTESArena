#include "SDL.h"
#include "SDL_render.h"
#include "SDL_vulkan.h"

#include "ArenaRenderUtils.h"
#include "Window.h"
#include "../Math/Constants.h"
#include "../Math/Rect.h"
#include "../UI/Surface.h"
#include "../Utilities/Platform.h"

#include "components/debug/Debug.h"

namespace
{
	int GetSdlWindowPosition(RenderWindowMode windowMode)
	{
		switch (windowMode)
		{
		case RenderWindowMode::Window:
			return SDL_WINDOWPOS_CENTERED;
		case RenderWindowMode::BorderlessFullscreen:
		case RenderWindowMode::ExclusiveFullscreen:
			return SDL_WINDOWPOS_UNDEFINED;
		default:
			DebugUnhandledReturnMsg(int, std::to_string(static_cast<int>(windowMode)));
		}
	}

	uint32_t GetSdlWindowFlags(RenderWindowMode windowMode, uint32_t additionalFlags)
	{
		uint32_t flags = SDL_WINDOW_ALLOW_HIGHDPI;
		if (windowMode == RenderWindowMode::Window)
		{
			flags |= SDL_WINDOW_RESIZABLE;
		}
		else if (windowMode == RenderWindowMode::BorderlessFullscreen)
		{
			flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		}
		else if (windowMode == RenderWindowMode::ExclusiveFullscreen)
		{
			flags |= SDL_WINDOW_FULLSCREEN;
		}

		flags |= additionalFlags;

		return flags;
	}

	const char *GetSdlWindowTitle()
	{
		return "OpenTESArena";
	}

	Int2 GetWindowDimsForMode(RenderWindowMode windowMode, int fallbackWidth, int fallbackHeight)
	{
		if (windowMode == RenderWindowMode::ExclusiveFullscreen)
		{
			// Use desktop resolution of the primary display device. In the future, the display index could be
			// an option in the options menu.
			constexpr int displayIndex = 0;
			SDL_DisplayMode displayMode;
			const int result = SDL_GetDesktopDisplayMode(displayIndex, &displayMode);
			if (result == 0)
			{
				return Int2(displayMode.w, displayMode.h);
			}
			else
			{
				DebugLogErrorFormat("Couldn't get desktop %d display mode, using given window dimensions \"%dx%d\" (%s).", displayIndex, fallbackWidth, fallbackHeight, SDL_GetError());
			}
		}

		return Int2(fallbackWidth, fallbackHeight);
	}
}

Window::Window()
{
	this->window = nullptr;
	this->additionalFlags = 0;
	this->letterboxMode = 0;
	this->fullGameWindow = false;
}

Window::~Window()
{
	SDL_DestroyWindow(this->window);
	SDL_Quit();
}

bool Window::init(int width, int height, RenderWindowMode windowMode, uint32_t additionalFlags, int letterboxMode, bool fullGameWindow)
{
	DebugLog("Initializing.");

	const int result = SDL_Init(SDL_INIT_VIDEO); // Required for SDL_GetDesktopDisplayMode() to work for exclusive fullscreen.
	if (result != 0)
	{
		DebugLogErrorFormat("Couldn't init SDL video subsystem (result: %d, %s).", result, SDL_GetError());
		return false;
	}

	if ((width <= 0) || (height <= 0))
	{
		DebugLogErrorFormat("Invalid window dimensions %dx%d.", width, height);
		return false;
	}

	const char *windowTitle = GetSdlWindowTitle();
	const int windowPosition = GetSdlWindowPosition(windowMode);
	const uint32_t windowFlags = GetSdlWindowFlags(windowMode, additionalFlags);
	const Int2 windowDims = GetWindowDimsForMode(windowMode, width, height);
	this->window = SDL_CreateWindow(windowTitle, windowPosition, windowPosition, windowDims.x, windowDims.y, windowFlags);
	if (this->window == nullptr)
	{
		DebugLogErrorFormat("Couldn't create SDL_Window (dimensions: %dx%d, window mode: %d, %s).", width, height, windowMode, SDL_GetError());
		return false;
	}

	this->additionalFlags = additionalFlags;

	// Initialize display modes list for the current window.
	// @todo: these display modes will only work on the display device the window was initialized on
	const int displayIndex = SDL_GetWindowDisplayIndex(this->window);
	const int displayModeCount = SDL_GetNumDisplayModes(displayIndex);
	for (int i = 0; i < displayModeCount; i++)
	{
		// Convert SDL display mode to our display mode.
		SDL_DisplayMode mode;
		if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0)
		{
			// Filter away non-24-bit displays. Perhaps this could be handled better, but I don't
			// know how to do that for all possible displays out there.
			if (mode.format == SDL_PIXELFORMAT_RGB888)
			{
				this->displayModes.emplace_back(RenderDisplayMode(mode.w, mode.h, mode.refresh_rate));
			}
		}
	}

	this->letterboxMode = letterboxMode;
	this->fullGameWindow = fullGameWindow;

	return true;
}

Int2 Window::getLogicalDimensions() const
{
	int logicalWidth, logicalHeight;
	SDL_GetWindowSize(this->window, &logicalWidth, &logicalHeight);
	return Int2(logicalWidth, logicalHeight);
}

Int2 Window::getPixelDimensions() const
{
	// @todo SDL_GetWindowSizeInPixels() in newer SDL versions replaces these two functions
	int pixelWidth, pixelHeight;
	
	if ((this->additionalFlags & SDL_WINDOW_VULKAN) != 0)
	{
		SDL_Vulkan_GetDrawableSize(this->window, &pixelWidth, &pixelHeight);
	}
	else
	{
		SDL_Renderer *renderer = SDL_GetRenderer(this->window);
		if (renderer == nullptr)
		{
			DebugLogError("Couldn't get SDL_Renderer for window pixel dimensions.");
			return Int2();
		}
		
		SDL_GetRendererOutputSize(renderer, &pixelWidth, &pixelHeight);
	}
	
	return Int2(pixelWidth, pixelHeight);
}

void Window::setMode(RenderWindowMode mode)
{
	int result = 0;
	if (mode == RenderWindowMode::ExclusiveFullscreen)
	{
		SDL_DisplayMode displayMode;
		result = SDL_GetDesktopDisplayMode(0, &displayMode);
		if (result != 0)
		{
			DebugLogErrorFormat("Couldn't get desktop display mode for exclusive fullscreen (%s).", SDL_GetError());
			return;
		}

		result = SDL_SetWindowDisplayMode(this->window, &displayMode);
		if (result != 0)
		{
			DebugLogErrorFormat("Couldn't set window display mode to %dx%d %dHz for exclusive fullscreen (%s).", displayMode.w, displayMode.h, displayMode.refresh_rate, SDL_GetError());
			return;
		}
	}

	const uint32_t flags = GetSdlWindowFlags(mode, this->additionalFlags);
	result = SDL_SetWindowFullscreen(this->window, flags);
	if (result != 0)
	{
		DebugLogErrorFormat("Couldn't set window fullscreen flags to 0x%X (%s).", flags, SDL_GetError());
		return;
	}
}

void Window::setIcon(const Surface &icon)
{
	SDL_SetWindowIcon(this->window, icon.get());
}

void Window::setTitle(const char *title)
{
	SDL_SetWindowTitle(this->window, title);
}

void Window::warpMouse(int logicalX, int logicalY)
{
	SDL_WarpMouseInWindow(this->window, logicalX, logicalY);
}
