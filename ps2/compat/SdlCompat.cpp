// Implementation of the PS2 SDL subset (see SDL.h). Surfaces are plain EE-memory images; events/keyboard/mouse
// come from the DualShock 2 backend; message boxes go to the PS2 fatal error screen.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

#include "SDL.h"

#include "ps2/input/Ps2Input.h"
#include "ps2/platform/Ps2Platform.h"

namespace
{
	char g_errorBuffer[256] = "";

	SDL_PixelFormat g_formatRGBA32;
	SDL_PixelFormat g_formatARGB8888;
	SDL_PixelFormat g_formatRGBA8888;
	SDL_PixelFormat g_formatBGRA8888;
	SDL_PixelFormat g_formatIndex8;
	SDL_PixelFormat g_formatBGR24;
	bool g_formatsInited = false;

	uint8_t MaskShift(Uint32 mask)
	{
		if (mask == 0)
		{
			return 0;
		}

		uint8_t shift = 0;
		while ((mask & 1u) == 0)
		{
			mask >>= 1;
			shift++;
		}

		return shift;
	}

	void InitFormat(SDL_PixelFormat &f, Uint32 format, int bpp, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
	{
		std::memset(&f, 0, sizeof(f));
		f.format = format;
		f.BitsPerPixel = static_cast<Uint8>(bpp);
		f.BytesPerPixel = static_cast<Uint8>((bpp + 7) / 8);
		f.Rmask = r; f.Gmask = g; f.Bmask = b; f.Amask = a;
		f.Rshift = MaskShift(r); f.Gshift = MaskShift(g); f.Bshift = MaskShift(b); f.Ashift = MaskShift(a);
		f.refcount = 1;
	}

	void InitFormats()
	{
		if (g_formatsInited)
		{
			return;
		}

		InitFormat(g_formatRGBA32, SDL_PIXELFORMAT_ABGR8888, 32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
		InitFormat(g_formatARGB8888, SDL_PIXELFORMAT_ARGB8888, 32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
		InitFormat(g_formatRGBA8888, SDL_PIXELFORMAT_RGBA8888, 32, 0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu);
		InitFormat(g_formatBGRA8888, SDL_PIXELFORMAT_BGRA8888, 32, 0x0000FF00u, 0x00FF0000u, 0xFF000000u, 0x000000FFu);
		InitFormat(g_formatIndex8, SDL_PIXELFORMAT_INDEX8, 8, 0, 0, 0, 0);
		InitFormat(g_formatBGR24, SDL_PIXELFORMAT_BGR24, 24, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0);
		g_formatsInited = true;
	}

	SDL_PixelFormat *GetFormat(Uint32 format)
	{
		InitFormats();
		switch (format)
		{
		case SDL_PIXELFORMAT_ABGR8888: return &g_formatRGBA32;
		case SDL_PIXELFORMAT_ARGB8888: return &g_formatARGB8888;
		case SDL_PIXELFORMAT_RGBA8888: return &g_formatRGBA8888;
		case SDL_PIXELFORMAT_BGRA8888: return &g_formatBGRA8888;
		case SDL_PIXELFORMAT_INDEX8: return &g_formatIndex8;
		case SDL_PIXELFORMAT_BGR24: return &g_formatBGR24;
		default: return nullptr;
		}
	}

	SDL_Surface *AllocSurface(int width, int height, SDL_PixelFormat *format, void *pixels, int pitch)
	{
		if ((format == nullptr) || (width < 0) || (height < 0))
		{
			SDL_SetError("Unsupported surface format/size.");
			return nullptr;
		}

		SDL_Surface *s = new (std::nothrow) SDL_Surface();
		if (s == nullptr)
		{
			SDL_SetError("Out of memory (surface header).");
			return nullptr;
		}

		std::memset(s, 0, sizeof(*s));
		s->format = format;
		s->w = width;
		s->h = height;
		s->refcount = 1;
		s->blendMode = (format->Amask != 0) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;

		if (pixels != nullptr)
		{
			s->pixels = pixels;
			s->pitch = pitch;
			s->flags = SDL_PREALLOC;
		}
		else
		{
			s->pitch = width * format->BytesPerPixel;
			const size_t bytes = static_cast<size_t>(s->pitch) * static_cast<size_t>(height);
			if (bytes > 0)
			{
				// 16-byte alignment keeps EE memcpy/DMA-friendly.
				s->pixels = memalign(16, bytes);
				if (s->pixels == nullptr)
				{
					delete s;
					std::snprintf(g_errorBuffer, sizeof(g_errorBuffer), "Out of memory (%dx%d surface).", width, height);
					return nullptr;
				}

				std::memset(s->pixels, 0, bytes);
			}
		}

		if (format->format == SDL_PIXELFORMAT_INDEX8)
		{
			SDL_Palette *palette = new (std::nothrow) SDL_Palette();
			SDL_Color *colors = new (std::nothrow) SDL_Color[256];
			if ((palette == nullptr) || (colors == nullptr))
			{
				delete palette;
				delete[] colors;
				SDL_FreeSurface(s);
				return nullptr;
			}

			std::memset(colors, 0, sizeof(SDL_Color) * 256);
			palette->ncolors = 256;
			palette->colors = colors;

			// Per-surface palette requires a per-surface format copy.
			SDL_PixelFormat *ownFormat = new (std::nothrow) SDL_PixelFormat(*format);
			if (ownFormat == nullptr)
			{
				delete[] colors;
				delete palette;
				SDL_FreeSurface(s);
				return nullptr;
			}

			ownFormat->palette = palette;
			ownFormat->refcount = -1; // Marks as owned.
			s->format = ownFormat;
		}

		return s;
	}

	inline Uint32 ReadPixel32(const SDL_Surface *s, int x, int y)
	{
		const uint8_t *row = static_cast<const uint8_t*>(s->pixels) + (y * s->pitch);
		return reinterpret_cast<const Uint32*>(row)[x];
	}

	inline void WritePixel32(SDL_Surface *s, int x, int y, Uint32 value)
	{
		uint8_t *row = static_cast<uint8_t*>(s->pixels) + (y * s->pitch);
		reinterpret_cast<Uint32*>(row)[x] = value;
	}

	void GetPixelRGBA(const SDL_Surface *s, int x, int y, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
	{
		const SDL_PixelFormat *f = s->format;
		const uint8_t *row = static_cast<const uint8_t*>(s->pixels) + (y * s->pitch);
		if (f->BytesPerPixel == 4)
		{
			SDL_GetRGBA(reinterpret_cast<const Uint32*>(row)[x], f, r, g, b, a);
		}
		else if (f->BytesPerPixel == 3)
		{
			const uint8_t *p = row + (x * 3);
			*b = p[0]; *g = p[1]; *r = p[2]; *a = 255; // BMP-style BGR.
		}
		else
		{
			const SDL_Color &c = f->palette->colors[row[x]];
			*r = c.r; *g = c.g; *b = c.b; *a = 255;
		}
	}

	bool ClipRects(const SDL_Surface *src, SDL_Rect &srcRect, const SDL_Surface *dst, SDL_Rect &dstRect)
	{
		// Clip source to source surface.
		if (srcRect.x < 0) { dstRect.x -= srcRect.x; srcRect.w += srcRect.x; srcRect.x = 0; }
		if (srcRect.y < 0) { dstRect.y -= srcRect.y; srcRect.h += srcRect.y; srcRect.y = 0; }
		srcRect.w = std::min(srcRect.w, src->w - srcRect.x);
		srcRect.h = std::min(srcRect.h, src->h - srcRect.y);

		// Clip destination to destination surface.
		if (dstRect.x < 0) { srcRect.x -= dstRect.x; srcRect.w += dstRect.x; dstRect.x = 0; }
		if (dstRect.y < 0) { srcRect.y -= dstRect.y; srcRect.h += dstRect.y; dstRect.y = 0; }
		srcRect.w = std::min(srcRect.w, dst->w - dstRect.x);
		srcRect.h = std::min(srcRect.h, dst->h - dstRect.y);

		dstRect.w = srcRect.w;
		dstRect.h = srcRect.h;
		return (srcRect.w > 0) && (srcRect.h > 0);
	}

	uint32_t ReadLE32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
	uint16_t ReadLE16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
	void WriteLE32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
	void WriteLE16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
}

// ------------------------------------------------------------------------------------------------
// Surfaces
// ------------------------------------------------------------------------------------------------
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
	InitFormats();
	if (depth == 8)
	{
		return AllocSurface(width, height, &g_formatIndex8, nullptr, 0);
	}

	if (depth == 32)
	{
		for (SDL_PixelFormat *f : { &g_formatRGBA32, &g_formatARGB8888, &g_formatRGBA8888, &g_formatBGRA8888 })
		{
			if ((f->Rmask == Rmask) && (f->Gmask == Gmask) && (f->Bmask == Bmask) && ((f->Amask == Amask) || (Amask == 0)))
			{
				return AllocSurface(width, height, f, nullptr, 0);
			}
		}

		if ((Rmask | Gmask | Bmask | Amask) == 0)
		{
			return AllocSurface(width, height, &g_formatARGB8888, nullptr, 0);
		}
	}

	SDL_SetError("SDL_CreateRGBSurface: unsupported depth/masks on PS2.");
	return nullptr;
}

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format)
{
	return AllocSurface(width, height, GetFormat(format), nullptr, 0);
}

SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 format)
{
	return AllocSurface(width, height, GetFormat(format), pixels, pitch);
}

void SDL_FreeSurface(SDL_Surface *surface)
{
	if (surface == nullptr)
	{
		return;
	}

	if ((surface->flags & SDL_PREALLOC) == 0)
	{
		std::free(surface->pixels);
	}

	if ((surface->format != nullptr) && (surface->format->refcount == -1))
	{
		if (surface->format->palette != nullptr)
		{
			delete[] surface->format->palette->colors;
			delete surface->format->palette;
		}

		delete surface->format;
	}

	delete surface;
}

SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixelFormat, Uint32 flags)
{
	if (src == nullptr)
	{
		return nullptr;
	}

	SDL_PixelFormat *dstFormat = GetFormat(pixelFormat);
	if ((dstFormat == nullptr) || (dstFormat->BytesPerPixel != 4))
	{
		SDL_SetError("SDL_ConvertSurfaceFormat: only 32-bit targets are supported on PS2.");
		return nullptr;
	}

	SDL_Surface *dst = AllocSurface(src->w, src->h, dstFormat, nullptr, 0);
	if (dst == nullptr)
	{
		return nullptr;
	}

	for (int y = 0; y < src->h; y++)
	{
		for (int x = 0; x < src->w; x++)
		{
			Uint8 r, g, b, a;
			GetPixelRGBA(src, x, y, &r, &g, &b, &a);
			WritePixel32(dst, x, y, SDL_MapRGBA(dstFormat, r, g, b, a));
		}
	}

	if (src->hasColorKey)
	{
		// Carry the color key over in the new format.
		Uint8 r, g, b, a;
		if (src->format->BytesPerPixel == 1)
		{
			const SDL_Color &c = src->format->palette->colors[src->colorKey & 0xFF];
			r = c.r; g = c.g; b = c.b; a = 255;
		}
		else
		{
			SDL_GetRGBA(src->colorKey, src->format, &r, &g, &b, &a);
		}

		SDL_SetColorKey(dst, SDL_TRUE, SDL_MapRGBA(dstFormat, r, g, b, a));
	}

	return dst;
}

