#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>

#include <kernel.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>

#include "Ps2GsRenderBackend.h"
#include "ps2/platform/Ps2Memory.h"
#include "ps2/platform/Ps2Platform.h"
#include "ps2/platform/Ps2Video.h"

#include "OpenTESArena/src/Math/Matrix4.h"
#include "OpenTESArena/src/Math/Rect.h"
#include "OpenTESArena/src/Rendering/Renderer.h"
#include "OpenTESArena/src/Rendering/RenderBuffer.h"
#include "OpenTESArena/src/Rendering/RenderCamera.h"
#include "OpenTESArena/src/Rendering/RenderDrawCall.h"
#include "OpenTESArena/src/Rendering/RenderDrawCommand.h"
#include "OpenTESArena/src/Rendering/RenderFrameSettings.h"
#include "OpenTESArena/src/Rendering/RenderInitSettings.h"
#include "OpenTESArena/src/Rendering/RenderShaderUtils.h"
#include "OpenTESArena/src/Rendering/Window.h"
#include "OpenTESArena/src/UI/Surface.h"
#include "OpenTESArena/src/UI/UiDrawCommand.h"
#include "OpenTESArena/src/Utilities/Color.h"

#include "components/debug/Debug.h"

namespace
{
	constexpr uint32_t GS_VRAM_BYTES = 4u * 1024u * 1024u;
	constexpr uint32_t GS_PAGE_BYTES = 8192;
	constexpr uint32_t CLUT_BYTES = 1024; // 16x16 CT32 = 4 GS blocks.

	// Output buffers: 16-bit color + 16-bit Z keeps VRAM for textures (see Ps2Video.h).
	constexpr int FRAME_PSM = GS_PSM_CT16S;
	constexpr int Z_PSM = GS_PSMZ_16S;

	// Z = ZK / w, clamped. 1/w is affine in screen space so the GS interpolates it exactly.
	constexpr float Z_NEAR_FOR_DEPTH = 0.2f;
	constexpr float Z_MAX = 65535.0f;
	constexpr float ZK = Z_MAX * Z_NEAR_FOR_DEPTH;

	// Guard band in NDC units: GS window coordinates are 0..4095 with the draw area centered at 2048.
	constexpr float GUARD_BAND = 4.0f;

	// Per-frame scratch budgets (bounded, never exceeded).
	constexpr uint32_t MAX_FRAME_VERTICES = 24 * 1024;
	constexpr uint32_t MAX_FRAME_BATCHES = 4096;

	// Batch state flags.
	enum : uint16_t
	{
		STATE_ALPHA_TEST = 1 << 0,
		STATE_BLEND = 1 << 1,
		STATE_DEPTH_READ = 1 << 2,
		STATE_DEPTH_WRITE = 1 << 3,
		STATE_REPEAT_U = 1 << 4,
		STATE_REPEAT_V = 1 << 5
	};

	// REGLIST register order for textured triangles: RGBAQ (with Q), ST, XYZ2 (kick).
	constexpr uint64_t REGS_TRI = 0x1 | (0x2 << 4) | (0x5 << 8);
	// Sprites: RGBAQ, UV, XYZ2, UV, XYZ2.
	constexpr uint64_t REGS_SPRITE = 0x1 | (0x3 << 4) | (0x5 << 8) | (0x3 << 12) | (0x5 << 16);

	constexpr uint64_t GifTag(uint64_t nloop, uint64_t eop, uint64_t pre, uint64_t prim, uint64_t flg, uint64_t nreg)
	{
		return (nloop & 0x7FFF) | (eop << 15) | (pre << 46) | ((prim & 0x7FF) << 47) | (flg << 58) | (nreg << 60);
	}

	constexpr uint64_t FLG_PACKED = 0;
	constexpr uint64_t FLG_REGLIST = 1;

	constexpr uint64_t Prim(uint64_t type, uint64_t iip, uint64_t tme, uint64_t fge, uint64_t abe, uint64_t fst)
	{
		return type | (iip << 3) | (tme << 4) | (fge << 5) | (abe << 6) | (fst << 8);
	}

	constexpr uint64_t PRIM_TRIANGLE = 3;
	constexpr uint64_t PRIM_SPRITE = 6;

	inline uint32_t FloatBits(float f)
	{
		uint32_t u;
		std::memcpy(&u, &f, sizeof(u));
		return u;
	}

	inline int Log2Ceil(int v)
	{
		int l = 0;
		while ((1 << l) < v)
		{
			l++;
		}

		return l;
	}

	// CLUT entry order for CSM1: entries 8-15 and 16-23 swap within each group of 32.
	inline int ClutCsm1Index(int i)
	{
		return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
	}

	struct ClipVertex
	{
		float x, y, z, w; // Clip space.
		float u, v;
		float light;      // 0..1 intensity for Gouraud modulation.
	};

	constexpr int MAX_CLIP_VERTICES = 16;

	// Clips a convex polygon against dot(plane, v) >= 0 where plane = (a, b, c, d) over (x, y, z, w).
	int ClipPolygon(const ClipVertex *in, int inCount, ClipVertex *out, float a, float b, float c, float d)
	{
		int outCount = 0;
		for (int i = 0; i < inCount; i++)
		{
			const ClipVertex &p0 = in[i];
			const ClipVertex &p1 = in[(i + 1) % inCount];
			const float d0 = (a * p0.x) + (b * p0.y) + (c * p0.z) + (d * p0.w);
			const float d1 = (a * p1.x) + (b * p1.y) + (c * p1.z) + (d * p1.w);
			if (d0 >= 0.0f)
			{
				out[outCount++] = p0;
			}

			if ((d0 >= 0.0f) != (d1 >= 0.0f))
			{
				const float t = d0 / (d0 - d1);
				ClipVertex &r = out[outCount++];
				r.x = p0.x + ((p1.x - p0.x) * t);
				r.y = p0.y + ((p1.y - p0.y) * t);
				r.z = p0.z + ((p1.z - p0.z) * t);
				r.w = p0.w + ((p1.w - p0.w) * t);
				r.u = p0.u + ((p1.u - p0.u) * t);
				r.v = p0.v + ((p1.v - p0.v) * t);
				r.light = p0.light + ((p1.light - p0.light) * t);
			}

			if (outCount >= (MAX_CLIP_VERTICES - 1))
			{
				break;
			}
		}

		return outCount;
	}

	struct FrameLight
	{
		float x, y, z;
		float startSqr, endSqr, start, radiusDiffRecip;
	};

	constexpr int MAX_FRAME_LIGHTS = 64;
}

// -------------------------------------------------------------------------------------------------
// Lifetime
// -------------------------------------------------------------------------------------------------
Ps2GsRenderBackend::Ps2GsRenderBackend()
{
	this->window = nullptr;
	this->gs = nullptr;
	this->vramPoolStart = 0;
	this->vramPoolPages = 0;
	this->vramUsedPages = 0;
	this->vramPeakPages = 0;
	this->frameIndex = 1;
	this->vertexBudgetExceeded = false;
	this->statDrawCalls = this->statTriangles = this->statBatches = this->statTextureUploads = 0;
	this->statUploadBytes = 0;
	this->objectTextureBytes = 0;
	this->uiTextureBytes = 0;
}

Ps2GsRenderBackend::~Ps2GsRenderBackend()
{
	this->shutdown();
}

bool Ps2GsRenderBackend::initContext(const RenderContextSettings &contextSettings)
{
	this->window = contextSettings.window;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	// One-shot queue: 512 KiB per buffer (x2, double-buffered) holds ~21k REGLIST vertices per frame.
	constexpr int osQueueBytes = 512 * 1024;
	constexpr int perQueueBytes = 16 * 1024;
	this->gs = gsKit_init_global_custom(osQueueBytes, perQueueBytes);
	if (this->gs == nullptr)
	{
		DebugLogError("gsKit_init_global_custom() failed (out of EE memory for render queues?).");
		return false;
	}

	GSGLOBAL *gs = this->gs;
	gs->Mode = Ps2Video::isPal() ? GS_MODE_PAL : GS_MODE_NTSC;
	gs->Interlace = GS_INTERLACED;
	gs->Field = GS_FIELD;
	gs->Width = 640;
	gs->Height = Ps2Video::isPal() ? 512 : 448;
	gs->PSM = FRAME_PSM;
	gs->PSMZ = Z_PSM;
	gs->ZBuffering = GS_SETTING_ON;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->PrimAlphaEnable = GS_SETTING_OFF;
	gs->PrimAAEnable = GS_SETTING_OFF;
	gs->Dithering = GS_SETTING_ON; // 16-bit output: dither to hide banding in Arena's gradients.

	gsKit_init_screen(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);
	gsKit_set_dither(gs);

	DebugLogFormat("GS initialized: %s, framebuffer %dx%d, PSM 0x%x, PSMZ 0x%x, screen buffers 0x%x/0x%x, Z 0x%x, vram cursor 0x%x.",
		Ps2Video::getModeName(), gs->Width, gs->Height, gs->PSM, gs->PSMZ, gs->ScreenBuffer[0], gs->ScreenBuffer[1], gs->ZBuffer, gs->CurrentPointer);

	return true;
}

