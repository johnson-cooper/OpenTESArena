#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Sony SPU ADPCM ("VAG") encoding for SPU2 playback. 28 PCM samples -> one 16-byte block (~3.5:1 vs 16-bit PCM,
// ~1.75:1 vs Arena's 8-bit PCM), decoded in hardware by the SPU2 with free pitch conversion.
namespace Ps2Adpcm
{
	constexpr int SAMPLES_PER_BLOCK = 28;
	constexpr int BYTES_PER_BLOCK = 16;
	constexpr int VAG_HEADER_SIZE = 48;

	// Encodes unsigned 8-bit mono PCM into a complete VAG file image (header + blocks) for audsrv_load_adpcm().
	// The output is one-shot (the final block carries the end flag).
	void encodeU8ToVag(const uint8_t *samples, int sampleCount, int sampleRate, std::vector<uint8_t> *outVag);

	// Same for signed 16-bit mono PCM.
	void encodeS16ToVag(const int16_t *samples, int sampleCount, int sampleRate, std::vector<uint8_t> *outVag);
}
