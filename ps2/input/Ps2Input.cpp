#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <kernel.h>
#include <libpad.h>

#include "Ps2Input.h"
#include "ps2/platform/Ps2Platform.h"

#include "OpenTESArena/src/Game/Game.h"
#include "OpenTESArena/src/Input/InputActionMapName.h"
#include "OpenTESArena/src/Input/InputActionName.h"
#include "OpenTESArena/src/Input/InputActionType.h"
#include "OpenTESArena/src/Input/InputManager.h"
#include "OpenTESArena/src/Input/InputStateType.h"
#include "OpenTESArena/src/Input/PointerTypes.h"
#include "OpenTESArena/src/Player/PlayerLogic.h"
#include "OpenTESArena/src/UI/UiElement.h"
#include "OpenTESArena/src/UI/UiManager.h"
#include "OpenTESArena/src/World/CardinalDirectionName.h"

#include "components/debug/Debug.h"

namespace
{
	// --- Pad hardware state ---
	char g_padArea[256] __attribute__((aligned(64)));
	bool g_padOpened = false;
	bool g_dualShockRequested = false;
	bool g_connected = false;
	uint16_t g_buttons = 0; // Active-high PAD_* bits.
	uint16_t g_prevButtons = 0;
	float g_lx = 0.0f, g_ly = 0.0f, g_rx = 0.0f, g_ry = 0.0f; // -1..1 after deadzone.

	Ps2Input::Settings g_settings;

	// --- Virtual keyboard/mouse state exposed through the SDL compat layer ---
	uint8_t g_keyState[SDL_NUM_SCANCODES];
	uint16_t g_modState = KMOD_NONE;
	uint32_t g_mouseButtons = 0;
	float g_cursorX = 320.0f, g_cursorY = 224.0f;
	float g_mouseDeltaAccumX = 0.0f, g_mouseDeltaAccumY = 0.0f; // Sub-pixel carry.
	int g_mouseDeltaX = 0, g_mouseDeltaY = 0;
	bool g_relativeMouseMode = false;
	bool g_textInputActive = false;

	// Fixed-capacity event ring (no per-frame allocation).
	constexpr int EVENT_CAPACITY = 64;
	SDL_Event g_events[EVENT_CAPACITY];
	int g_eventHead = 0, g_eventCount = 0;

	// Text entry helper (no on-screen keyboard yet).
	char g_textEntryChar = 'A';

	// Controller-native melee.
	int g_queuedSwingDirection = -1;
	bool g_swingChosenThisPress = false;

	// Actions whose keys are currently held down because of a pad button (so they can be released).
	struct HeldKey
	{
		uint16_t padButton;
		SDL_Keycode keycode;
		uint16_t keymod;
		bool active;
	};

	constexpr int MAX_HELD = 24;
	HeldKey g_heldKeys[MAX_HELD];

	// Key synthesized by an analog stick (movement) that is held as long as the stick is deflected.
	struct StickKey
	{
		SDL_Scancode scancode;
		bool down;
	};

	StickKey g_stickKeys[4] = { { SDL_SCANCODE_W, false }, { SDL_SCANCODE_S, false }, { SDL_SCANCODE_A, false }, { SDL_SCANCODE_D, false } };

	float ApplyDeadzone(unsigned char raw)
	{
		const float v = (static_cast<float>(raw) - 127.5f) / 127.5f;
		const float dz = g_settings.deadzone;
		const float mag = std::fabs(v);
		if (mag <= dz)
		{
			return 0.0f;
		}

		const float scaled = std::min((mag - dz) / (1.0f - dz), 1.0f);
		return (v < 0.0f) ? -scaled : scaled;
	}

	bool IsPressedEdge(uint16_t button)
	{
		return ((g_buttons & button) != 0) && ((g_prevButtons & button) == 0);
	}

	bool IsReleasedEdge(uint16_t button)
	{
		return ((g_buttons & button) == 0) && ((g_prevButtons & button) != 0);
	}