int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode)
{
	if (surface == nullptr)
	{
		return -1;
	}

	surface->blendMode = blendMode;
	return 0;
}

int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key)
{
	if (surface == nullptr)
	{
		return -1;
	}

	surface->hasColorKey = flag != 0;
	surface->colorKey = key;
	return 0;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
	if ((dst == nullptr) || (dst->format->BytesPerPixel != 4))
	{
		return -1;
	}

	SDL_Rect r = (rect != nullptr) ? *rect : SDL_Rect{ 0, 0, dst->w, dst->h };
	const int x0 = std::max(r.x, 0);
	const int y0 = std::max(r.y, 0);
	const int x1 = std::min(r.x + r.w, dst->w);
	const int y1 = std::min(r.y + r.h, dst->h);
	for (int y = y0; y < y1; y++)
	{
		Uint32 *row = reinterpret_cast<Uint32*>(static_cast<uint8_t*>(dst->pixels) + (y * dst->pitch));
		std::fill(row + x0, row + x1, color);
	}

	return 0;
}

int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
	if ((src == nullptr) || (dst == nullptr) || (dst->format->BytesPerPixel != 4))
	{
		SDL_SetError("SDL_BlitSurface: unsupported surfaces.");
		return -1;
	}

	SDL_Rect s = (srcrect != nullptr) ? *srcrect : SDL_Rect{ 0, 0, src->w, src->h };
	SDL_Rect d = { (dstrect != nullptr) ? dstrect->x : 0, (dstrect != nullptr) ? dstrect->y : 0, s.w, s.h };
	if (!ClipRects(src, s, dst, d))
	{
		if (dstrect != nullptr) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	const bool sameFormat = (src->format->format == dst->format->format) && (src->format->BytesPerPixel == 4);
	const bool blend = (src->blendMode == SDL_BLENDMODE_BLEND) && (src->format->Amask != 0);
	const bool colorKey = src->hasColorKey;

	for (int y = 0; y < s.h; y++)
	{
		for (int x = 0; x < s.w; x++)
		{
			const int sx = s.x + x, sy = s.y + y;
			const int dx = d.x + x, dy = d.y + y;

			if (sameFormat && !blend && !colorKey)
			{
				WritePixel32(dst, dx, dy, ReadPixel32(src, sx, sy));
				continue;
			}

			if (colorKey)
			{
				const Uint32 raw = (src->format->BytesPerPixel == 4) ? ReadPixel32(src, sx, sy) :
					((src->format->BytesPerPixel == 1) ? static_cast<const uint8_t*>(src->pixels)[(sy * src->pitch) + sx] : 0xFFFFFFFFu);
				if (raw == src->colorKey)
				{
					continue;
				}
			}

			Uint8 r, g, b, a;
			GetPixelRGBA(src, sx, sy, &r, &g, &b, &a);
			if (blend)
			{
				if (a == 0)
				{
					continue;
				}

				if (a != 255)
				{
					Uint8 dr, dg, db, da;
					SDL_GetRGBA(ReadPixel32(dst, dx, dy), dst->format, &dr, &dg, &db, &da);
					const int ia = 255 - a;
					r = static_cast<Uint8>(((r * a) + (dr * ia)) / 255);
					g = static_cast<Uint8>(((g * a) + (dg * ia)) / 255);
					b = static_cast<Uint8>(((b * a) + (db * ia)) / 255);
					a = static_cast<Uint8>(a + ((da * ia) / 255));
				}
			}

			WritePixel32(dst, dx, dy, SDL_MapRGBA(dst->format, r, g, b, a));
		}
	}

	if (dstrect != nullptr)
	{
		*dstrect = d;
	}

	return 0;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
	return SDL_MapRGBA(format, r, g, b, 255);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return (static_cast<Uint32>(r) << format->Rshift) | (static_cast<Uint32>(g) << format->Gshift) |
		(static_cast<Uint32>(b) << format->Bshift) | ((format->Amask != 0) ? (static_cast<Uint32>(a) << format->Ashift) : 0u);
}

