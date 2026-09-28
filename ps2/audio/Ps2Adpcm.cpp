#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Ps2Adpcm.h"

namespace
{
	constexpr int K0[5] = { 0, 60, 115, 98, 122 };
	constexpr int K1[5] = { 0, 0, -52, -55, -60 };

	inline int Clamp16(int v)
	{
		return std::clamp(v, -32768, 32767);
	}

	void WriteBE32(uint8_t *p, uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v >> 24);
		p[1] = static_cast<uint8_t>(v >> 16);
		p[2] = static_cast<uint8_t>(v >> 8);
		p[3] = static_cast<uint8_t>(v);
	}

	// Integer-only encoder (the EE has no double-precision FPU). Picks the predictor with the smallest peak
	// residual, then quantizes with decoder feedback so rounding error doesn't accumulate.
	void EncodeBlock(const int *x, int count, int *hist1, int *hist2, uint8_t flags, uint8_t *out)
	{
		int bestFilter = 0;
		int bestMax = 0x7FFFFFFF;
		for (int f = 0; f < 5; f++)
		{
			int p1 = *hist1, p2 = *hist2;
			int maxAbs = 0;
			for (int i = 0; i < Ps2Adpcm::SAMPLES_PER_BLOCK; i++)
			{
				const int sample = (i < count) ? x[i] : 0;
				const int pred = ((p1 * K0[f]) + (p2 * K1[f]) + 32) >> 6;
				maxAbs = std::max(maxAbs, std::abs(sample - pred));
				p2 = p1;
				p1 = sample;
			}

			if (maxAbs < bestMax)
			{
				bestMax = maxAbs;
				bestFilter = f;
			}
		}

		int exponent = 0; // Residual step = 1 << exponent; shift = 12 - exponent.
		while ((exponent < 12) && (bestMax > (7 << exponent)))
		{
			exponent++;
		}

		const int shift = 12 - exponent;
		out[0] = static_cast<uint8_t>((shift & 0x0F) | (bestFilter << 4));
		out[1] = flags;
		std::memset(out + 2, 0, 14);

		int h1 = *hist1, h2 = *hist2;
		const int step = 1 << exponent;
		const int half = step >> 1;
		for (int i = 0; i < Ps2Adpcm::SAMPLES_PER_BLOCK; i++)
		{
			const int sample = (i < count) ? x[i] : 0;
			const int pred = ((h1 * K0[bestFilter]) + (h2 * K1[bestFilter]) + 32) >> 6;
			const int residual = sample - pred;
			int q = (residual >= 0) ? ((residual + half) >> exponent) : -(((-residual) + half) >> exponent);
			q = std::clamp(q, -8, 7);

			const int decoded = Clamp16((static_cast<int16_t>(static_cast<uint16_t>(q << 12)) >> shift) + pred);
			h2 = h1;
			h1 = decoded;

			const uint8_t nibble = static_cast<uint8_t>(q & 0x0F);
			uint8_t &byte = out[2 + (i >> 1)];
			byte |= ((i & 1) == 0) ? nibble : static_cast<uint8_t>(nibble << 4);
		}

		*hist1 = h1;
		*hist2 = h2;
	}

	template<typename GetSample>
	void Encode(int sampleCount, int sampleRate, GetSample getSample, std::vector<uint8_t> *outVag)
	{
		const int blockCount = std::max(1, (sampleCount + Ps2Adpcm::SAMPLES_PER_BLOCK - 1) / Ps2Adpcm::SAMPLES_PER_BLOCK);
		const int dataSize = blockCount * Ps2Adpcm::BYTES_PER_BLOCK;
		outVag->assign(Ps2Adpcm::VAG_HEADER_SIZE + dataSize, 0);

		uint8_t *header = outVag->data();
		std::memcpy(header, "VAGp", 4);
		WriteBE32(header + 4, 0x20);
		WriteBE32(header + 12, static_cast<uint32_t>(dataSize));
		WriteBE32(header + 16, static_cast<uint32_t>(sampleRate));
		std::memcpy(header + 32, "OpenTESArena", 12);

		int hist1 = 0, hist2 = 0;
		int blockSamples[Ps2Adpcm::SAMPLES_PER_BLOCK];
		for (int block = 0; block < blockCount; block++)
		{
			const int first = block * Ps2Adpcm::SAMPLES_PER_BLOCK;
			const int count = std::min(Ps2Adpcm::SAMPLES_PER_BLOCK, sampleCount - first);
			for (int i = 0; i < count; i++)
			{
				blockSamples[i] = getSample(first + i);
			}

			const bool isLast = block == (blockCount - 1);
			const uint8_t flags = isLast ? 0x01 : 0x00; // 0x01 = end (voice stops, no loop).
			EncodeBlock(blockSamples, std::max(count, 0), &hist1, &hist2, flags,
				outVag->data() + Ps2Adpcm::VAG_HEADER_SIZE + (block * Ps2Adpcm::BYTES_PER_BLOCK));
		}
	}
}

void Ps2Adpcm::encodeU8ToVag(const uint8_t *samples, int sampleCount, int sampleRate, std::vector<uint8_t> *outVag)
{
	Encode(sampleCount, sampleRate, [samples](int i) { return (static_cast<int>(samples[i]) - 128) << 8; }, outVag);
}

void Ps2Adpcm::encodeS16ToVag(const int16_t *samples, int sampleCount, int sampleRate, std::vector<uint8_t> *outVag)
{
	Encode(sampleCount, sampleRate, [samples](int i) { return static_cast<int>(samples[i]); }, outVag);
}