	void PushKey(SDL_Keycode keycode, uint16_t keymod, bool down)
	{
		SDL_Event e;
		std::memset(&e, 0, sizeof(e));
		e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
		e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
		e.key.repeat = 0;
		e.key.keysym.sym = keycode;
		e.key.keysym.mod = keymod;
		e.key.keysym.scancode = SDL_GetScancodeFromKey(keycode);
		Ps2Input::pushEvent(e);

		const SDL_Scancode scancode = e.key.keysym.scancode;
		if ((scancode > SDL_SCANCODE_UNKNOWN) && (scancode < SDL_NUM_SCANCODES))
		{
			g_keyState[scancode] = down ? 1 : 0;
		}

		if ((keymod & KMOD_LCTRL) != 0)
		{
			g_keyState[SDL_SCANCODE_LCTRL] = down ? 1 : 0;
			g_modState = down ? (g_modState | KMOD_LCTRL) : (g_modState & ~KMOD_LCTRL);
		}
	}

	void PushMouseButton(int button, bool down)
	{
		SDL_Event e;
		std::memset(&e, 0, sizeof(e));
		e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		e.button.button = static_cast<Uint8>(button);
		e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
		e.button.clicks = 1;
		e.button.x = static_cast<int>(g_cursorX);
		e.button.y = static_cast<int>(g_cursorY);
		Ps2Input::pushEvent(e);

		if (down)
		{
			g_mouseButtons |= SDL_BUTTON(button);
		}
		else
		{
			g_mouseButtons &= ~SDL_BUTTON(button);
		}
	}

	void PushWheel(int y)
	{
		SDL_Event e;
		std::memset(&e, 0, sizeof(e));
		e.type = SDL_MOUSEWHEEL;
		e.wheel.y = y;
		Ps2Input::pushEvent(e);
	}

	void PushText(char c)
	{
		SDL_Event e;
		std::memset(&e, 0, sizeof(e));
		e.type = SDL_TEXTINPUT;
		e.text.text[0] = c;
		e.text.text[1] = '\0';
		Ps2Input::pushEvent(e);
	}

	// Looks up how the engine binds an action in the currently active maps. Only returns key bindings (mouse
	// bindings like Inspect are delivered as clicks by the caller).
	bool TryFindActiveKeyBinding(const InputManager &inputManager, const char *actionName, SDL_Keycode *outKeycode, uint16_t *outKeymod)
	{
		for (const InputActionMap &map : inputManager.getInputActionMaps())
		{
			if (!map.active || (g_textInputActive && !map.allowedDuringTextEntry))
			{
				continue;
			}

			for (const InputActionDefinition &def : map.defs)
			{
				if ((def.type == InputActionType::Key) && (def.name == actionName))
				{
					*outKeycode = def.keyDef.keycode;
					*outKeymod = def.keyDef.keymod;
					return true;
				}
			}
		}

		return false;
	}

	bool IsMapActive(const InputManager &inputManager, const char *mapName)
	{
		for (const InputActionMap &map : inputManager.getInputActionMaps())
		{
			if (map.active && (map.name == mapName))
			{
				return true;
			}
		}

		return false;
	}

	void ReleaseHeldKeysFor(uint16_t padButton)
	{
		for (HeldKey &held : g_heldKeys)
		{
			if (held.active && (held.padButton == padButton))
			{
				PushKey(held.keycode, held.keymod, false);
				held.active = false;
			}
		}
	}

	// Presses the first candidate action that is bound in an active map. The key stays down while the pad button is
	// held (for "Performing" actions like Jump) and is released on button release.
	bool TriggerAction(const InputManager &inputManager, uint16_t padButton, std::initializer_list<const char*> candidates)
	{
		for (const char *actionName : candidates)
		{
			SDL_Keycode keycode;
			uint16_t keymod;
			if (TryFindActiveKeyBinding(inputManager, actionName, &keycode, &keymod))
			{
				PushKey(keycode, keymod, true);
				for (HeldKey &held : g_heldKeys)
				{
					if (!held.active)
					{
						held = HeldKey{ padButton, keycode, keymod, true };
						break;
					}
				}

				return true;
			}
		}

		return false;
	}

	void SetStickKey(int index, bool down)
	{
		StickKey &key = g_stickKeys[index];
		if (key.down != down)
		{
			key.down = down;
			g_keyState[key.scancode] = down ? 1 : 0;
		}
	}

	void ReleaseStickKeys()
	{
		for (int i = 0; i < 4; i++)
		{
			SetStickKey(i, false);
		}
	}