bool Ps2GsRenderBackend::initRendering(const RenderInitSettings &initSettings)
{
	GSGLOBAL *gs = this->gs;

	// Everything after gsKit's framebuffers/Z belongs to the texture cache. CLUTs get a dedicated region first.
	const uint32_t poolStart = (static_cast<uint32_t>(gs->CurrentPointer) + (GS_PAGE_BYTES - 1)) & ~(GS_PAGE_BYTES - 1);
	const uint32_t clutRegionBytes = ((CLUT_SLOT_COUNT * CLUT_BYTES) + (GS_PAGE_BYTES - 1)) & ~(GS_PAGE_BYTES - 1);

	this->clutSlots = std::make_unique<ClutSlot[]>(CLUT_SLOT_COUNT);
	for (int i = 0; i < CLUT_SLOT_COUNT; i++)
	{
		this->clutSlots[i].byteAddress = poolStart + (static_cast<uint32_t>(i) * CLUT_BYTES);
	}

	this->vramPoolStart = poolStart + clutRegionBytes;
	if (this->vramPoolStart >= GS_VRAM_BYTES)
	{
		DebugLogErrorFormat("No GS VRAM left for textures (pool start 0x%x).", this->vramPoolStart);
		return false;
	}

	this->vramPoolPages = (GS_VRAM_BYTES - this->vramPoolStart) / GS_PAGE_BYTES;
	this->vramPageUsed.assign(this->vramPoolPages, 0);
	this->vramSlots.clear();
	this->vramSlots.reserve(512);

	this->frameVertices.reserve(MAX_FRAME_VERTICES);
	this->frameBatches.reserve(MAX_FRAME_BATCHES);

	DebugLogFormat("GS VRAM: framebuffers+Z %u KiB, CLUT slots %d (%u KiB), texture pool %u KiB (%u pages).",
		poolStart / 1024, CLUT_SLOT_COUNT, clutRegionBytes / 1024, (this->vramPoolPages * GS_PAGE_BYTES) / 1024, this->vramPoolPages);

	static_cast<void>(initSettings);
	return true;
}

void Ps2GsRenderBackend::shutdown()
{
	if (this->gs != nullptr)
	{
		dmaKit_wait(DMA_CHANNEL_GIF, 0);
		gsKit_deinit_global(this->gs);
		this->gs = nullptr;
	}

	this->window = nullptr;
}

void Ps2GsRenderBackend::resize(int windowWidth, int windowHeight, int sceneViewWidth, int sceneViewHeight, int internalWidth, int internalHeight)
{
	// Fixed TV output; the scene view rectangle is re-read from the Window every frame.
}

void Ps2GsRenderBackend::handleRenderTargetsReset(int windowWidth, int windowHeight, int sceneViewWidth, int sceneViewHeight, int internalWidth, int internalHeight)
{
}

RendererProfilerData2D Ps2GsRenderBackend::getProfilerData2D() const
{
	return this->profiler2D;
}

RendererProfilerData3D Ps2GsRenderBackend::getProfilerData3D() const
{
	return this->profiler3D;
}

Surface Ps2GsRenderBackend::getScreenshot() const
{
	// Reading back GS local memory is possible (GS->EE transfer) but not wired yet; return a black image.
	DebugLogWarning("Screenshots are not supported on PS2 yet.");
	const Int2 dims = this->window->getPixelDimensions();
	Surface surface = Surface::createWithFormat(dims.x, dims.y, 32, SDL_PIXELFORMAT_RGBA32);
	surface.fill(0, 0, 0, 255);
	return surface;
}

int Ps2GsRenderBackend::getBytesPerFloat() const
{
	return sizeof(float); // EE FPU is single precision only.
}

// -------------------------------------------------------------------------------------------------
// Buffers
// -------------------------------------------------------------------------------------------------
VertexPositionBufferID Ps2GsRenderBackend::createVertexPositionBuffer(int vertexCount, int componentsPerVertex, int bytesPerComponent)
{
	DebugAssert(bytesPerComponent == sizeof(float));
	const VertexPositionBufferID id = this->positionBuffers.alloc();
	VertexBuffer &buffer = this->positionBuffers.get(id);
	buffer.vertexCount = vertexCount;
	buffer.componentsPerVertex = componentsPerVertex;
	buffer.data.assign(static_cast<size_t>(vertexCount) * componentsPerVertex, 0.0f);
	return id;
}

void Ps2GsRenderBackend::freeVertexPositionBuffer(VertexPositionBufferID id)
{
	this->positionBuffers.free(id);
}

LockedBuffer Ps2GsRenderBackend::lockVertexPositionBuffer(VertexPositionBufferID id)
{
	VertexBuffer &buffer = this->positionBuffers.get(id);
	Span<std::byte> bytes(reinterpret_cast<std::byte*>(buffer.data.data()), static_cast<int>(buffer.data.size() * sizeof(float)));
	return LockedBuffer(bytes, static_cast<int>(buffer.data.size()), sizeof(float), sizeof(float));
}

void Ps2GsRenderBackend::unlockVertexPositionBuffer(VertexPositionBufferID id)
{
}

VertexAttributeBufferID Ps2GsRenderBackend::createVertexAttributeBuffer(int vertexCount, int componentsPerVertex, int bytesPerComponent)
{
	DebugAssert(bytesPerComponent == sizeof(float));
	const VertexAttributeBufferID id = this->attributeBuffers.alloc();
	VertexBuffer &buffer = this->attributeBuffers.get(id);
	buffer.vertexCount = vertexCount;
	buffer.componentsPerVertex = componentsPerVertex;
	buffer.data.assign(static_cast<size_t>(vertexCount) * componentsPerVertex, 0.0f);
	return id;
}

void Ps2GsRenderBackend::freeVertexAttributeBuffer(VertexAttributeBufferID id)
{
	this->attributeBuffers.free(id);
}

LockedBuffer Ps2GsRenderBackend::lockVertexAttributeBuffer(VertexAttributeBufferID id)
{
	VertexBuffer &buffer = this->attributeBuffers.get(id);
	Span<std::byte> bytes(reinterpret_cast<std::byte*>(buffer.data.data()), static_cast<int>(buffer.data.size() * sizeof(float)));
	return LockedBuffer(bytes, static_cast<int>(buffer.data.size()), sizeof(float), sizeof(float));
}

void Ps2GsRenderBackend::unlockVertexAttributeBuffer(VertexAttributeBufferID id)
{
}

IndexBufferID Ps2GsRenderBackend::createIndexBuffer(int indexCount, int bytesPerIndex)
{
	DebugAssert(bytesPerIndex == sizeof(int32_t));
	const IndexBufferID id = this->indexBuffers.alloc();
	this->indexBuffers.get(id).indices.assign(indexCount, 0);
	return id;
}

void Ps2GsRenderBackend::freeIndexBuffer(IndexBufferID id)
{
	this->indexBuffers.free(id);
}

LockedBuffer Ps2GsRenderBackend::lockIndexBuffer(IndexBufferID id)
{
	IndexBuffer &buffer = this->indexBuffers.get(id);
	Span<std::byte> bytes(reinterpret_cast<std::byte*>(buffer.indices.data()), static_cast<int>(buffer.indices.size() * sizeof(int32_t)));
	return LockedBuffer(bytes, static_cast<int>(buffer.indices.size()), sizeof(int32_t), sizeof(int32_t));
}

void Ps2GsRenderBackend::unlockIndexBuffer(IndexBufferID id)
{
}

UniformBufferID Ps2GsRenderBackend::createUniformBuffer(int elementCount, int bytesPerElement, int alignmentOfElement)
{
	const UniformBufferID id = this->uniformBuffers.alloc();
	UniformBuffer &buffer = this->uniformBuffers.get(id);
	const int alignment = std::max(alignmentOfElement, 1);
	buffer.elementCount = elementCount;
	buffer.bytesPerElement = bytesPerElement;
	buffer.bytesPerStride = ((bytesPerElement + alignment - 1) / alignment) * alignment;
	buffer.bytes.assign(static_cast<size_t>(elementCount) * buffer.bytesPerStride, std::byte(0));
	return id;
}