void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *format, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
	*r = static_cast<Uint8>((pixel & format->Rmask) >> format->Rshift);
	*g = static_cast<Uint8>((pixel & format->Gmask) >> format->Gshift);
	*b = static_cast<Uint8>((pixel & format->Bmask) >> format->Bshift);
	*a = (format->Amask != 0) ? static_cast<Uint8>((pixel & format->Amask) >> format->Ashift) : 255;
}

SDL_Surface *SDL_LoadBMP(const char *file)
{
	FILE *f = std::fopen(file, "rb");
	if (f == nullptr)
	{
		std::snprintf(g_errorBuffer, sizeof(g_errorBuffer), "Couldn't open \"%s\".", file);
		return nullptr;
	}

	uint8_t header[54];
	if (std::fread(header, 1, sizeof(header), f) != sizeof(header) || (header[0] != 'B') || (header[1] != 'M'))
	{
		std::fclose(f);
		SDL_SetError("Not a BMP file.");
		return nullptr;
	}

	const uint32_t dataOffset = ReadLE32(header + 10);
	const uint32_t infoSize = ReadLE32(header + 14);
	const int32_t width = static_cast<int32_t>(ReadLE32(header + 18));
	const int32_t heightSigned = static_cast<int32_t>(ReadLE32(header + 22));
	const uint16_t bpp = ReadLE16(header + 28);
	const uint32_t compression = ReadLE32(header + 30);
	const uint32_t paletteCountField = ReadLE32(header + 46);
	const bool bottomUp = heightSigned > 0;
	const int height = bottomUp ? heightSigned : -heightSigned;

	if ((compression != 0 && compression != 3) || ((bpp != 8) && (bpp != 24) && (bpp != 32)) || (width <= 0) || (height <= 0))
	{
		std::fclose(f);
		SDL_SetError("Unsupported BMP (compression/bit depth).");
		return nullptr;
	}

	SDL_Surface *surface = nullptr;
	if (bpp == 8)
	{
		surface = AllocSurface(width, height, GetFormat(SDL_PIXELFORMAT_INDEX8), nullptr, 0);
		if (surface != nullptr)
		{
			const int colorCount = (paletteCountField != 0) ? static_cast<int>(std::min<uint32_t>(paletteCountField, 256)) : 256;
			std::fseek(f, 14 + static_cast<long>(infoSize), SEEK_SET);
			for (int i = 0; i < colorCount; i++)
			{
				uint8_t bgra[4];
				if (std::fread(bgra, 1, 4, f) != 4)
				{
					break;
				}

				SDL_Color &c = surface->format->palette->colors[i];
				c.b = bgra[0]; c.g = bgra[1]; c.r = bgra[2]; c.a = 255;
			}
		}
	}
	else if (bpp == 24)
	{
		surface = AllocSurface(width, height, GetFormat(SDL_PIXELFORMAT_BGR24), nullptr, 0);
	}
	else
	{
		surface = AllocSurface(width, height, GetFormat(SDL_PIXELFORMAT_ARGB8888), nullptr, 0);
	}

	if (surface == nullptr)
	{
		std::fclose(f);
		return nullptr;
	}

	const int bytesPerPixel = bpp / 8;
	const int filePitch = ((width * bytesPerPixel) + 3) & ~3;
	uint8_t *rowBuffer = new (std::nothrow) uint8_t[filePitch];
	if (rowBuffer == nullptr)
	{
		std::fclose(f);
		SDL_FreeSurface(surface);
		return nullptr;
	}

	std::fseek(f, static_cast<long>(dataOffset), SEEK_SET);
	for (int row = 0; row < height; row++)
	{
		if (std::fread(rowBuffer, 1, filePitch, f) != static_cast<size_t>(filePitch))
		{
			break;
		}

		const int y = bottomUp ? (height - 1 - row) : row;
		std::memcpy(static_cast<uint8_t*>(surface->pixels) + (y * surface->pitch), rowBuffer, width * bytesPerPixel);
	}

	delete[] rowBuffer;
	std::fclose(f);
	return surface;
}

