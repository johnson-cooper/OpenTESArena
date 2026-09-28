#pragma once

// PlayStation 2 SDL2 *subset* for OpenTESArena.
//
// This is not an SDL port. The shared engine headers expose a handful of SDL data types (SDL_Surface as a
// 32-bit scratch image, SDL_Rect, SDL_Event/keycodes in the input-action system, SDL_BYTEORDER). On PS2 those
// names resolve to these minimal definitions and every function is implemented by a native PS2 module:
//   - Surfaces/BMP:    ps2/compat/SdlCompat.cpp (plain EE memory, no GPU involvement)
//   - Events/keyboard/mouse state: ps2/input/Ps2Input.cpp (DualShock 2 -> semantic actions)
//   - Message boxes:   ps2/platform (TTY + on-screen fatal error screen)
// No SDL video, audio, threads or renderer exist on PS2; those engine files are replaced wholesale.

#include <cstddef>
#include <cstdint>
#include <cstring>

using Uint8 = uint8_t;
using Uint16 = uint16_t;
using Uint32 = uint32_t;
using Sint16 = int16_t;
using Sint32 = int32_t;
using Uint64 = uint64_t;

enum SDL_bool
{
	SDL_FALSE = 0,
	SDL_TRUE = 1
};

// ------------------------------------------------------------------------------------------------
// Version / endianness
// ------------------------------------------------------------------------------------------------
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 30
#define SDL_PATCHLEVEL 0
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) (SDL_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))

#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER SDL_LIL_ENDIAN // The R5900 runs little-endian.

#define SDL_memcpy std::memcpy
#define SDL_memset std::memset

// ------------------------------------------------------------------------------------------------
// Rects / pixels / surfaces
// ------------------------------------------------------------------------------------------------
struct SDL_Rect
{
	int x, y;
	int w, h;
};

struct SDL_Point
{
	int x, y;
};

enum SDL_PixelFormatEnum : Uint32
{
	SDL_PIXELFORMAT_UNKNOWN = 0,
	SDL_PIXELFORMAT_INDEX8 = 0x13000801u,
	SDL_PIXELFORMAT_RGB24 = 0x17101803u,
	SDL_PIXELFORMAT_BGR24 = 0x17401803u,
	SDL_PIXELFORMAT_ARGB8888 = 0x16362004u,
	SDL_PIXELFORMAT_RGBA8888 = 0x16462004u,
	SDL_PIXELFORMAT_ABGR8888 = 0x16762004u,
	SDL_PIXELFORMAT_BGRA8888 = 0x16862004u,
	SDL_PIXELFORMAT_RGBA32 = SDL_PIXELFORMAT_ABGR8888, // Byte order R,G,B,A on little endian (== GS PSMCT32).
	SDL_PIXELFORMAT_ARGB32 = SDL_PIXELFORMAT_BGRA8888,
	SDL_PIXELFORMAT_BGRA32 = SDL_PIXELFORMAT_ARGB8888,
	SDL_PIXELFORMAT_ABGR32 = SDL_PIXELFORMAT_RGBA8888
};

struct SDL_Color
{
	Uint8 r, g, b, a;
};

struct SDL_Palette
{
	int ncolors;
	SDL_Color *colors;
	Uint32 version;
	int refcount;
};

struct SDL_PixelFormat
{
	Uint32 format;
	SDL_Palette *palette;
	Uint8 BitsPerPixel;
	Uint8 BytesPerPixel;
	Uint8 padding[2];
	Uint32 Rmask, Gmask, Bmask, Amask;
	Uint8 Rloss, Gloss, Bloss, Aloss;
	Uint8 Rshift, Gshift, Bshift, Ashift;
	int refcount;
	SDL_PixelFormat *next;
};

enum SDL_BlendMode
{
	SDL_BLENDMODE_NONE = 0x00000000,
	SDL_BLENDMODE_BLEND = 0x00000001,
	SDL_BLENDMODE_ADD = 0x00000002,
	SDL_BLENDMODE_MOD = 0x00000004
};

#define SDL_SWSURFACE 0
#define SDL_PREALLOC 0x00000001

struct SDL_Surface
{
	Uint32 flags;
	SDL_PixelFormat *format;
	int w, h;
	int pitch;
	void *pixels;
	void *userdata;
	int locked;

	// PS2 compat extras (not in real SDL).
	SDL_BlendMode blendMode;
	bool hasColorKey;
	Uint32 colorKey;
	int refcount;
};

struct SDL_RWops;

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixelFormat, Uint32 flags);
int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode);
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);
int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
#define SDL_BlitSurface SDL_UpperBlit
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *format, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a);
SDL_Surface *SDL_LoadBMP(const char *file);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);
const char *SDL_GetError();
int SDL_SetError(const char *fmt, ...);