void Ps2GsRenderBackend::freeUniformBuffer(UniformBufferID id)
{
	this->uniformBuffers.free(id);
}

LockedBuffer Ps2GsRenderBackend::lockUniformBuffer(UniformBufferID id)
{
	UniformBuffer &buffer = this->uniformBuffers.get(id);
	Span<std::byte> bytes(buffer.bytes.data(), static_cast<int>(buffer.bytes.size()));
	return LockedBuffer(bytes, buffer.elementCount, buffer.bytesPerElement, buffer.bytesPerStride);
}

LockedBuffer Ps2GsRenderBackend::lockUniformBufferIndex(UniformBufferID id, int index)
{
	UniformBuffer &buffer = this->uniformBuffers.get(id);
	DebugAssert((index >= 0) && (index < buffer.elementCount));
	Span<std::byte> bytes(buffer.bytes.data() + (static_cast<size_t>(index) * buffer.bytesPerStride), buffer.bytesPerElement);
	return LockedBuffer(bytes, 1, buffer.bytesPerElement, buffer.bytesPerStride);
}

void Ps2GsRenderBackend::unlockUniformBuffer(UniformBufferID id)
{
}

void Ps2GsRenderBackend::unlockUniformBufferIndex(UniformBufferID id, int index)
{
}

// -------------------------------------------------------------------------------------------------
// Textures
// -------------------------------------------------------------------------------------------------
namespace
{
	void InitTexture(Ps2GsRenderBackend::Texture &texture, int width, int height, int bytesPerTexel, bool isUi)
	{
		texture.width = width;
		texture.height = height;
		texture.bytesPerTexel = bytesPerTexel;
		texture.isUi = isUi;
		texture.texelBytes = static_cast<size_t>(width) * height * bytesPerTexel;
		texture.texels.reset(static_cast<uint8_t*>(memalign(64, (texture.texelBytes + 63) & ~static_cast<size_t>(63))));
		if (texture.texels != nullptr)
		{
			std::memset(texture.texels.get(), 0, texture.texelBytes);
		}

		texture.log2W = std::min(Log2Ceil(width), 10);
		texture.log2H = std::min(Log2Ceil(height), 10);

		// TBW in 64-texel units; PSMT8 needs an even TBW (128-texel granularity).
		if (bytesPerTexel == 1)
		{
			texture.tbw = std::max(2, ((width + 127) / 128) * 2);
		}
		else
		{
			texture.tbw = std::max(1, (width + 63) / 64);
		}
	}
}

Ps2GsRenderBackend::Texture *Ps2GsRenderBackend::getTexture(bool isUi, int id)
{
	return isUi ? this->uiTextures.tryGet(id) : this->objectTextures.tryGet(id);
}

ObjectTextureID Ps2GsRenderBackend::createObjectTexture(int width, int height, int bytesPerTexel)
{
	if ((width <= 0) || (height <= 0) || ((bytesPerTexel != 1) && (bytesPerTexel != 4)) || (width > 1024) || (height > 1024))
	{
		DebugLogErrorFormat("Unsupported object texture %dx%d (%d bytes/texel) on PS2.", width, height, bytesPerTexel);
		return -1;
	}

	const ObjectTextureID id = this->objectTextures.alloc();
	Texture &texture = this->objectTextures.get(id);
	InitTexture(texture, width, height, bytesPerTexel, false);
	if (texture.texels == nullptr)
	{
		DebugLogErrorFormat("Out of EE memory for %dx%d object texture.", width, height);
		this->objectTextures.free(id);
		return -1;
	}

	this->objectTextureBytes += static_cast<int64_t>(texture.texelBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::ObjectTextureBytes, this->objectTextureBytes);
	return id;
}

void Ps2GsRenderBackend::freeObjectTexture(ObjectTextureID id)
{
	Texture *texture = this->objectTextures.tryGet(id);
	if (texture == nullptr)
	{
		return;
	}

	if (texture->vramSlot >= 0)
	{
		this->freeVramSlot(texture->vramSlot);
	}

	this->objectTextureBytes -= static_cast<int64_t>(texture->texelBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::ObjectTextureBytes, this->objectTextureBytes);
	this->objectTextures.free(id);

	// A slot's owner is identified by ID; freeing moved another texture in the pool but IDs are stable.
}

std::optional<Int2> Ps2GsRenderBackend::tryGetObjectTextureDims(ObjectTextureID id) const
{
	const Texture *texture = this->objectTextures.tryGet(id);
	if (texture == nullptr)
	{
		return std::nullopt;
	}

	return Int2(texture->width, texture->height);
}

LockedTexture Ps2GsRenderBackend::lockObjectTexture(ObjectTextureID id)
{
	Texture &texture = this->objectTextures.get(id);
	this->convertTexelsToEngineAlpha(texture);
	Span<std::byte> texels(reinterpret_cast<std::byte*>(texture.texels.get()), static_cast<int>(texture.texelBytes));
	return LockedTexture(texels, texture.width, texture.height, texture.bytesPerTexel);
}

void Ps2GsRenderBackend::unlockObjectTexture(ObjectTextureID id)
{
	Texture &texture = this->objectTextures.get(id);
	texture.version++;
}

UiTextureID Ps2GsRenderBackend::createUiTexture(int width, int height)
{
	if ((width <= 0) || (height <= 0) || (width > 1024) || (height > 1024))
	{
		DebugLogErrorFormat("Unsupported UI texture %dx%d on PS2.", width, height);
		return -1;
	}

	const UiTextureID id = this->uiTextures.alloc();
	Texture &texture = this->uiTextures.get(id);
	InitTexture(texture, width, height, 4, true);
	if (texture.texels == nullptr)
	{
		DebugLogErrorFormat("Out of EE memory for %dx%d UI texture.", width, height);
		this->uiTextures.free(id);
		return -1;
	}

	this->uiTextureBytes += static_cast<int64_t>(texture.texelBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::UiTextureBytes, this->uiTextureBytes);
	return id;
}

void Ps2GsRenderBackend::freeUiTexture(UiTextureID id)
{
	Texture *texture = this->uiTextures.tryGet(id);
	if (texture == nullptr)
	{
		return;
	}

	if (texture->vramSlot >= 0)
	{
		this->freeVramSlot(texture->vramSlot);
	}

	this->uiTextureBytes -= static_cast<int64_t>(texture->texelBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::UiTextureBytes, this->uiTextureBytes);
	this->uiTextures.free(id);
}

std::optional<Int2> Ps2GsRenderBackend::tryGetUiTextureDims(UiTextureID id) const
{
	const Texture *texture = this->uiTextures.tryGet(id);
	if (texture == nullptr)
	{
		return std::nullopt;
	}

	return Int2(texture->width, texture->height);
}

LockedTexture Ps2GsRenderBackend::lockUiTexture(UiTextureID id)
{
	Texture &texture = this->uiTextures.get(id);
	this->convertTexelsToEngineAlpha(texture);
	Span<std::byte> texels(reinterpret_cast<std::byte*>(texture.texels.get()), static_cast<int>(texture.texelBytes));
	return LockedTexture(texels, texture.width, texture.height, 4);
}

void Ps2GsRenderBackend::unlockUiTexture(UiTextureID id)
{
	Texture &texture = this->uiTextures.get(id);
	texture.version++;
}

// 32-bit textures keep one EE copy. GS alpha is 0..0x80, the engine's is 0..0xFF; convert in place around locks
// rather than holding a second staging copy.
void Ps2GsRenderBackend::convertTexelsToGsAlpha(Texture &texture)
{
	if ((texture.bytesPerTexel != 4) || texture.gsAlpha)
	{
		return;
	}

	uint32_t *texels = reinterpret_cast<uint32_t*>(texture.texels.get());
	const size_t count = texture.texelBytes / 4;
	for (size_t i = 0; i < count; i++)
	{
		const uint32_t a = texels[i] >> 24;
		texels[i] = (texels[i] & 0x00FFFFFFu) | (((a + 1) >> 1) << 24);
	}

	texture.gsAlpha = true;
}

void Ps2GsRenderBackend::convertTexelsToEngineAlpha(Texture &texture)
{
	if ((texture.bytesPerTexel != 4) || !texture.gsAlpha)
	{
		return;
	}

	uint32_t *texels = reinterpret_cast<uint32_t*>(texture.texels.get());
	const size_t count = texture.texelBytes / 4;
	for (size_t i = 0; i < count; i++)
	{
		const uint32_t a = texels[i] >> 24;
		texels[i] = (texels[i] & 0x00FFFFFFu) | (std::min<uint32_t>(a * 2, 255) << 24);
	}

	texture.gsAlpha = false;
}