int SDL_SaveBMP(SDL_Surface *surface, const char *file)
{
	if ((surface == nullptr) || (surface->format->BytesPerPixel != 4))
	{
		SDL_SetError("SDL_SaveBMP: only 32-bit surfaces are supported on PS2.");
		return -1;
	}

	FILE *f = std::fopen(file, "wb");
	if (f == nullptr)
	{
		std::snprintf(g_errorBuffer, sizeof(g_errorBuffer), "Couldn't create \"%s\".", file);
		return -1;
	}

	const int w = surface->w, h = surface->h;
	const int rowBytes = w * 3;
	const int filePitch = (rowBytes + 3) & ~3;
	uint8_t header[54] = {};
	header[0] = 'B'; header[1] = 'M';
	WriteLE32(header + 2, 54 + (filePitch * h));
	WriteLE32(header + 10, 54);
	WriteLE32(header + 14, 40);
	WriteLE32(header + 18, static_cast<uint32_t>(w));
	WriteLE32(header + 22, static_cast<uint32_t>(h));
	WriteLE16(header + 26, 1);
	WriteLE16(header + 28, 24);
	std::fwrite(header, 1, sizeof(header), f);

	uint8_t *row = new (std::nothrow) uint8_t[filePitch];
	if (row == nullptr)
	{
		std::fclose(f);
		return -1;
	}

	std::memset(row, 0, filePitch);
	for (int y = h - 1; y >= 0; y--)
	{
		for (int x = 0; x < w; x++)
		{
			Uint8 r, g, b, a;
			SDL_GetRGBA(ReadPixel32(surface, x, y), surface->format, &r, &g, &b, &a);
			row[(x * 3) + 0] = b; row[(x * 3) + 1] = g; row[(x * 3) + 2] = r;
		}

		std::fwrite(row, 1, filePitch, f);
	}

	delete[] row;
	std::fclose(f);
	return 0;
}

