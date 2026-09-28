#include <cmath>

#include "ArenaRenderUtils.h"
#include "Window.h"
#include "../Math/Constants.h"
#include "../Math/Rect.h"

#include "components/debug/Debug.h"

// Platform-independent Window functions (letterboxing, coordinate conversion, scene view sizing). The windowing
// system specific parts live in Window.cpp (SDL, desktop) or ps2/platform/Ps2Window.cpp (PlayStation 2).

double Window::getLogicalToPixelScale() const
{
	const Int2 logicalDims = this->getLogicalDimensions();
	const Int2 pixelDims = this->getPixelDimensions();
	return static_cast<double>(pixelDims.x) / static_cast<double>(logicalDims.x);
}

double Window::getAspectRatio() const
{
	const Int2 dims = this->getPixelDimensions();
	return static_cast<double>(dims.x) / static_cast<double>(dims.y);
}

double Window::getLetterboxAspectRatio() const
{
	if (this->letterboxMode == 0)
	{
		return 16.0 / 10.0;
	}
	else if (this->letterboxMode == 1)
	{
		return 4.0 / 3.0;
	}
	else if (this->letterboxMode == 2)
	{
		// Stretch to fill.
		const Int2 windowDims = this->getPixelDimensions();
		return static_cast<double>(windowDims.x) / static_cast<double>(windowDims.y);
	}
	else
	{
		DebugUnhandledReturnMsg(double, std::to_string(this->letterboxMode));
	}
}

Int2 Window::getSceneViewDimensions() const
{
	constexpr double classicViewHeightRatio = static_cast<double>(ArenaRenderUtils::SCENE_VIEW_HEIGHT) / ArenaRenderUtils::SCREEN_HEIGHT_REAL;

	const Int2 windowDims = this->getPixelDimensions();
	const int viewHeight = this->fullGameWindow ? windowDims.y : static_cast<int>(std::ceil(windowDims.y * classicViewHeightRatio));

	return Int2(windowDims.x, viewHeight);
}

double Window::getSceneViewAspectRatio() const
{
	const Int2 viewDims = this->getSceneViewDimensions();
	return static_cast<double>(viewDims.x) / static_cast<double>(viewDims.y);
}

Rect Window::getLetterboxRect() const
{
	const Int2 windowDims = this->getPixelDimensions();
	const double windowAspect = static_cast<double>(windowDims.x) / static_cast<double>(windowDims.y);
	const double letterboxAspect = this->getLetterboxAspectRatio();

	Rect rect;

	// Compare the two aspects to decide what the letterbox dimensions are.
	if (std::abs(windowAspect - letterboxAspect) < Constants::Epsilon)
	{
		// Equal aspects. The letterbox is equal to the screen size.
		rect.x = 0;
		rect.y = 0;
		rect.width = windowDims.x;
		rect.height = windowDims.y;
	}
	else if (windowAspect > letterboxAspect)
	{
		// Native window is wider = empty left and right.
		const int subWidth = static_cast<int>(std::ceil(static_cast<double>(windowDims.y) * letterboxAspect));
		rect.x = (windowDims.x - subWidth) / 2;
		rect.y = 0;
		rect.width = subWidth;
		rect.height = windowDims.y;
	}
	else
	{
		// Native window is taller = empty top and bottom.
		const int subHeight = static_cast<int>(std::ceil(static_cast<double>(windowDims.x) / letterboxAspect));
		rect.x = 0;
		rect.y = (windowDims.y - subHeight) / 2;
		rect.width = windowDims.x;
		rect.height = subHeight;
	}

	return rect;
}

Int2 Window::nativeToOriginal(const Int2 &nativePoint) const
{
	const Rect letterboxRect = this->getLetterboxRect();
	const Int2 letterboxPoint(
		nativePoint.x - letterboxRect.x,
		nativePoint.y - letterboxRect.y);

	// Then from letterbox point to original point.
	const double letterboxXPercent = static_cast<double>(letterboxPoint.x) / static_cast<double>(letterboxRect.width);
	const double letterboxYPercent = static_cast<double>(letterboxPoint.y) / static_cast<double>(letterboxRect.height);

	const double originalWidthReal = ArenaRenderUtils::SCREEN_WIDTH_REAL;
	const double originalHeightReal = ArenaRenderUtils::SCREEN_HEIGHT_REAL;

	const Int2 originalPoint(
		static_cast<int>(originalWidthReal * letterboxXPercent),
		static_cast<int>(originalHeightReal * letterboxYPercent));

	return originalPoint;
}

Rect Window::nativeToOriginal(const Rect &nativeRect) const
{
	const Int2 newTopLeft = this->nativeToOriginal(nativeRect.getTopLeft());
	const Int2 newBottomRight = this->nativeToOriginal(nativeRect.getBottomRight());
	return Rect(
		newTopLeft.x,
		newTopLeft.y,
		newBottomRight.x - newTopLeft.x,
		newBottomRight.y - newTopLeft.y);
}

Int2 Window::originalToNative(const Int2 &originalPoint) const
{
	// From original point to letterbox point.
	const double originalXPercent = static_cast<double>(originalPoint.x) / ArenaRenderUtils::SCREEN_WIDTH_REAL;
	const double originalYPercent = static_cast<double>(originalPoint.y) / ArenaRenderUtils::SCREEN_HEIGHT_REAL;

	const Rect letterbox = this->getLetterboxRect();
	const double letterboxWidthReal = static_cast<double>(letterbox.width);
	const double letterboxHeightReal = static_cast<double>(letterbox.height);

	// Convert to letterbox point. Round to avoid off-by-one errors.
	const Int2 letterboxPoint(
		static_cast<int>(std::round(letterboxWidthReal * originalXPercent)),
		static_cast<int>(std::round(letterboxHeightReal * originalYPercent)));

	// Then from letterbox point to native point.
	const Int2 nativePoint(
		letterboxPoint.x + letterbox.x,
		letterboxPoint.y + letterbox.y);

	return nativePoint;
}

Rect Window::originalToNative(const Rect &originalRect) const
{
	const Int2 newTopLeft = this->originalToNative(originalRect.getTopLeft());
	const Int2 newBottomRight = this->originalToNative(originalRect.getBottomRight());
	return Rect(
		newTopLeft.x,
		newTopLeft.y,
		newBottomRight.x - newTopLeft.x,
		newBottomRight.y - newTopLeft.y);
}

bool Window::letterboxContains(const Int2 &nativePoint) const
{
	const Rect letterboxRect = this->getLetterboxRect();
	return letterboxRect.contains(nativePoint);
}