// -------------------------------------------------------------------------------------------------
// VRAM residency
// -------------------------------------------------------------------------------------------------
bool Ps2GsRenderBackend::allocVram(uint32_t bytes, bool isUi, int ownerID, int *outSlot)
{
	const uint32_t pages = std::max<uint32_t>(1, (bytes + GS_PAGE_BYTES - 1) / GS_PAGE_BYTES);
	if (pages > this->vramPoolPages)
	{
		return false;
	}

	for (int attempt = 0; attempt < 512; attempt++)
	{
		// First fit over the page map.
		uint32_t run = 0;
		for (uint32_t page = 0; page < this->vramPoolPages; page++)
		{
			run = (this->vramPageUsed[page] == 0) ? (run + 1) : 0;
			if (run == pages)
			{
				const uint32_t firstPage = page + 1 - pages;
				for (uint32_t p = firstPage; p <= page; p++)
				{
					this->vramPageUsed[p] = 1;
				}

				int slotIndex = -1;
				for (size_t i = 0; i < this->vramSlots.size(); i++)
				{
					if (!this->vramSlots[i].inUse)
					{
						slotIndex = static_cast<int>(i);
						break;
					}
				}

				if (slotIndex < 0)
				{
					slotIndex = static_cast<int>(this->vramSlots.size());
					this->vramSlots.emplace_back();
				}

				VramSlot &slot = this->vramSlots[slotIndex];
				slot.byteAddress = this->vramPoolStart + (firstPage * GS_PAGE_BYTES);
				slot.pageCount = pages;
				slot.ownerTexture = ownerID;
				slot.ownerIsUi = isUi;
				slot.lastUsedFrame = this->frameIndex;
				slot.inUse = true;

				this->vramUsedPages += pages;
				this->vramPeakPages = std::max(this->vramPeakPages, this->vramUsedPages);
				*outSlot = slotIndex;
				return true;
			}
		}

		if (!this->evictOne())
		{
			return false;
		}
	}

	return false;
}

void Ps2GsRenderBackend::freeVramSlot(int slotIndex)
{
	VramSlot &slot = this->vramSlots[slotIndex];
	if (!slot.inUse)
	{
		return;
	}

	const uint32_t firstPage = (slot.byteAddress - this->vramPoolStart) / GS_PAGE_BYTES;
	for (uint32_t p = 0; p < slot.pageCount; p++)
	{
		this->vramPageUsed[firstPage + p] = 0;
	}

	this->vramUsedPages -= slot.pageCount;

	Texture *owner = this->getTexture(slot.ownerIsUi, slot.ownerTexture);
	if ((owner != nullptr) && (owner->vramSlot == slotIndex))
	{
		owner->vramSlot = -1;
		owner->uploadedVersion = 0xFFFFFFFF;
	}

	slot = VramSlot();
}

// Evicts the least recently used resident texture. Textures used earlier in the current frame may be evicted too:
// uploads are queued inline, so the GS finishes the earlier draws before the overwriting upload executes.
bool Ps2GsRenderBackend::evictOne()
{
	int victim = -1;
	uint32_t oldest = 0xFFFFFFFF;
	for (size_t i = 0; i < this->vramSlots.size(); i++)
	{
		const VramSlot &slot = this->vramSlots[i];
		if (slot.inUse && (slot.lastUsedFrame < oldest))
		{
			oldest = slot.lastUsedFrame;
			victim = static_cast<int>(i);
		}
	}

	if (victim < 0)
	{
		return false;
	}

	this->freeVramSlot(victim);
	return true;
}

bool Ps2GsRenderBackend::ensureResident(bool isUi, int id)
{
	Texture *texture = this->getTexture(isUi, id);
	if ((texture == nullptr) || (texture->texels == nullptr))
	{
		return false;
	}

	if ((texture->vramSlot >= 0) && (texture->uploadedVersion == texture->version))
	{
		this->vramSlots[texture->vramSlot].lastUsedFrame = this->frameIndex;
		return true;
	}

	const int psm = (texture->bytesPerTexel == 1) ? GS_PSM_T8 : GS_PSM_CT32;
	if (texture->vramSlot < 0)
	{
		const uint32_t bytes = gsKit_texture_size(texture->tbw * 64, texture->height, psm);
		int slot;
		if (!this->allocVram(bytes, isUi, id, &slot))
		{
			DebugLogErrorFormat("GS VRAM exhausted for %dx%d texture.", texture->width, texture->height);
			return false;
		}

		// Allocation may have evicted and moved pool entries; re-fetch.
		texture = this->getTexture(isUi, id);
		texture->vramSlot = slot;
	}

	this->convertTexelsToGsAlpha(*texture);

	VramSlot &slot = this->vramSlots[texture->vramSlot];
	slot.lastUsedFrame = this->frameIndex;

	// Inline upload: queued ahead of the draws that use it. The EE copy stays valid until DMA completes because
	// the frame waits for the GIF channel before returning.
	gsKit_texture_send_inline(this->gs, reinterpret_cast<u32*>(texture->texels.get()), texture->width, texture->height,
		slot.byteAddress, psm, texture->tbw, GS_CLUT_NONE);

	static int s_debugUploadLogs = 0;
	if (s_debugUploadLogs < 12)
	{
		s_debugUploadLogs++;
		const uint32_t *t32 = reinterpret_cast<const uint32_t*>(texture->texels.get());
		const uint8_t *t8 = texture->texels.get();
		const size_t mid = texture->texelBytes / (2 * texture->bytesPerTexel);
		std::printf("[PS2][gs] upload %s %dx%d bpp=%d vram=0x%x tbw=%d tw=%d th=%d first=0x%08x mid=0x%08x\n", isUi ? "ui" : "obj",
			texture->width, texture->height, texture->bytesPerTexel, static_cast<unsigned>(slot.byteAddress), texture->tbw, texture->log2W, texture->log2H,
			(texture->bytesPerTexel == 4) ? static_cast<unsigned>(t32[0]) : t8[0], (texture->bytesPerTexel == 4) ? static_cast<unsigned>(t32[mid]) : t8[mid]);
	}

	texture->uploadedVersion = texture->version;
	this->statTextureUploads++;
	this->statUploadBytes += static_cast<int64_t>(texture->texelBytes);
	return true;
}