// ------------------------------------------------------------------------------------------------
// Keyboard
// ------------------------------------------------------------------------------------------------
enum SDL_Scancode
{
	SDL_SCANCODE_UNKNOWN = 0,
	SDL_SCANCODE_A = 4, SDL_SCANCODE_B = 5, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7, SDL_SCANCODE_E = 8,
	SDL_SCANCODE_F = 9, SDL_SCANCODE_G = 10, SDL_SCANCODE_H = 11, SDL_SCANCODE_I = 12, SDL_SCANCODE_J = 13,
	SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15, SDL_SCANCODE_M = 16, SDL_SCANCODE_N = 17, SDL_SCANCODE_O = 18,
	SDL_SCANCODE_P = 19, SDL_SCANCODE_Q = 20, SDL_SCANCODE_R = 21, SDL_SCANCODE_S = 22, SDL_SCANCODE_T = 23,
	SDL_SCANCODE_U = 24, SDL_SCANCODE_V = 25, SDL_SCANCODE_W = 26, SDL_SCANCODE_X = 27, SDL_SCANCODE_Y = 28,
	SDL_SCANCODE_Z = 29,
	SDL_SCANCODE_1 = 30, SDL_SCANCODE_2 = 31, SDL_SCANCODE_3 = 32, SDL_SCANCODE_4 = 33, SDL_SCANCODE_5 = 34,
	SDL_SCANCODE_6 = 35, SDL_SCANCODE_7 = 36, SDL_SCANCODE_8 = 37, SDL_SCANCODE_9 = 38, SDL_SCANCODE_0 = 39,
	SDL_SCANCODE_RETURN = 40,
	SDL_SCANCODE_ESCAPE = 41,
	SDL_SCANCODE_BACKSPACE = 42,
	SDL_SCANCODE_TAB = 43,
	SDL_SCANCODE_SPACE = 44,
	SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2 = 59, SDL_SCANCODE_F3 = 60, SDL_SCANCODE_F4 = 61, SDL_SCANCODE_F5 = 62,
	SDL_SCANCODE_F6 = 63, SDL_SCANCODE_F7 = 64, SDL_SCANCODE_F8 = 65, SDL_SCANCODE_F9 = 66, SDL_SCANCODE_F10 = 67,
	SDL_SCANCODE_F11 = 68, SDL_SCANCODE_F12 = 69,
	SDL_SCANCODE_PRINTSCREEN = 70,
	SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
	SDL_SCANCODE_KP_ENTER = 88,
	SDL_SCANCODE_KP_BACKSPACE = 187,
	SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_LALT = 226, SDL_SCANCODE_LGUI = 227,
	SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT = 229, SDL_SCANCODE_RALT = 230, SDL_SCANCODE_RGUI = 231,
	SDL_NUM_SCANCODES = 512
};

using SDL_Keycode = Sint32;

#define SDLK_SCANCODE_MASK (1 << 30)
#define SDL_SCANCODE_TO_KEYCODE(X) (static_cast<SDL_Keycode>(X) | SDLK_SCANCODE_MASK)

enum SDL_KeyCode : SDL_Keycode
{
	SDLK_UNKNOWN = 0,
	SDLK_RETURN = '\r',
	SDLK_ESCAPE = '\x1B',
	SDLK_BACKSPACE = '\b',
	SDLK_TAB = '\t',
	SDLK_SPACE = ' ',
	SDLK_0 = '0', SDLK_1 = '1', SDLK_2 = '2', SDLK_3 = '3', SDLK_4 = '4',
	SDLK_5 = '5', SDLK_6 = '6', SDLK_7 = '7', SDLK_8 = '8', SDLK_9 = '9',
	SDLK_a = 'a', SDLK_b = 'b', SDLK_c = 'c', SDLK_d = 'd', SDLK_e = 'e', SDLK_f = 'f', SDLK_g = 'g',
	SDLK_h = 'h', SDLK_i = 'i', SDLK_j = 'j', SDLK_k = 'k', SDLK_l = 'l', SDLK_m = 'm', SDLK_n = 'n',
	SDLK_o = 'o', SDLK_p = 'p', SDLK_q = 'q', SDLK_r = 'r', SDLK_s = 's', SDLK_t = 't', SDLK_u = 'u',
	SDLK_v = 'v', SDLK_w = 'w', SDLK_x = 'x', SDLK_y = 'y', SDLK_z = 'z',
	SDLK_F1 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F1),
	SDLK_F2 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F2),
	SDLK_F3 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F3),
	SDLK_F4 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F4),
	SDLK_F5 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F5),
	SDLK_F6 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F6),
	SDLK_F7 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F7),
	SDLK_F8 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F8),
	SDLK_F9 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F9),
	SDLK_F10 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F10),
	SDLK_F11 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F11),
	SDLK_F12 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F12),
	SDLK_PRINTSCREEN = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_PRINTSCREEN),
	SDLK_RIGHT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RIGHT),
	SDLK_LEFT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LEFT),
	SDLK_DOWN = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_DOWN),
	SDLK_UP = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_UP),
	SDLK_KP_ENTER = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_ENTER),
	SDLK_KP_BACKSPACE = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_BACKSPACE),
	SDLK_LCTRL = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LCTRL),
	SDLK_LSHIFT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LSHIFT),
	SDLK_LALT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LALT)
};

