#pragma once

#include <memory>

#include "../Rendering/RenderTextureUtils.h"
#include "../Utilities/Palette.h"
#include "../UI/UiContext.h"
#include "../UI/UiElement.h"
#include "../UI/UiLibrary.h"

class FLCStream;
class Game;

enum class MouseButtonType;

using CinematicFinishedCallback = std::function<void()>;

// Must be set before UI context is started.
struct CinematicUiInitInfo
{
	std::string paletteName;
	std::string sequenceName;
	double secondsPerImage;
	CinematicFinishedCallback callback;

	CinematicUiInitInfo();

	void init(const std::string &paletteName, const std::string &sequenceName, double secondsPerImage, const CinematicFinishedCallback &callback);
};

struct CinematicUiState
{
	CinematicUiInitInfo initInfo;

	Game *game;
	UiContextInstanceID contextInstID;

	Buffer<UiTextureID> videoTextureIDs; // Per-frame textures for non-streamed sequences (allocated lazily).

	// Streamed .FLC/.CEL playback: one decoder and one reused UI texture regardless of video length.
	std::shared_ptr<FLCStream> flcStream;
	UiTextureID streamTextureID;
	Palette streamPalette;
	double secondsPerImage, currentSeconds;
	int imageIndex;
	CinematicFinishedCallback callback;

	CinematicUiState();

	void init(Game &game);
	void freeTextures(Renderer &renderer);

	// Streaming: creates/frees the UI texture of a single frame.
	bool tryAllocFrameTexture(int index);
	void freeFrameTexture(int index, Renderer &renderer);

	int getFrameCount() const;
	UiTextureID getFrameTexture(int index); // Streams/allocates as needed.
};

namespace CinematicUI
{
	DECLARE_UI_CONTEXT(Cinematic);

	// @improvement this could be a mouse button change handler instead so the invisible mouse skip never fails even outside the 320x200 range
	void onSkipButtonSelected(MouseButtonType mouseButtonType);

	void onSkipInputAction(const InputActionCallbackValues &values);

	constexpr std::pair<const char*, UiButtonDefinitionCallback> ButtonCallbacks[] =
	{
		DECLARE_UI_FUNC(CinematicUI, onSkipButtonSelected)
	};

	constexpr std::pair<const char*, UiInputListenerDefinitionCallback> InputActionCallbacks[] =
	{
		DECLARE_UI_FUNC(CinematicUI, onSkipInputAction)
	};
}