const char *SDL_GetError()
{
	return g_errorBuffer;
}

int SDL_SetError(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(g_errorBuffer, sizeof(g_errorBuffer), fmt, args);
	va_end(args);
	return -1;
}

// ------------------------------------------------------------------------------------------------
// Keyboard / mouse / events (virtualized from the DualShock 2)
// ------------------------------------------------------------------------------------------------
const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
	if (numkeys != nullptr)
	{
		*numkeys = SDL_NUM_SCANCODES;
	}

	return Ps2Input::getKeyboardState();
}

SDL_Keymod SDL_GetModState()
{
	return static_cast<SDL_Keymod>(Ps2Input::getModState());
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key)
{
	if ((key & SDLK_SCANCODE_MASK) != 0)
	{
		return static_cast<SDL_Scancode>(key & ~SDLK_SCANCODE_MASK);
	}

	if ((key >= 'a') && (key <= 'z')) return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key - 'a'));
	if ((key >= '1') && (key <= '9')) return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key - '1'));

	switch (key)
	{
	case '0': return SDL_SCANCODE_0;
	case SDLK_RETURN: return SDL_SCANCODE_RETURN;
	case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
	case SDLK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
	case SDLK_TAB: return SDL_SCANCODE_TAB;
	case SDLK_SPACE: return SDL_SCANCODE_SPACE;
	default: return SDL_SCANCODE_UNKNOWN;
	}
}

