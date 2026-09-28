#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../Utilities/Palette.h"

#include "components/utilities/Buffer2D.h"

// An .FLC file is a video file. .CEL files are nearly identical to .FLCs, though with an extra chunk
// of header data which can probably be skipped. I'm fairly certain now after looking into it, that
// the Arena developers used Autodesk Animator to make these .FLC and .CEL animations.
//
// Some interesting trivia I found in some .FLC files:
// - END02.FLC was initially created on Friday, Oct. 15th, 1993, and last updated
//   on the Wednesday after that.
// - KING.FLC was initially created on Tuesday, Oct. 19th, 1993.
// - VISION.FLC was initially created a month before that, on Monday, Sept. 13th 1993.
//
// These websites have some information on the FLIC format:
// - http://www.compuphase.com/flic.htm
// - http://www.fileformat.info/format/fli/egff.htm
class FLCStream;

class FLCFile
{
	friend class FLCStream;
private:
	// One buffer for each frame. Each integer points into that frame's palette.
	std::vector<std::pair<int, Buffer2D<uint8_t>>> images;
	std::vector<Palette> palettes;
	double secondsPerFrame;
	int width;
	int height;

	// Reads a palette chunk and writes out the results to the reference parameter.
	static bool readPalette(const uint8_t *chunkData, Palette *dst);

	// Decodes a fullscreen FLC chunk by updating the initial frame indices and
	// returning a complete frame.
	Buffer2D<uint8_t> decodeFullFrame(const uint8_t *chunkData, int chunkSize,
		Buffer2D<uint8_t> &initialFrame);

	// Decodes a delta FLC chunk by partially updating the initial frame indices and
	// returning a complete frame.
	// In-place variants decode directly into the scratch frame (no allocation).
	void decodeFullFrameInPlace(const uint8_t *chunkData, int chunkSize, Buffer2D<uint8_t> &initialFrame);
	void decodeDeltaFrameInPlace(const uint8_t *chunkData, int chunkSize, Buffer2D<uint8_t> &initialFrame);

	Buffer2D<uint8_t> decodeDeltaFrame(const uint8_t *chunkData, int chunkSize,
		Buffer2D<uint8_t> &initialFrame);
public:
	bool init(const char *filename);

	int getFrameCount() const;
	double getSecondsPerFrame() const;
	int getWidth() const;
	int getHeight() const;

	// Gets the palette associated with the given frame index.
	const Palette &getFramePalette(int index) const;

	// Gets the pixel data for some frame.
	const uint8_t *getPixels(int index) const;
};

// Sequential .FLC/.CEL decoder that keeps only one frame in memory. Frames are read one at a time from the VFS
// stream, so memory use is independent of video length (Arena's cinematics are up to ~14 MB compressed and hundreds
// of full-screen frames). Used by cinematics instead of decoding every frame up front.
class FLCStream
{
private:
	FLCFile decoder; // Chunk decoders + dimensions only; its frame lists stay empty.
	std::shared_ptr<std::istream> stream;
	std::string filename;
	std::vector<uint8_t> frameBytes; // Reused frame read buffer.
	Buffer2D<uint8_t> pixels;
	Palette palette;
	int frameCount;
	int currentFrame;
	std::streamoff firstFrameOffset;

	bool readNextFrame();
public:
	FLCStream();

	// Reads the header and counts frames (header-only pass, no decoding).
	bool init(const char *filename);

	int getFrameCount() const;
	int getWidth() const;
	int getHeight() const;
	double getSecondsPerFrame() const;

	// Decodes forward to the given frame (restarting from the beginning if it's behind the current one).
	bool seekToFrame(int index);

	int getCurrentFrameIndex() const;
	const uint8_t *getPixels() const;
	const Palette &getPalette() const;
};