int Ps2GsRenderBackend::acquireClut(uint64_t cacheKey, const uint32_t *paletteRGBA, const uint8_t *lightTableRow, bool firstIndexTransparent)
{
	int freeSlot = -1;
	int oldestSlot = 0;
	for (int i = 0; i < CLUT_SLOT_COUNT; i++)
	{
		ClutSlot &slot = this->clutSlots[i];
		if (slot.valid && (slot.cacheKey == cacheKey))
		{
			slot.lastUsedFrame = this->frameIndex;
			return i;
		}

		if (!slot.valid && (freeSlot < 0))
		{
			freeSlot = i;
		}

		if (slot.lastUsedFrame < this->clutSlots[oldestSlot].lastUsedFrame)
		{
			oldestSlot = i;
		}
	}

	const int slotIndex = (freeSlot >= 0) ? freeSlot : oldestSlot;
	ClutSlot &slot = this->clutSlots[slotIndex];
	for (int i = 0; i < 256; i++)
	{
		const int paletteIndex = (lightTableRow != nullptr) ? lightTableRow[i] : i;
		const Color color = Color::fromRGBA(paletteRGBA[paletteIndex]);
		const uint32_t alpha = ((i == 0) && firstIndexTransparent) ? 0x00 : 0x80;
		slot.colors[ClutCsm1Index(i)] = static_cast<uint32_t>(color.r) | (static_cast<uint32_t>(color.g) << 8) |
			(static_cast<uint32_t>(color.b) << 16) | (alpha << 24);
	}

	slot.cacheKey = cacheKey;
	slot.lastUsedFrame = this->frameIndex;
	slot.valid = true;

	gsKit_texture_send_inline(this->gs, reinterpret_cast<u32*>(slot.colors), 16, 16, slot.byteAddress, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
	this->statUploadBytes += sizeof(slot.colors);
	return slotIndex;
}

// -------------------------------------------------------------------------------------------------
// Materials
// -------------------------------------------------------------------------------------------------
RenderMaterialID Ps2GsRenderBackend::createMaterial(RenderMaterialKey key)
{
	const RenderMaterialID id = this->materials.alloc();
	this->materials.get(id).key = key;
	return id;
}

void Ps2GsRenderBackend::freeMaterial(RenderMaterialID id)
{
	this->materials.free(id);
}

RenderMaterialInstanceID Ps2GsRenderBackend::createMaterialInstance()
{
	return this->materialInsts.alloc();
}

void Ps2GsRenderBackend::freeMaterialInstance(RenderMaterialInstanceID id)
{
	this->materialInsts.free(id);
}

void Ps2GsRenderBackend::setMaterialInstanceMeshLightPercent(RenderMaterialInstanceID id, double value)
{
	this->materialInsts.get(id).meshLightPercent = static_cast<float>(value);
}

void Ps2GsRenderBackend::setMaterialInstanceTexCoordAnimPercent(RenderMaterialInstanceID id, double value)
{
	this->materialInsts.get(id).texCoordAnimPercent = static_cast<float>(value);
}

// -------------------------------------------------------------------------------------------------
// GIF packet emission (straight into gsKit's one-shot DMA queue)
// -------------------------------------------------------------------------------------------------
void Ps2GsRenderBackend::emitRegisters(const uint64_t *valueRegPairs, int pairCount)
{
	uint64_t *p = static_cast<uint64_t*>(gsKit_heap_alloc(this->gs, pairCount, pairCount * 16, GIF_AD));
	*p++ = GifTag(pairCount, 1, 0, 0, FLG_PACKED, 1);
	*p++ = GIF_AD;
	std::memcpy(p, valueRegPairs, static_cast<size_t>(pairCount) * 16);
}

void Ps2GsRenderBackend::emitTriangles(const GsVertex *vertices, int vertexCount, uint64_t primRegister)
{
	// One GIF tag per chunk; NLOOP is 15 bits and DMA tags cap at 64K qwords.
	constexpr int MAX_VERTICES_PER_TAG = 3 * 2000;
	while (vertexCount > 0)
	{
		const int count = std::min(vertexCount, MAX_VERTICES_PER_TAG);
		const int doublewords = count * 3;
		const int qwords = (doublewords + 1) / 2;
		const uint64_t primRegs[] = { primRegister, GS_PRIM };
		this->emitRegisters(primRegs, 1);
		uint64_t *p = static_cast<uint64_t*>(gsKit_heap_alloc(this->gs, qwords, qwords * 16, GIF_AD));
		*p++ = GifTag(count, 1, 0, 0, FLG_REGLIST, 3); // PRIM is set via A+D: the GIFtag PRIM field is ignored in REGLIST mode.
		*p++ = REGS_TRI;
		std::memcpy(p, vertices, static_cast<size_t>(doublewords) * 8);
		if ((doublewords & 1) != 0)
		{
			p[doublewords] = 0;
		}

		vertices += count;
		vertexCount -= count;
	}
}

void Ps2GsRenderBackend::emitClear(uint32_t color)
{
	GSGLOBAL *gs = this->gs;
	const uint64_t regs[] =
	{
		GS_SETREG_SCISSOR(0, gs->Width - 1, 0, gs->Height - 1), GS_SCISSOR_1,
		GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), GS_TEST_1, // Z test ALWAYS.
		GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0), GS_ZBUF_1
	};
	this->emitRegisters(regs, 3);

	{
		const uint64_t primRegs[] = { Prim(PRIM_SPRITE, 0, 0, 0, 0, 0), GS_PRIM };
		this->emitRegisters(primRegs, 1);
	}

	uint64_t *p = static_cast<uint64_t*>(gsKit_heap_alloc(gs, 2, 32, GIF_AD));
	*p++ = GifTag(1, 1, 0, 0, FLG_REGLIST, 3);
	*p++ = 0x1 | (0x5 << 4) | (0x5 << 8); // RGBAQ, XYZ2, XYZ2
	*p++ = static_cast<uint64_t>(color) | (0x80ull << 24) | (static_cast<uint64_t>(FloatBits(1.0f)) << 32);
	*p++ = static_cast<uint64_t>(gs->OffsetX) | (static_cast<uint64_t>(gs->OffsetY) << 16);
	*p++ = static_cast<uint64_t>(gs->OffsetX + (gs->Width << 4)) | (static_cast<uint64_t>(gs->OffsetY + (gs->Height << 4)) << 16);
	*p++ = 0;
}

// -------------------------------------------------------------------------------------------------
// Frame
// -------------------------------------------------------------------------------------------------
void Ps2GsRenderBackend::submitFrame(const RenderDrawCommandList &renderCommandList, const UiDrawCommandList &uiCommandList,
	const RenderCamera &camera, const RenderFrameSettings &frameSettings)
{
	if (this->gs == nullptr)
	{
		return;
	}

	this->frameIndex++;
	this->statDrawCalls = this->statTriangles = this->statBatches = this->statTextureUploads = 0;
	this->statUploadBytes = 0;
	this->vertexBudgetExceeded = false;

	const Color clear = frameSettings.clearColor;
	this->emitClear(static_cast<uint32_t>(clear.r) | (static_cast<uint32_t>(clear.g) << 8) | (static_cast<uint32_t>(clear.b) << 16));

	if (renderCommandList.entryCount > 0)
	{
		this->drawScene(renderCommandList, camera, frameSettings);
	}

	this->drawUi(uiCommandList);

	// Write back D-cache so DMA sees texel/CLUT/queue data, then kick the queue and flip on vsync.
	FlushCache(0);
	gsKit_queue_exec(this->gs);
	gsKit_sync_flip(this->gs);
	dmaKit_wait(DMA_CHANNEL_GIF, 0); // EE-side texture memory is reusable once this returns.

	// Stats / instrumentation.
	const int64_t vramBytes = static_cast<int64_t>(this->vramUsedPages) * GS_PAGE_BYTES;
	Ps2Platform::setCounter(Ps2Platform::Counter::GsVramUsedBytes, vramBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::GsVramPeakBytes, static_cast<int64_t>(this->vramPeakPages) * GS_PAGE_BYTES);
	Ps2Platform::setCounter(Ps2Platform::Counter::GsTextureCount, this->objectTextures.getCount() + this->uiTextures.getCount());
	Ps2Platform::setCounter(Ps2Platform::Counter::GsTextureUploadBytesFrame, this->statUploadBytes);
	Ps2Platform::setCounter(Ps2Platform::Counter::DrawCalls, this->statDrawCalls);
	Ps2Platform::setCounter(Ps2Platform::Counter::Triangles, this->statTriangles);

	const Ps2Memory::FrameStats allocStats = Ps2Memory::takeFrameStats();

	// Periodic TTY summary (cheap; useful on hardware where the overlay isn't visible in logs).
	if ((this->frameIndex % 300) == 0)
	{
		Ps2Platform::sampleMemory();
		std::printf("[PS2][gs] frame %u: draws=%d tris=%d batches=%d uploads=%d (%u KiB) vram=%u/%u KiB ui=%d objTex=%d uiTex=%d allocs/frame=%u (%u B)%s\n",
			static_cast<unsigned>(this->frameIndex), this->statDrawCalls, this->statTriangles, this->statBatches, this->statTextureUploads,
			static_cast<unsigned>(this->statUploadBytes / 1024), static_cast<unsigned>(vramBytes / 1024),
			static_cast<unsigned>((this->vramPoolPages * GS_PAGE_BYTES) / 1024), uiCommandList.getTotalElementCount(),
			this->objectTextures.getCount(), this->uiTextures.getCount(), static_cast<unsigned>(allocStats.allocations), static_cast<unsigned>(allocStats.bytes),
			this->vertexBudgetExceeded ? " VERTEX-BUDGET-EXCEEDED" : "");
		Ps2Platform::logMemoryStats("frame");
	}

	const Int2 sceneDims = this->window->getSceneViewDimensions();
	this->profiler3D.width = this->gs->Width;
	this->profiler3D.height = (sceneDims.y * this->gs->Height) / std::max(1, this->window->getPixelDimensions().y);
	this->profiler3D.threadCount = 1;
	this->profiler3D.drawCallCount = this->statDrawCalls;
	this->profiler3D.presentedTriangleCount = this->statTriangles;
	this->profiler3D.objectTextureCount = this->objectTextures.getCount();
	this->profiler3D.objectTextureByteCount = this->objectTextureBytes;
	this->profiler3D.materialCount = this->materials.getCount();
	this->profiler3D.totalLightCount = frameSettings.visibleLightCount;
	this->profiler3D.totalCoverageTests = 0;
	this->profiler3D.totalDepthTests = 0;
	this->profiler3D.totalColorWrites = 0;
	this->profiler2D.drawCallCount = uiCommandList.getTotalElementCount();
	this->profiler2D.uiTextureCount = this->uiTextures.getCount();
	this->profiler2D.uiTextureByteCount = this->uiTextureBytes;
}