void SDL_StartTextInput()
{
	Ps2Input::setTextInputActive(true);
}

void SDL_StopTextInput()
{
	Ps2Input::setTextInputActive(false);
}

SDL_bool SDL_IsTextInputActive()
{
	return Ps2Input::isTextInputActive() ? SDL_TRUE : SDL_FALSE;
}

Uint32 SDL_GetMouseState(int *x, int *y)
{
	const Int2 cursor = Ps2Input::getCursor();
	if (x != nullptr) *x = cursor.x;
	if (y != nullptr) *y = cursor.y;
	return Ps2Input::getMouseButtonMask();
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
	const Int2 delta = Ps2Input::consumeMouseDelta();
	if (x != nullptr) *x = delta.x;
	if (y != nullptr) *y = delta.y;
	return Ps2Input::getMouseButtonMask();
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
	Ps2Input::setRelativeMouseMode(enabled == SDL_TRUE);
	return 0;
}

int SDL_ShowCursor(int toggle)
{
	return toggle; // The engine draws its own cursor sprite.
}

int SDL_PollEvent(SDL_Event *event)
{
	return Ps2Input::popEvent(event) ? 1 : 0;
}

int SDL_PushEvent(SDL_Event *event)
{
	if (event == nullptr)
	{
		return -1;
	}

	Ps2Input::pushEvent(*event);
	return 1;
}

Uint32 SDL_GetTicks()
{
	return static_cast<Uint32>(Ps2Platform::getSeconds() * 1000.0);
}

// ------------------------------------------------------------------------------------------------
// Misc
// ------------------------------------------------------------------------------------------------
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
	if ((flags & SDL_MESSAGEBOX_ERROR) != 0)
	{
		Ps2Platform::showFatalError((title != nullptr) ? title : "Error", (message != nullptr) ? message : "");
	}
	else
	{
		std::printf("[PS2] %s: %s\n", (title != nullptr) ? title : "", (message != nullptr) ? message : "");
	}

	return 0;
}

const char *SDL_GetPlatform()
{
	return "PlayStation 2";
}

char *SDL_getenv(const char *name)
{
	return nullptr;
}

char *SDL_strdup(const char *str)
{
	return (str != nullptr) ? strdup(str) : nullptr;
}

void SDL_free(void *mem)
{
	std::free(mem);
}
