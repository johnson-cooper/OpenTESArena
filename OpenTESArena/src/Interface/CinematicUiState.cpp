#include "CinematicUiState.h"
#include "../Assets/ArenaAssetUtils.h"
#include "../Assets/FLCFile.h"
#include "../Assets/TextureManager.h"
#include "../Game/Game.h"

#include "components/utilities/StringView.h"

namespace
{
	constexpr char ElementName_VideoImage[] = "VideoImage";
}

CinematicUiInitInfo::CinematicUiInitInfo()
{
	this->secondsPerImage = 0.0;
	this->callback = []() { };
}

void CinematicUiInitInfo::init(const std::string &paletteName, const std::string &sequenceName, double secondsPerImage, const CinematicFinishedCallback &callback)
{
	this->paletteName = paletteName;
	this->sequenceName = sequenceName;
	this->secondsPerImage = secondsPerImage;
	this->callback = callback;
}

CinematicUiState::CinematicUiState()
{
	this->game = nullptr;
	this->contextInstID = -1;
	this->secondsPerImage = 0.0;
	this->currentSeconds = 0.0;
	this->streamTextureID = -1;
	this->imageIndex = -1;
	this->callback = []() { };
}

void CinematicUiState::init(Game &game)
{
	DebugAssert(this->initInfo.secondsPerImage > 0.0);

	this->game = &game;

	TextureManager &textureManager = game.textureManager;
	Renderer &renderer = game.renderer;
	const std::string &sequenceName = this->initInfo.sequenceName;
	const std::string &paletteName = this->initInfo.paletteName;

	const std::string_view extension = StringView::getExtension(sequenceName);
	const bool isVideo = StringView::caseInsensitiveEquals(extension, ArenaAssetUtils::EXTENSION_FLC) ||
		StringView::caseInsensitiveEquals(extension, ArenaAssetUtils::EXTENSION_CEL);

	if (isVideo)
	{
		// Stream the video: decode frames on demand into a single UI texture.
		auto stream = std::make_shared<FLCStream>();
		if (!stream->init(sequenceName.c_str()) || !stream->seekToFrame(0))
		{
			DebugLogErrorFormat("Couldn't open video stream \"%s\".", sequenceName.c_str());
			return;
		}

		// Same palette selection as per-frame textures: the palette asset's first palette for every frame.
		if (StringView::caseInsensitiveEquals(paletteName, sequenceName))
		{
			this->streamPalette = stream->getPalette();
		}
		else
		{
			const std::optional<PaletteID> paletteID = textureManager.tryGetPaletteID(TextureAsset(paletteName));
			if (paletteID.has_value())
			{
				this->streamPalette = textureManager.getPaletteHandle(*paletteID);
			}
			else
			{
				DebugLogErrorFormat("Couldn't get palette \"%s\" for video \"%s\".", paletteName.c_str(), sequenceName.c_str());
				this->streamPalette = stream->getPalette();
			}
		}

		this->streamTextureID = renderer.createUiTexture(stream->getWidth(), stream->getHeight());
		if (this->streamTextureID < 0)
		{
			DebugLogErrorFormat("Couldn't create UI texture for video \"%s\".", sequenceName.c_str());
			return;
		}

		this->flcStream = std::move(stream);
		const Span<const std::byte> texels(reinterpret_cast<const std::byte*>(this->flcStream->getPixels()), this->flcStream->getWidth() * this->flcStream->getHeight());
		renderer.populateUiTexture(this->streamTextureID, texels, &this->streamPalette);
	}
	else
	{
		const std::optional<TextureFileMetadataID> metadataID = textureManager.tryGetMetadataID(sequenceName.c_str());
		if (!metadataID.has_value())
		{
			DebugLogErrorFormat("Couldn't get texture file metadata for \"%s\".", sequenceName.c_str());
			return;
		}

		const TextureFileMetadata &textureFileMetadata = textureManager.getMetadataHandle(*metadataID);
		this->videoTextureIDs.init(textureFileMetadata.getTextureCount());
		this->videoTextureIDs.fill(-1);

		// Frames are allocated lazily: only the visible frame has a UI texture.
		this->tryAllocFrameTexture(0);
	}

	this->secondsPerImage = this->initInfo.secondsPerImage;
	this->currentSeconds = 0.0;
	this->imageIndex = 0;
	this->callback = this->initInfo.callback;
}

void CinematicUiState::freeTextures(Renderer &renderer)
{
	for (int i = 0; i < this->videoTextureIDs.getCount(); i++)
	{
		this->freeFrameTexture(i, renderer);
	}

	this->videoTextureIDs.clear();

	if (this->streamTextureID >= 0)
	{
		renderer.freeUiTexture(this->streamTextureID);
		this->streamTextureID = -1;
	}

	this->flcStream = nullptr;
}

int CinematicUiState::getFrameCount() const
{
	return (this->flcStream != nullptr) ? this->flcStream->getFrameCount() : this->videoTextureIDs.getCount();
}