	// Snap the virtual pointer to the nearest active UI button (or list item) in the D-pad direction.
	void SnapCursor(Game &game, UiManager &uiManager, int dirX, int dirY)
	{
		const Int2 cursor(static_cast<int>(g_cursorX), static_cast<int>(g_cursorY));
		float bestScore = 1.0e30f;
		Int2 bestPoint = cursor;
		bool found = false;

		auto consider = [&](const Rect &originalRect)
		{
			const Rect nativeRect = game.window.originalToNative(originalRect);
			const Int2 center(nativeRect.x + (nativeRect.width / 2), nativeRect.y + (nativeRect.height / 2));
			const float dx = static_cast<float>(center.x - cursor.x);
			const float dy = static_cast<float>(center.y - cursor.y);
			const float along = (dx * static_cast<float>(dirX)) + (dy * static_cast<float>(dirY));
			if (along <= 2.0f)
			{
				return; // Not in the pressed direction.
			}

			const float across = std::fabs((dx * static_cast<float>(dirY)) - (dy * static_cast<float>(dirX)));
			const float score = along + (across * 2.0f); // Prefer targets roughly in line.
			if (score < bestScore)
			{
				bestScore = score;
				bestPoint = center;
				found = true;
			}
		};

		for (const UiElementInstanceID id : uiManager.getTopMostActiveElementsOfType(UiElementType::Button))
		{
			consider(uiManager.getTransformGlobalRect(id));
		}

		for (const UiElementInstanceID id : uiManager.getTopMostActiveElementsOfType(UiElementType::ListBox))
		{
			const int count = uiManager.getListBoxItemCount(id);
			for (int i = 0; i < count; i++)
			{
				consider(uiManager.getListBoxItemGlobalRect(id, i));
			}
		}

		if (found)
		{
			g_cursorX = static_cast<float>(bestPoint.x);
			g_cursorY = static_cast<float>(bestPoint.y);
		}
		else
		{
			// Nothing in that direction: nudge.
			g_cursorX += static_cast<float>(dirX * 16);
			g_cursorY += static_cast<float>(dirY * 16);
		}
	}

	void ClampCursor(const Game &game)
	{
		const Int2 dims = game.window.getPixelDimensions();
		g_cursorX = std::clamp(g_cursorX, 0.0f, static_cast<float>(dims.x - 1));
		g_cursorY = std::clamp(g_cursorY, 0.0f, static_cast<float>(dims.y - 1));
	}

	void ReadPad()
	{
		g_prevButtons = g_buttons;
		if (!g_padOpened)
		{
			g_buttons = 0;
			return;
		}

		const int state = padGetState(0, 0);
		if (state == PAD_STATE_EXECCMD)
		{
			// Busy executing a command (e.g. switching to analog mode); keep the last reading.
			return;
		}

		if ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1))
		{
			if (g_connected)
			{
				std::printf("[PS2][pad] Controller disconnected (state %d).\n", state);
			}

			g_connected = false;
			g_dualShockRequested = false;
			g_buttons = 0;
			g_lx = g_ly = g_rx = g_ry = 0.0f;
			return;
		}

		if (!g_connected)
		{
			g_connected = true;
			std::printf("[PS2][pad] Controller connected (type %d).\n", padInfoMode(0, 0, PAD_MODECURID, 0));
		}

		// Put DualShock pads into analog mode and lock it so sticks always report.
		if (!g_dualShockRequested && (state == PAD_STATE_STABLE))
		{
			if (padInfoMode(0, 0, PAD_MODETABLE, -1) > 0)
			{
				padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
			}

			g_dualShockRequested = true;
		}

		struct padButtonStatus status;
		if (padRead(0, 0, &status) == 0)
		{
			return;
		}

		g_buttons = static_cast<uint16_t>(0xFFFF ^ status.btns);

		const int modeId = (status.mode >> 4);
		const bool hasSticks = (modeId == 0x5) || (modeId == 0x7); // Analog / DualShock.
		if (hasSticks)
		{
			g_lx = ApplyDeadzone(status.ljoy_h);
			g_ly = ApplyDeadzone(status.ljoy_v);
			g_rx = ApplyDeadzone(status.rjoy_h);
			g_ry = ApplyDeadzone(status.rjoy_v);
		}
		else
		{
			g_lx = g_ly = g_rx = g_ry = 0.0f;
		}
	}
}

