#pragma once

#include <cstdint>

#include "SDL.h"

#include "OpenTESArena/src/Math/Vector2.h"

class Game;
class InputManager;
struct UiManager;

// DualShock 2 input backend.
//
// The pad is read once per frame and translated into OpenTESArena's *semantic* input actions (InputActionName) for
// whichever action maps are currently active. Each action is delivered through the binding the engine itself defines
// for it in InputActionMap, so the existing listener/UI code runs unchanged. On top of that:
//   - Analog sticks drive movement and camera look (with deadzone, sensitivity and optional Y inversion).
//   - Menus use a virtual pointer: the left stick moves it, the D-pad snaps it between UI buttons (no precise analog
//     aiming required), Cross clicks, Square right-clicks, L1/R1 scroll lists.
//   - Melee attacks: hold Square and flick the right stick to choose the swing direction (controller-native version
//     of Arena's mouse-gesture swings); a plain Square press swings in a random direction like modern mode.
//
// Default layout (see PS2_PORT.md):
//   Game world: LS move/strafe, RS look, Cross activate, Circle inspect, Square attack, Triangle character sheet,
//               R1 jump, L1 draw/sheathe weapon, L2 cast magic, R2 use item, Start pause menu, Select automap,
//               D-pad Up world map / Down camp / Left logbook / Right status, L3 player position, R3 steal.
//   Menus:      LS pointer, D-pad snap to buttons, Cross click, Circle back, Square right-click, Start accept,
//               L1/R1 scroll, Triangle backspace. In text entry: D-pad Up/Down pick letter, Right add, Left delete.
namespace Ps2Input
{
	struct Settings
	{
		float deadzone = 0.22f;        // Fraction of full stick deflection ignored.
		float lookSpeed = 320.0f;      // Virtual mouse pixels per second at full right-stick deflection.
		float cursorSpeed = 420.0f;    // Menu pointer pixels per second at full left-stick deflection.
		bool invertY = false;
	};

	void init();
	bool isConnected();

	Settings &getSettings();

	// Per-frame update, called at the start of InputManager::update().
	void update(InputManager &inputManager, Game &game, UiManager &uiManager, double dt);

	// Raw button state (active-high PAD_* bitmask); usable even from the fatal error screen.
	bool readButtonsRaw(uint16_t *outPressed);

	// --- Queries used by the SDL compat layer ---
	const uint8_t *getKeyboardState();
	uint16_t getModState();
	uint32_t getMouseButtonMask();
	Int2 getCursor();
	void warpCursor(int x, int y);
	Int2 consumeMouseDelta();
	void setRelativeMouseMode(bool enabled);
	bool isRelativeMouseMode();
	void setTextInputActive(bool active);
	bool isTextInputActive();

	void pushEvent(const SDL_Event &e);
	bool popEvent(SDL_Event *outEvent);

	// Controller-native melee: returns true (once) with a CardinalDirectionName value if the player chose a swing
	// direction with the right stick while holding attack.
	bool tryConsumeMeleeSwingDirection(int *outCardinalDirection);
}