void Ps2GsRenderBackend::drawScene(const RenderDrawCommandList &renderCommandList, const RenderCamera &camera, const RenderFrameSettings &frameSettings)
{
	GSGLOBAL *gs = this->gs;

	// Viewport: the scene view in logical (window) space mapped onto the field framebuffer.
	const Int2 logicalDims = this->window->getPixelDimensions();
	const Int2 sceneDims = this->window->getSceneViewDimensions();
	const float fbScaleY = static_cast<float>(gs->Height) / static_cast<float>(logicalDims.y);
	const float viewW = static_cast<float>(sceneDims.x);
	const float viewH = static_cast<float>(sceneDims.y) * fbScaleY;
	const int scissorY1 = std::clamp(static_cast<int>(viewH) - 1, 0, gs->Height - 1);

	// View-projection in float (the engine's matrices are column-major doubles).
	const Matrix4d vpD = camera.projectionMatrix * camera.viewMatrix;
	float vp[16];
	{
		const Vector4f<double> *cols[4] = { &vpD.x, &vpD.y, &vpD.z, &vpD.w };
		for (int c = 0; c < 4; c++)
		{
			vp[(c * 4) + 0] = static_cast<float>(cols[c]->x);
			vp[(c * 4) + 1] = static_cast<float>(cols[c]->y);
			vp[(c * 4) + 2] = static_cast<float>(cols[c]->z);
			vp[(c * 4) + 3] = static_cast<float>(cols[c]->w);
		}
	}

	// Palette + light table for CLUT generation.
	Texture *paletteTexture = this->objectTextures.tryGet(frameSettings.paletteTextureID);
	Texture *lightTableTexture = this->objectTextures.tryGet(frameSettings.lightTableTextureID);
	if ((paletteTexture == nullptr) || (paletteTexture->bytesPerTexel != 4) || (paletteTexture->width * paletteTexture->height < 256))
	{
		return;
	}

	this->convertTexelsToEngineAlpha(*paletteTexture);
	const uint32_t *paletteRGBA = reinterpret_cast<const uint32_t*>(paletteTexture->texels.get());
	const int lightLevelCount = ((lightTableTexture != nullptr) && (lightTableTexture->bytesPerTexel == 1) && (lightTableTexture->width == 256)) ?
		lightTableTexture->height : 0;
	// The engine rewrites the palette texture every frame (sky gradient), so key CLUTs by palette *content*.
	uint32_t paletteHash = 2166136261u;
	for (int i = 0; i < 256; i++)
	{
		paletteHash = (paletteHash ^ paletteRGBA[i]) * 16777619u;
	}

	const uint64_t paletteKeyBase = (static_cast<uint64_t>(paletteHash) << 32) |
		(static_cast<uint64_t>(frameSettings.lightTableTextureID & 0xFFF) << 20) |
		(static_cast<uint64_t>((lightTableTexture != nullptr) ? (lightTableTexture->version & 0xFFF) : 0) << 8);

	// Visible lights (floating-origin space, same as model matrices).
	FrameLight lights[MAX_FRAME_LIGHTS];
	int lightCount = 0;
	if (UniformBuffer *lightBuffer = this->uniformBuffers.tryGet(frameSettings.visibleLightsBufferID))
	{
		const int count = std::min({ frameSettings.visibleLightCount, lightBuffer->elementCount, MAX_FRAME_LIGHTS });
		for (int i = 0; i < count; i++)
		{
			float f[5];
			std::memcpy(f, lightBuffer->bytes.data() + (static_cast<size_t>(i) * lightBuffer->bytesPerStride), sizeof(f));
			FrameLight &light = lights[lightCount++];
			light.x = f[0]; light.y = f[1]; light.z = f[2];
			light.start = f[3];
			light.startSqr = f[3] * f[3];
			light.endSqr = f[4] * f[4];
			light.radiusDiffRecip = (f[4] > f[3]) ? (1.0f / (f[4] - f[3])) : 0.0f;
		}
	}

	const float ambient = static_cast<float>(frameSettings.ambientPercent);
	const float screenAnim = static_cast<float>(frameSettings.screenSpaceAnimPercent);

	this->frameVertices.clear();
	this->frameBatches.clear();

	for (int entryIndex = 0; entryIndex < renderCommandList.entryCount; entryIndex++)
	{
		const Span<const RenderDrawCall> drawCalls = renderCommandList.entries[entryIndex];
		const size_t entryFirstBatch = this->frameBatches.size();

		for (int drawCallIndex = 0; drawCallIndex < drawCalls.getCount(); drawCallIndex++)
		{
			const RenderDrawCall &drawCall = drawCalls[drawCallIndex];
			const Material *material = this->materials.tryGet(drawCall.materialID);
			const VertexBuffer *positions = this->positionBuffers.tryGet(drawCall.positionBufferID);
			const VertexBuffer *texCoords = this->attributeBuffers.tryGet(drawCall.texCoordBufferID);
			const IndexBuffer *indices = this->indexBuffers.tryGet(drawCall.indexBufferID);
			const UniformBuffer *transforms = this->uniformBuffers.tryGet(drawCall.transformBufferID);
			if ((material == nullptr) || (positions == nullptr) || (texCoords == nullptr) || (indices == nullptr) || (transforms == nullptr))
			{
				continue;
			}

			if ((drawCall.transformIndex < 0) || (drawCall.transformIndex >= transforms->elementCount) || (material->key.textureCount <= 0))
			{
				continue;
			}

			const RenderMaterialKey &key = material->key;
			const ObjectTextureID textureID = key.textureIDs[0];
			Texture *texture = this->objectTextures.tryGet(textureID);
			if (texture == nullptr)
			{
				continue;
			}

			this->statDrawCalls++;

			const MaterialInstance *materialInst = this->materialInsts.tryGet(drawCall.materialInstID);
			const float meshLight = (materialInst != nullptr) ? materialInst->meshLightPercent : 1.0f;
			const float texAnim = (materialInst != nullptr) ? materialInst->texCoordAnimPercent : 0.0f;
			const FragmentShaderType fragmentShader = key.fragmentShaderType;
			const bool isOpaque = RenderShaderUtils::isOpaque(fragmentShader);
			const bool usesPerPixelLight = key.lightingType == RenderLightingType::PerPixel;
			const bool usesMeshLight = RenderShaderUtils::requiresMeshLightPercent(fragmentShader) && !usesPerPixelLight;
			const bool isBlended = (fragmentShader == FragmentShaderType::AlphaTestedWithLightLevelOpacity);

			// Model and MVP.
			float m[16];
			std::memcpy(m, transforms->bytes.data() + (static_cast<size_t>(drawCall.transformIndex) * transforms->bytesPerStride), sizeof(m));
			float mvp[16];
			for (int c = 0; c < 4; c++)
			{
				for (int r = 0; r < 4; r++)
				{
					mvp[(c * 4) + r] = (vp[r] * m[c * 4]) + (vp[4 + r] * m[(c * 4) + 1]) + (vp[8 + r] * m[(c * 4) + 2]) + (vp[12 + r] * m[(c * 4) + 3]);
				}
			}

			// CLUT: per-mesh lighting picks the exact light-table row; per-pixel lighting uses the brightest row and
			// modulates per vertex.
			int lightLevel = 0;
			if (usesMeshLight && (lightLevelCount > 0))
			{
				const int lastLevel = lightLevelCount - 1;
				const int clamped = std::clamp(static_cast<int>(meshLight * static_cast<float>(lightLevelCount)), 0, lastLevel);
				lightLevel = lastLevel - clamped;
			}

			const bool alphaTested = !isOpaque || (fragmentShader == FragmentShaderType::OpaqueWithAlphaTestLayer);
			const int16_t batchLightLevel = (texture->bytesPerTexel == 1) ? static_cast<int16_t>(lightLevel) : static_cast<int16_t>(-1);

			// Texture coordinate scale (textures are stored in power-of-two GS dimensions).
			const float sScale = static_cast<float>(texture->width) / static_cast<float>(1 << texture->log2W);
			const float tScale = static_cast<float>(texture->height) / static_cast<float>(1 << texture->log2H);
			const bool isVariableU = fragmentShader == FragmentShaderType::AlphaTestedWithVariableTexCoordUMin;
			const bool isVariableV = fragmentShader == FragmentShaderType::AlphaTestedWithVariableTexCoordVMin;
			const bool isScreenAnim = (fragmentShader == FragmentShaderType::OpaqueScreenSpaceAnimation) ||
				(fragmentShader == FragmentShaderType::OpaqueScreenSpaceAnimationWithAlphaTestLayer);

			const uint32_t firstVertex = static_cast<uint32_t>(this->frameVertices.size());
			const int posComponents = positions->componentsPerVertex;
			const int uvComponents = texCoords->componentsPerVertex;
			const int indexCount = static_cast<int>(indices->indices.size());
			bool anyRepeatU = false, anyRepeatV = false;

			for (int tri = 0; (tri + 2) < indexCount; tri += 3)
			{
				ClipVertex polyA[MAX_CLIP_VERTICES], polyB[MAX_CLIP_VERTICES];
				int polyCount = 3;
				bool allInside = true;
				bool invalid = false;

				for (int k = 0; k < 3; k++)
				{
					const int vi = indices->indices[tri + k];
					if ((vi < 0) || (vi >= positions->vertexCount) || (vi >= texCoords->vertexCount))
					{
						invalid = true;
						break;
					}

					const float *pos = positions->data.data() + (vi * posComponents);
					const float *uv = texCoords->data.data() + (vi * uvComponents);
					const float px = pos[0], py = pos[1], pz = pos[2];

					ClipVertex &cv = polyA[k];
					cv.x = (mvp[0] * px) + (mvp[4] * py) + (mvp[8] * pz) + mvp[12];
					cv.y = (mvp[1] * px) + (mvp[5] * py) + (mvp[9] * pz) + mvp[13];
					cv.z = (mvp[2] * px) + (mvp[6] * py) + (mvp[10] * pz) + mvp[14];
					cv.w = (mvp[3] * px) + (mvp[7] * py) + (mvp[11] * pz) + mvp[15];
					cv.u = uv[0];
					cv.v = uv[1];

					if (isVariableU)
					{
						cv.u = texAnim + ((1.0f - texAnim) * cv.u);
					}
					else if (isVariableV)
					{
						cv.v = texAnim + ((1.0f - texAnim) * cv.v);
					}
					else if (isScreenAnim)
					{
						cv.v += screenAnim; // Approximates Arena's screen-space water/lava animation.
					}

					anyRepeatU |= (cv.u < -0.001f) || (cv.u > 1.001f);
					anyRepeatV |= (cv.v < -0.001f) || (cv.v > 1.001f);

					if (usesPerPixelLight)
					{
						const float wx = (m[0] * px) + (m[4] * py) + (m[8] * pz) + m[12];
						const float wy = (m[1] * px) + (m[5] * py) + (m[9] * pz) + m[13];
						const float wz = (m[2] * px) + (m[6] * py) + (m[10] * pz) + m[14];
						float intensity = ambient;
						for (int li = 0; (li < lightCount) && (intensity < 1.0f); li++)
						{
							const FrameLight &light = lights[li];
							const float dx = light.x - wx, dy = light.y - wy, dz = light.z - wz;
							const float distSqr = (dx * dx) + (dy * dy) + (dz * dz);
							if (distSqr <= light.startSqr)
							{
								intensity = 1.0f;
							}
							else if (distSqr < light.endSqr)
							{
								intensity += std::clamp(1.0f - ((std::sqrt(distSqr) - light.start) * light.radiusDiffRecip), 0.0f, 1.0f);
							}
						}

						cv.light = std::min(intensity, 1.0f);
					}
					else
					{
						cv.light = 1.0f;
					}

					const bool inside = (cv.z >= 0.0f) && (std::fabs(cv.x) <= (GUARD_BAND * cv.w)) && (std::fabs(cv.y) <= (GUARD_BAND * cv.w));
					allInside &= inside;
				}

				if (invalid)
				{
					break;
				}

				const ClipVertex *poly = polyA;
				if (!allInside)
				{
					// Near plane (z >= 0), then guard band planes.
					polyCount = ClipPolygon(polyA, polyCount, polyB, 0.0f, 0.0f, 1.0f, 0.0f);
					polyCount = (polyCount >= 3) ? ClipPolygon(polyB, polyCount, polyA, -1.0f, 0.0f, 0.0f, GUARD_BAND) : 0;
					polyCount = (polyCount >= 3) ? ClipPolygon(polyA, polyCount, polyB, 1.0f, 0.0f, 0.0f, GUARD_BAND) : 0;
					polyCount = (polyCount >= 3) ? ClipPolygon(polyB, polyCount, polyA, 0.0f, -1.0f, 0.0f, GUARD_BAND) : 0;
					polyCount = (polyCount >= 3) ? ClipPolygon(polyA, polyCount, polyB, 0.0f, 1.0f, 0.0f, GUARD_BAND) : 0;
					poly = polyB;
				}

				if (polyCount < 3)
				{
					continue;
				}

				// Project.
				GsVertex projected[MAX_CLIP_VERTICES];
				float sx[MAX_CLIP_VERTICES], sy[MAX_CLIP_VERTICES];
				for (int k = 0; k < polyCount; k++)
				{
					const ClipVertex &cv = poly[k];
					const float invW = 1.0f / std::max(cv.w, 1.0e-6f);
					sx[k] = ((cv.x * invW * 0.5f) + 0.5f) * viewW;
					sy[k] = (0.5f - (cv.y * invW * 0.5f)) * viewH;

					const int x16 = static_cast<int>(sx[k] * 16.0f) + gs->OffsetX;
					const int y16 = static_cast<int>(sy[k] * 16.0f) + gs->OffsetY;
					const uint32_t z = static_cast<uint32_t>(std::clamp(ZK * invW, 1.0f, Z_MAX));
					const uint32_t c = static_cast<uint32_t>(std::clamp(cv.light * 128.0f, 0.0f, 128.0f));
					const float q = invW;

					GsVertex &gv = projected[k];
					gv.rgbaq = c | (c << 8) | (c << 16) | (0x80u << 24) | (static_cast<uint64_t>(FloatBits(q)) << 32);
					gv.st = static_cast<uint64_t>(FloatBits(cv.u * sScale * q)) | (static_cast<uint64_t>(FloatBits(cv.v * tScale * q)) << 32);
					gv.xyz2 = static_cast<uint64_t>(x16 & 0xFFFF) | (static_cast<uint64_t>(y16 & 0xFFFF) << 16) | (static_cast<uint64_t>(z) << 32);
				}

				// Back-face test on the whole polygon (same orientation rule as the software renderer).
				if (key.enableBackFaceCulling)
				{
					float area2 = 0.0f;
					for (int k = 0; k < polyCount; k++)
					{
						const int n = (k + 1) % polyCount;
						area2 += (sx[k] * sy[n]) - (sx[n] * sy[k]);
					}

					if (area2 >= 0.0f)
					{
						continue;
					}
				}

				const int triCount = polyCount - 2;
				if ((this->frameVertices.size() + (triCount * 3)) > MAX_FRAME_VERTICES)
				{
					this->vertexBudgetExceeded = true;
					break;
				}

				for (int k = 1; (k + 1) < polyCount; k++)
				{
					this->frameVertices.push_back(projected[0]);
					this->frameVertices.push_back(projected[k]);
					this->frameVertices.push_back(projected[k + 1]);
				}

				this->statTriangles += triCount;
			}

			const uint32_t vertexCount = static_cast<uint32_t>(this->frameVertices.size()) - firstVertex;
			if ((vertexCount == 0) || (this->frameBatches.size() >= MAX_FRAME_BATCHES))
			{
				continue;
			}

			uint16_t state = 0;
			state |= alphaTested ? STATE_ALPHA_TEST : 0;
			state |= isBlended ? STATE_BLEND : 0;
			state |= key.enableDepthRead ? STATE_DEPTH_READ : 0;
			state |= key.enableDepthWrite ? STATE_DEPTH_WRITE : 0;
			state |= anyRepeatU ? STATE_REPEAT_U : 0;
			state |= anyRepeatV ? STATE_REPEAT_V : 0;

			Batch batch;
			batch.textureID = textureID;
			batch.lightLevel = batchLightLevel;
			batch.stateFlags = state;
			batch.firstVertex = firstVertex;
			batch.vertexCount = vertexCount;

			// Blended draws keep submission order; everything else groups by texture/CLUT/state.
			const uint64_t sequence = this->frameBatches.size() - entryFirstBatch;
			batch.sortKey = isBlended ? ((1ull << 63) | sequence) :
				((static_cast<uint64_t>(textureID & 0xFFFFF) << 32) | (static_cast<uint64_t>((batchLightLevel + 1) & 0xFF) << 16) | state);
			this->frameBatches.push_back(batch);
		}

		// Sort this entry's batches (entries themselves stay ordered: sky, voxels, entities, weather...).
		std::stable_sort(this->frameBatches.begin() + entryFirstBatch, this->frameBatches.end(),
			[](const Batch &a, const Batch &b) { return a.sortKey < b.sortKey; });
	}

	if (this->vertexBudgetExceeded)
	{
		static uint32_t s_lastWarnFrame = 0;
		if ((this->frameIndex - s_lastWarnFrame) > 300)
		{
			DebugLogWarningFormat("PS2 frame vertex budget (%u) exceeded; some geometry was skipped.", MAX_FRAME_VERTICES);
			s_lastWarnFrame = this->frameIndex;
		}
	}

	// Emit batches with minimal state changes.
	int boundTexture = -1;
	int boundClut = -2;
	int boundState = -1;
	for (const Batch &batch : this->frameBatches)
	{
		Texture *texture = this->objectTextures.tryGet(batch.textureID);
		if ((texture == nullptr) || !this->ensureResident(false, batch.textureID))
		{
			continue;
		}

		texture = this->objectTextures.tryGet(batch.textureID);
		const VramSlot &slot = this->vramSlots[texture->vramSlot];
		const bool isPaletted = texture->bytesPerTexel == 1;
		const int state = batch.stateFlags;

		// CLUTs are acquired (and uploaded inline) right before the batches that use them, so a slot reused later in
		// the frame can't affect already-queued draws.
		int clutSlot = -1;
		if (isPaletted)
		{
			const bool alphaTested = (state & STATE_ALPHA_TEST) != 0;
			const int level = std::max<int>(batch.lightLevel, 0);
			const uint8_t *lightRow = (lightLevelCount > 0) ? (lightTableTexture->texels.get() + (level * 256)) : nullptr;
			const uint64_t clutKey = paletteKeyBase | (static_cast<uint64_t>(level & 0x7F) << 1) | (alphaTested ? 1u : 0u);
			clutSlot = this->acquireClut(clutKey, paletteRGBA, lightRow, alphaTested);
		}

		if ((batch.textureID != boundTexture) || (clutSlot != boundClut) || (state != boundState))
		{
			const int wms = ((state & STATE_REPEAT_U) != 0) ? 0 : 1; // REPEAT : CLAMP
			const int wmt = ((state & STATE_REPEAT_V) != 0) ? 0 : 1;
			const uint32_t cbp = (clutSlot >= 0) ? (this->clutSlots[clutSlot].byteAddress / 256) : 0;
			const int ztst = ((state & STATE_DEPTH_READ) != 0) ? 2 : 1; // GEQUAL : ALWAYS
			const uint64_t regs[] =
			{
				GS_SETREG_TEX0(slot.byteAddress / 256, texture->tbw, isPaletted ? GS_PSM_T8 : GS_PSM_CT32, texture->log2W, texture->log2H,
					1, 0, cbp, 0, 0, 0, isPaletted ? 1 : 0), GS_TEX0_1,
				GS_SETREG_TEX1(0, 0, 0, 0, 0, 0, 0), GS_TEX1_1,
				GS_SETREG_CLAMP(wms, wmt, 0, 0, 0, 0), GS_CLAMP_1,
				GS_SETREG_TEST(((state & STATE_ALPHA_TEST) != 0) ? 1 : 0, 5, 0x40, 0, 0, 0, 1, ztst), GS_TEST_1,
				GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, ((state & STATE_DEPTH_WRITE) != 0) ? 0 : 1), GS_ZBUF_1,
				GS_SETREG_ALPHA(0, 1, 0, 1, 0), GS_ALPHA_1,
				GS_SETREG_SCISSOR(0, gs->Width - 1, 0, scissorY1), GS_SCISSOR_1,
				0, GS_TEXFLUSH
			};

			this->emitRegisters(regs, 8);
			boundTexture = batch.textureID;
			boundClut = clutSlot;
			boundState = state;
			this->statBatches++;
		}

		const uint64_t prim = Prim(PRIM_TRIANGLE, 1, 1, 0, ((state & STATE_BLEND) != 0) ? 1 : 0, 0);
		this->emitTriangles(this->frameVertices.data() + batch.firstVertex, static_cast<int>(batch.vertexCount), prim);
	}
}