enum SDL_Keymod
{
	KMOD_NONE = 0x0000,
	KMOD_LSHIFT = 0x0001,
	KMOD_RSHIFT = 0x0002,
	KMOD_LCTRL = 0x0040,
	KMOD_RCTRL = 0x0080,
	KMOD_LALT = 0x0100,
	KMOD_RALT = 0x0200,
	KMOD_LGUI = 0x0400,
	KMOD_RGUI = 0x0800,
	KMOD_NUM = 0x1000,
	KMOD_CAPS = 0x2000,
	KMOD_MODE = 0x4000,
	KMOD_SCROLL = 0x8000,
	KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
	KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
	KMOD_ALT = KMOD_LALT | KMOD_RALT
};

struct SDL_Keysym
{
	SDL_Scancode scancode;
	SDL_Keycode sym;
	Uint16 mod;
	Uint32 unused;
};

const Uint8 *SDL_GetKeyboardState(int *numkeys);
SDL_Keymod SDL_GetModState();
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key);
void SDL_StartTextInput();
void SDL_StopTextInput();
SDL_bool SDL_IsTextInputActive();

// ------------------------------------------------------------------------------------------------
// Mouse (virtual pointer driven by the DualShock 2, see Ps2Input)
// ------------------------------------------------------------------------------------------------
#define SDL_BUTTON(X) (1u << ((X) - 1))
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_RMASK SDL_BUTTON(SDL_BUTTON_RIGHT)

Uint32 SDL_GetMouseState(int *x, int *y);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(SDL_bool enabled);
int SDL_ShowCursor(int toggle);

// ------------------------------------------------------------------------------------------------
// Events
// ------------------------------------------------------------------------------------------------
enum SDL_EventType : Uint32
{
	SDL_FIRSTEVENT = 0,
	SDL_QUIT = 0x100,
	SDL_WINDOWEVENT = 0x200,
	SDL_KEYDOWN = 0x300,
	SDL_KEYUP,
	SDL_TEXTEDITING,
	SDL_TEXTINPUT,
	SDL_MOUSEMOTION = 0x400,
	SDL_MOUSEBUTTONDOWN,
	SDL_MOUSEBUTTONUP,
	SDL_MOUSEWHEEL,
	SDL_RENDER_TARGETS_RESET = 0x2000,
	SDL_RENDER_DEVICE_RESET,
	SDL_LASTEVENT = 0xFFFF
};

enum SDL_WindowEventID : Uint8
{
	SDL_WINDOWEVENT_NONE,
	SDL_WINDOWEVENT_SHOWN,
	SDL_WINDOWEVENT_HIDDEN,
	SDL_WINDOWEVENT_EXPOSED,
	SDL_WINDOWEVENT_MOVED,
	SDL_WINDOWEVENT_RESIZED,
	SDL_WINDOWEVENT_SIZE_CHANGED
};

#define SDL_PRESSED 1
#define SDL_RELEASED 0
#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32

struct SDL_CommonEvent { Uint32 type; Uint32 timestamp; };
struct SDL_QuitEvent { Uint32 type; Uint32 timestamp; };
struct SDL_WindowEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 event; Uint8 padding1, padding2, padding3; Sint32 data1; Sint32 data2; };
struct SDL_KeyboardEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 state; Uint8 repeat; Uint8 padding2, padding3; SDL_Keysym keysym; };
struct SDL_TextInputEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; char text[SDL_TEXTINPUTEVENT_TEXT_SIZE]; };
struct SDL_MouseMotionEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which; Uint32 state; Sint32 x, y, xrel, yrel; };
struct SDL_MouseButtonEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which; Uint8 button; Uint8 state; Uint8 clicks; Uint8 padding1; Sint32 x, y; };
struct SDL_MouseWheelEvent { Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which; Sint32 x, y; Uint32 direction; };

union SDL_Event
{
	Uint32 type;
	SDL_CommonEvent common;
	SDL_QuitEvent quit;
	SDL_WindowEvent window;
	SDL_KeyboardEvent key;
	SDL_TextInputEvent text;
	SDL_MouseMotionEvent motion;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
	Uint8 padding[56];
};

int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);
Uint32 SDL_GetTicks();

// ------------------------------------------------------------------------------------------------
// Misc
// ------------------------------------------------------------------------------------------------
struct SDL_Window;

#define SDL_WINDOW_FULLSCREEN 0x00000001
#define SDL_WINDOW_VULKAN 0x10000000

#define SDL_MESSAGEBOX_ERROR 0x00000010
#define SDL_MESSAGEBOX_WARNING 0x00000020
#define SDL_MESSAGEBOX_INFORMATION 0x00000040

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);
const char *SDL_GetPlatform();
char *SDL_getenv(const char *name);
char *SDL_strdup(const char *str);
void SDL_free(void *mem);