void Ps2Input::init()
{
	std::memset(g_keyState, 0, sizeof(g_keyState));
	std::memset(g_heldKeys, 0, sizeof(g_heldKeys));

	if (!Ps2Platform::isPadDriverLoaded())
	{
		std::printf("[PS2][pad] No pad driver loaded; input disabled.\n");
		return;
	}

	if (padInit(0) != 1)
	{
		std::printf("[PS2][pad] padInit failed.\n");
		return;
	}

	if (padPortOpen(0, 0, g_padArea) == 0)
	{
		std::printf("[PS2][pad] padPortOpen(0,0) failed.\n");
		return;
	}

	g_padOpened = true;
	std::printf("[PS2][pad] Port 0 opened (libpad module version 0x%x).\n", padGetModVersion());
}

bool Ps2Input::isConnected()
{
	return g_connected;
}

Ps2Input::Settings &Ps2Input::getSettings()
{
	return g_settings;
}

bool Ps2Input::readButtonsRaw(uint16_t *outPressed)
{
	if (!g_padOpened)
	{
		init();
	}

	if (!g_padOpened)
	{
		return false;
	}

	const int state = padGetState(0, 0);
	if ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1))
	{
		return false;
	}

	struct padButtonStatus status;
	if (padRead(0, 0, &status) == 0)
	{
		return false;
	}

	*outPressed = static_cast<uint16_t>(0xFFFF ^ status.btns);
	return true;
}