UiTextureID CinematicUiState::getFrameTexture(int index)
{
	if (this->flcStream != nullptr)
	{
		if ((this->flcStream->getCurrentFrameIndex() != index) && this->flcStream->seekToFrame(index))
		{
			const Span<const std::byte> texels(reinterpret_cast<const std::byte*>(this->flcStream->getPixels()), this->flcStream->getWidth() * this->flcStream->getHeight());
			this->game->renderer.populateUiTexture(this->streamTextureID, texels, &this->streamPalette);
		}

		return this->streamTextureID;
	}

	return this->tryAllocFrameTexture(index) ? this->videoTextureIDs[index] : -1;
}

bool CinematicUiState::tryAllocFrameTexture(int index)
{
	if ((index < 0) || (index >= this->videoTextureIDs.getCount()))
	{
		return false;
	}

	if (this->videoTextureIDs[index] >= 0)
	{
		return true;
	}

	const TextureAsset textureAsset(this->initInfo.sequenceName, index);
	const TextureAsset paletteTextureAsset(this->initInfo.paletteName);
	UiTextureID textureID;
	if (!TextureUtils::tryAllocUiTexture(textureAsset, paletteTextureAsset, this->game->textureManager, this->game->renderer, &textureID))
	{
		DebugLogErrorFormat("Couldn't create UI texture for sequence \"%s\" frame %d.", this->initInfo.sequenceName.c_str(), index);
		return false;
	}

	this->videoTextureIDs.set(index, textureID);
	return true;
}

void CinematicUiState::freeFrameTexture(int index, Renderer &renderer)
{
	if ((index < 0) || (index >= this->videoTextureIDs.getCount()))
	{
		return;
	}

	const UiTextureID textureID = this->videoTextureIDs[index];
	if (textureID >= 0)
	{
		renderer.freeUiTexture(textureID);
		this->videoTextureIDs.set(index, -1);
	}
}

void CinematicUI::create(Game &game)
{
	CinematicUiState &state = CinematicUI::state;
	state.init(game);

	UiManager &uiManager = game.uiManager;
	InputManager &inputManager = game.inputManager;
	TextureManager &textureManager = game.textureManager;
	Renderer &renderer = game.renderer;

	const UiLibrary &uiLibrary = UiLibrary::getInstance();
	const UiContextDefinition &contextDef = uiLibrary.getDefinition(CinematicUI::ContextName);
	state.contextInstID = uiManager.createContext(contextDef, inputManager, textureManager, renderer);

	UiElementInitInfo videoImageElementInitInfo;
	videoImageElementInitInfo.name = ElementName_VideoImage;
	uiManager.createImage(videoImageElementInitInfo, state.getFrameTexture(0), state.contextInstID, renderer);

	game.setCursorOverride(std::nullopt);
	uiManager.setElementActive(game.cursorImageElementInstID, false);
}

void CinematicUI::destroy()
{
	CinematicUiState &state = CinematicUI::state;
	Game &game = *state.game;
	UiManager &uiManager = game.uiManager;
	InputManager &inputManager = game.inputManager;
	Renderer &renderer = game.renderer;

	if (state.contextInstID >= 0)
	{
		uiManager.freeContext(state.contextInstID, inputManager, renderer);
		state.contextInstID = -1;
	}

	state.freeTextures(renderer);
	state.secondsPerImage = 0.0;
	state.currentSeconds = 0.0;
	state.imageIndex = -1;
	state.callback = []() { };

	uiManager.setElementActive(game.cursorImageElementInstID, true);
}

void CinematicUI::update(double dt)
{
	CinematicUiState &state = CinematicUI::state;
	Game &game = *state.game;

	const int prevImageIndex = state.imageIndex;
	state.currentSeconds += dt;
	while (state.currentSeconds > state.secondsPerImage)
	{
		state.currentSeconds -= state.secondsPerImage;
		state.imageIndex++;
	}

	const int frameCount = state.getFrameCount();
	if (state.imageIndex >= frameCount)
	{
		state.imageIndex = frameCount - 1;
		CinematicUI::onSkipButtonSelected(MouseButtonType::Left);
	}

	if ((state.imageIndex != prevImageIndex) && (state.imageIndex >= 0))
	{
		const UiTextureID prevImageTextureID = (state.flcStream != nullptr) ? state.streamTextureID :
			(((prevImageIndex >= 0) && (prevImageIndex < state.videoTextureIDs.getCount())) ? state.videoTextureIDs[prevImageIndex] : -1);
		const UiTextureID currentImageTextureID = state.getFrameTexture(state.imageIndex);
		if ((currentImageTextureID >= 0) && (currentImageTextureID != prevImageTextureID))
		{
			UiManager &uiManager = game.uiManager;
			const UiElementInstanceID videoImageElementInstID = uiManager.getElementByName(ElementName_VideoImage);
			uiManager.setImageTexture(videoImageElementInstID, currentImageTextureID);
			state.freeFrameTexture(prevImageIndex, game.renderer);
		}
	}
}

void CinematicUI::onSkipButtonSelected(MouseButtonType mouseButtonType)
{
	CinematicUiState &state = CinematicUI::state;
	state.callback();
}

void CinematicUI::onSkipInputAction(const InputActionCallbackValues &values)
{
	if (values.performed)
	{
		CinematicUI::onSkipButtonSelected(MouseButtonType::Left);
	}
}