void Ps2GsRenderBackend::drawUi(const UiDrawCommandList &uiCommandList)
{
	GSGLOBAL *gs = this->gs;
	const Int2 logicalDims = this->window->getPixelDimensions();
	const float scaleX = static_cast<float>(gs->Width) / static_cast<float>(logicalDims.x);
	const float scaleY = static_cast<float>(gs->Height) / static_cast<float>(logicalDims.y);

	int boundTexture = -1;
	bool boundClip = true;
	for (int entryIndex = 0; entryIndex < uiCommandList.entryCount; entryIndex++)
	{
		const Span<const RenderElement2D> elements = uiCommandList.entries[entryIndex];
		for (int i = 0; i < elements.getCount(); i++)
		{
			const RenderElement2D &element = elements[i];
			const Rect &rect = element.rect;
			if ((rect.width <= 0) || (rect.height <= 0) || !this->ensureResident(true, element.id))
			{
				continue;
			}

			Texture *texture = this->uiTextures.tryGet(element.id);
			const VramSlot &slot = this->vramSlots[texture->vramSlot];

			const bool hasClip = (element.clipRect.width > 0) && (element.clipRect.height > 0);
			const int clipX0 = hasClip ? std::max(0, static_cast<int>(element.clipRect.x * scaleX)) : 0;
			const int clipY0 = hasClip ? std::max(0, static_cast<int>(element.clipRect.y * scaleY)) : 0;
			const int clipX1 = hasClip ? std::min(gs->Width - 1, static_cast<int>((element.clipRect.x + element.clipRect.width) * scaleX) - 1) : (gs->Width - 1);
			const int clipY1 = hasClip ? std::min(gs->Height - 1, static_cast<int>((element.clipRect.y + element.clipRect.height) * scaleY) - 1) : (gs->Height - 1);

			if ((element.id != boundTexture) || hasClip || boundClip)
			{
				const uint64_t regs[] =
				{
					GS_SETREG_TEX0(slot.byteAddress / 256, texture->tbw, GS_PSM_CT32, texture->log2W, texture->log2H, 1, 0, 0, 0, 0, 0, 0), GS_TEX0_1,
					GS_SETREG_TEX1(0, 0, 0, 0, 0, 0, 0), GS_TEX1_1,
					GS_SETREG_CLAMP(1, 1, 0, 0, 0, 0), GS_CLAMP_1,
					GS_SETREG_TEST(1, 7, 0x00, 0, 0, 0, 1, 1), GS_TEST_1, // Skip fully transparent texels, no Z test.
					GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 1), GS_ZBUF_1,
					GS_SETREG_ALPHA(0, 1, 0, 1, 0), GS_ALPHA_1,
					GS_SETREG_SCISSOR(clipX0, clipX1, clipY0, clipY1), GS_SCISSOR_1,
					0, GS_TEXFLUSH
				};

				this->emitRegisters(regs, 8);
				boundTexture = element.id;
				boundClip = hasClip;
			}

			const int x0 = static_cast<int>(rect.x * scaleX * 16.0f) + gs->OffsetX;
			const int y0 = static_cast<int>(rect.y * scaleY * 16.0f) + gs->OffsetY;
			const int x1 = static_cast<int>((rect.x + rect.width) * scaleX * 16.0f) + gs->OffsetX;
			const int y1 = static_cast<int>((rect.y + rect.height) * scaleY * 16.0f) + gs->OffsetY;

			{
				const uint64_t primRegs[] = { Prim(PRIM_SPRITE, 0, 1, 0, 1, 1), GS_PRIM };
				this->emitRegisters(primRegs, 1);
			}

			uint64_t *p = static_cast<uint64_t*>(gsKit_heap_alloc(gs, 3, 48, GIF_AD));
			*p++ = GifTag(1, 1, 0, 0, FLG_REGLIST, 5);
			*p++ = REGS_SPRITE;
			*p++ = 0x80 | (0x80 << 8) | (0x80 << 16) | (0x80ull << 24) | (static_cast<uint64_t>(FloatBits(1.0f)) << 32);
			*p++ = 0;                                                                                            // UV 0,0
			*p++ = static_cast<uint64_t>(x0 & 0xFFFF) | (static_cast<uint64_t>(y0 & 0xFFFF) << 16);
			*p++ = static_cast<uint64_t>(texture->width << 4) | (static_cast<uint64_t>(texture->height << 4) << 16); // UV w,h
			*p++ = static_cast<uint64_t>(x1 & 0xFFFF) | (static_cast<uint64_t>(y1 & 0xFFFF) << 16);
			*p++ = 0;
		}
	}
}