void Ps2Input::update(InputManager &inputManager, Game &game, UiManager &uiManager, double dt)
{
	ReadPad();

	const float fdt = static_cast<float>(dt);
	const bool inGameWorld = game.shouldSimulateScene && IsMapActive(inputManager, InputActionMapName::GameWorld) && !g_textInputActive;

	// Release keys whose pad buttons went up.
	for (const uint16_t button : { PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2,
		PAD_START, PAD_SELECT, PAD_L3, PAD_R3, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT })
	{
		if (IsReleasedEdge(button))
		{
			ReleaseHeldKeysFor(button);
		}
	}

	if (inGameWorld)
	{
		// Pointer sits at screen center for center-screen interaction in free-look mode.
		const Int2 dims = game.window.getPixelDimensions();
		g_cursorX = static_cast<float>(dims.x / 2);
		g_cursorY = static_cast<float>(dims.y / 2);

		// Movement: left stick -> the engine's movement keys (W/S forward/back, A/D strafe in modern mode).
		constexpr float moveThreshold = 0.35f;
		SetStickKey(0, g_ly < -moveThreshold);
		SetStickKey(1, g_ly > moveThreshold);
		SetStickKey(2, g_lx < -moveThreshold);
		SetStickKey(3, g_lx > moveThreshold);

		// Attack: Square is the attack button. The engine's attack reads the right mouse button, so it is pressed
		// once a swing direction is chosen with the right stick (or on release for a plain tap = random swing), and
		// held for at least one full tick so PlayerLogic::handleAttack() sees it.
		const bool attackHeld = (g_buttons & PAD_SQUARE) != 0;
		static bool s_attackButtonSent = false;
		static bool s_releaseAttackNextFrame = false;
		if (s_releaseAttackNextFrame)
		{
			PushMouseButton(SDL_BUTTON_RIGHT, false);
			s_releaseAttackNextFrame = false;
			s_attackButtonSent = false;
		}

		if (IsPressedEdge(PAD_SQUARE))
		{
			g_swingChosenThisPress = false;
			s_attackButtonSent = false;
		}

		if (attackHeld && !s_attackButtonSent)
		{
			const float stickMag = std::sqrt((g_rx * g_rx) + (g_ry * g_ry));
			if (!g_swingChosenThisPress && (stickMag > 0.6f))
			{
				// Reuse the engine's own gesture -> direction mapping with a synthetic flick.
				const Int2 flick(static_cast<int>(g_rx * 100.0f), static_cast<int>(g_ry * 100.0f));
				CardinalDirectionName direction;
				if (PlayerLogic::tryGetMeleeSwingDirectionFromMouseDelta(flick, dims, &direction))
				{
					g_queuedSwingDirection = static_cast<int>(direction);
					g_swingChosenThisPress = true;
					PushMouseButton(SDL_BUTTON_RIGHT, true);
					s_attackButtonSent = true;
				}
			}
		}

		if (IsReleasedEdge(PAD_SQUARE))
		{
			if (!s_attackButtonSent)
			{
				// Plain tap: press now (engine picks a random swing), release next frame.
				PushMouseButton(SDL_BUTTON_RIGHT, true);
				s_releaseAttackNextFrame = true;
				s_attackButtonSent = true;
			}
			else
			{
				s_releaseAttackNextFrame = true;
			}
		}
		else
		{
			// Look: right stick -> relative mouse motion (engine applies its own sensitivity/inversion options too).
			const float ySign = g_settings.invertY ? -1.0f : 1.0f;
			g_mouseDeltaAccumX += g_rx * g_settings.lookSpeed * fdt;
			g_mouseDeltaAccumY += g_ry * g_settings.lookSpeed * fdt * ySign;
		}

		// Buttons -> semantic actions.
		if (IsPressedEdge(PAD_CROSS)) TriggerAction(inputManager, PAD_CROSS, { InputActionName::Activate });
		if (IsPressedEdge(PAD_CIRCLE)) { PushMouseButton(SDL_BUTTON_LEFT, true); PushMouseButton(SDL_BUTTON_LEFT, false); } // Inspect.
		if (IsPressedEdge(PAD_TRIANGLE)) TriggerAction(inputManager, PAD_TRIANGLE, { InputActionName::CharacterSheet });
		if (IsPressedEdge(PAD_R1)) TriggerAction(inputManager, PAD_R1, { InputActionName::Jump });
		if (IsPressedEdge(PAD_L1)) TriggerAction(inputManager, PAD_L1, { InputActionName::ToggleWeapon });
		if (IsPressedEdge(PAD_L2)) TriggerAction(inputManager, PAD_L2, { InputActionName::CastMagic });
		if (IsPressedEdge(PAD_R2)) TriggerAction(inputManager, PAD_R2, { InputActionName::UseItem });
		if (IsPressedEdge(PAD_START)) TriggerAction(inputManager, PAD_START, { InputActionName::PauseMenu });
		if (IsPressedEdge(PAD_SELECT)) TriggerAction(inputManager, PAD_SELECT, { InputActionName::Automap });
		if (IsPressedEdge(PAD_UP)) TriggerAction(inputManager, PAD_UP, { InputActionName::WorldMap });
		if (IsPressedEdge(PAD_DOWN)) TriggerAction(inputManager, PAD_DOWN, { InputActionName::Camp });
		if (IsPressedEdge(PAD_LEFT)) TriggerAction(inputManager, PAD_LEFT, { InputActionName::Logbook });
		if (IsPressedEdge(PAD_RIGHT)) TriggerAction(inputManager, PAD_RIGHT, { InputActionName::Status });
		if (IsPressedEdge(PAD_L3)) TriggerAction(inputManager, PAD_L3, { InputActionName::PlayerPosition });
		if (IsPressedEdge(PAD_R3)) TriggerAction(inputManager, PAD_R3, { InputActionName::Steal });
	}
	else
	{
		ReleaseStickKeys();
		if ((g_mouseButtons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0)
		{
			PushMouseButton(SDL_BUTTON_RIGHT, false);
		}

		if (g_textInputActive)
		{
			// Minimal controller text entry until an on-screen keyboard exists.
			if (IsPressedEdge(PAD_UP)) g_textEntryChar = (g_textEntryChar >= 'Z') ? ' ' : ((g_textEntryChar == ' ') ? 'A' : static_cast<char>(g_textEntryChar + 1));
			if (IsPressedEdge(PAD_DOWN)) g_textEntryChar = (g_textEntryChar == ' ') ? 'Z' : ((g_textEntryChar <= 'A') ? ' ' : static_cast<char>(g_textEntryChar - 1));
			if (IsPressedEdge(PAD_RIGHT) || IsPressedEdge(PAD_CROSS)) PushText(g_textEntryChar);
			if (IsPressedEdge(PAD_LEFT) || IsPressedEdge(PAD_TRIANGLE)) TriggerAction(inputManager, PAD_LEFT, { InputActionName::Backspace });
		}
		else
		{
			// Pointer: left stick moves it, D-pad snaps between UI elements.
			g_cursorX += g_lx * g_settings.cursorSpeed * fdt;
			g_cursorY += g_ly * g_settings.cursorSpeed * fdt;
			if (IsPressedEdge(PAD_UP)) SnapCursor(game, uiManager, 0, -1);
			if (IsPressedEdge(PAD_DOWN)) SnapCursor(game, uiManager, 0, 1);
			if (IsPressedEdge(PAD_LEFT)) SnapCursor(game, uiManager, -1, 0);
			if (IsPressedEdge(PAD_RIGHT)) SnapCursor(game, uiManager, 1, 0);
			ClampCursor(game);

			if (IsPressedEdge(PAD_CROSS)) PushMouseButton(SDL_BUTTON_LEFT, true);
			if (IsReleasedEdge(PAD_CROSS)) PushMouseButton(SDL_BUTTON_LEFT, false);
			if (IsPressedEdge(PAD_SQUARE)) PushMouseButton(SDL_BUTTON_RIGHT, true);
			if (IsReleasedEdge(PAD_SQUARE)) PushMouseButton(SDL_BUTTON_RIGHT, false);
			if (IsPressedEdge(PAD_L1)) PushWheel(1);
			if (IsPressedEdge(PAD_R1)) PushWheel(-1);
			if (IsPressedEdge(PAD_TRIANGLE)) TriggerAction(inputManager, PAD_TRIANGLE, { InputActionName::Backspace });
		}

		if (IsPressedEdge(PAD_CIRCLE)) TriggerAction(inputManager, PAD_CIRCLE, { InputActionName::Back, InputActionName::Skip });
		if (IsPressedEdge(PAD_START)) TriggerAction(inputManager, PAD_START, { InputActionName::Accept, InputActionName::Skip });
		if (IsPressedEdge(PAD_SELECT)) TriggerAction(inputManager, PAD_SELECT, { InputActionName::Skip });
	}

	// Convert accumulated look motion to whole virtual pixels, carrying the fraction.
	const float wholeX = std::trunc(g_mouseDeltaAccumX);
	const float wholeY = std::trunc(g_mouseDeltaAccumY);
	g_mouseDeltaX += static_cast<int>(wholeX);
	g_mouseDeltaY += static_cast<int>(wholeY);
	g_mouseDeltaAccumX -= wholeX;
	g_mouseDeltaAccumY -= wholeY;
}

const uint8_t *Ps2Input::getKeyboardState()
{
	return g_keyState;
}

uint16_t Ps2Input::getModState()
{
	return g_modState;
}

uint32_t Ps2Input::getMouseButtonMask()
{
	return g_mouseButtons;
}

Int2 Ps2Input::getCursor()
{
	return Int2(static_cast<int>(g_cursorX), static_cast<int>(g_cursorY));
}

void Ps2Input::warpCursor(int x, int y)
{
	g_cursorX = static_cast<float>(x);
	g_cursorY = static_cast<float>(y);
}

Int2 Ps2Input::consumeMouseDelta()
{
	const Int2 delta(g_mouseDeltaX, g_mouseDeltaY);
	g_mouseDeltaX = 0;
	g_mouseDeltaY = 0;
	return delta;
}

void Ps2Input::setRelativeMouseMode(bool enabled)
{
	g_relativeMouseMode = enabled;
}

bool Ps2Input::isRelativeMouseMode()
{
	return g_relativeMouseMode;
}

void Ps2Input::setTextInputActive(bool active)
{
	g_textInputActive = active;
	g_textEntryChar = 'A';
}

bool Ps2Input::isTextInputActive()
{
	return g_textInputActive;
}

void Ps2Input::pushEvent(const SDL_Event &e)
{
	if (g_eventCount >= EVENT_CAPACITY)
	{
		std::printf("[PS2][pad] Event queue full, dropping event 0x%x.\n", static_cast<unsigned>(e.type));
		return;
	}

	const int index = (g_eventHead + g_eventCount) % EVENT_CAPACITY;
	g_events[index] = e;
	g_eventCount++;
}

bool Ps2Input::popEvent(SDL_Event *outEvent)
{
	if (g_eventCount == 0)
	{
		return false;
	}

	*outEvent = g_events[g_eventHead];
	g_eventHead = (g_eventHead + 1) % EVENT_CAPACITY;
	g_eventCount--;
	return true;
}

bool Ps2Input::tryConsumeMeleeSwingDirection(int *outCardinalDirection)
{
	if (g_queuedSwingDirection < 0)
	{
		return false;
	}

	*outCardinalDirection = g_queuedSwingDirection;
	g_queuedSwingDirection = -1;
	return true;
}
