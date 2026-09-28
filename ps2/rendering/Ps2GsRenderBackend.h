#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "OpenTESArena/src/Rendering/RenderBackend.h"
#include "OpenTESArena/src/Rendering/RenderMaterialUtils.h"

#include "components/utilities/KeyValuePool.h"

struct gsGlobal;
struct Window;

// PlayStation 2 Graphics Synthesizer render backend.
//
// Implements OpenTESArena's RenderBackend on the GS through gsKit/dmaKit:
//   - The EE transforms, clips (near plane + guard band) and lights vertices, then writes batched REGLIST GIF
//     packets (24 bytes/vertex) straight into gsKit's double-buffered DMA queue. No framebuffer is ever produced or
//     copied by the CPU; the GS rasterizes everything.
//   - Draw calls within each command-list range are sorted by GS state (texture, CLUT, alpha/depth mode) so a frame
//     is a few dozen state changes rather than one per voxel face. Blended draws keep submission order.
//   - Arena's 8-bit textures stay 8-bit: they are uploaded as PSMT8 with a 256-entry CLUT. Per-mesh lighting uses a
//     CLUT built from palette + Arena's light table row, which reproduces the original colormap shading exactly;
//     per-pixel lit geometry uses Gouraud modulation against the brightest row.
//   - GS VRAM (4 MiB) is a managed cache: framebuffers/Z first, then a page allocator for textures and CLUT slots
//     with LRU eviction and re-upload from the EE copy. Uploads are queued inline so they stay ordered with draws.
//   - Output: interlaced field buffers (640x224 NTSC / 640x256 PAL), 16-bit color + 16-bit Z, 4:3.
namespace Ps2Gs
{
	// Texel storage is DMA source memory: EE DMA requires 16-byte alignment; use a full cache line.
	struct AlignedFree { void operator()(uint8_t *p) const { std::free(p); } };

	struct VertexBuffer
	{
		std::vector<float> data;
		int vertexCount = 0;
		int componentsPerVertex = 0;
	};

	struct IndexBuffer
	{
		std::vector<int32_t> indices;
	};

	struct UniformBuffer
	{
		std::vector<std::byte> bytes;
		int elementCount = 0;
		int bytesPerElement = 0;
		int bytesPerStride = 0;
	};

	struct Texture
	{
		int width = 0, height = 0;
		int bytesPerTexel = 0;
		std::unique_ptr<uint8_t[], AlignedFree> texels; // EE copy (engine format while locked, GS format otherwise). 64-byte aligned for DMA.
		size_t texelBytes = 0;
		bool isUi = false;
		bool gsAlpha = false;          // 32-bit texels currently hold GS alpha (0..0x80).
		uint32_t version = 0;          // Bumped on every unlock.
		uint32_t uploadedVersion = 0xFFFFFFFF;
		int vramSlot = -1;             // Index into the VRAM residency table, or -1 if not resident.
		int log2W = 0, log2H = 0;      // GS TW/TH.
		int tbw = 1;                   // Buffer width in 64-texel units.
	};

	struct Material
	{
		RenderMaterialKey key;
	};

	struct MaterialInstance
	{
		float meshLightPercent = 1.0f;
		float texCoordAnimPercent = 0.0f;
	};

	// REGLIST vertex: RGBAQ, ST, XYZ2 as raw 64-bit register values.
	struct GsVertex
	{
		uint64_t rgbaq;
		uint64_t st;
		uint64_t xyz2;
	};

	// Recorded triangles for one draw call, emitted later in sorted order.
	struct Batch
	{
		uint64_t sortKey;
		ObjectTextureID textureID;
		int16_t lightLevel;     // Light table row for the CLUT (0 = brightest), or -1 for non-paletted textures.
		uint16_t stateFlags;
		uint32_t firstVertex;
		uint32_t vertexCount;
	};

	struct VramSlot
	{
		uint32_t byteAddress = 0;
		uint32_t pageCount = 0;
		int ownerTexture = -1;   // ObjectTextureID or UiTextureID (see ownerIsUi).
		bool ownerIsUi = false;
		uint32_t lastUsedFrame = 0;
		bool inUse = false;
	};

	struct ClutSlot
	{
		uint32_t byteAddress = 0;
		uint64_t cacheKey = 0;
		uint32_t lastUsedFrame = 0;
		bool valid = false;
		alignas(64) uint32_t colors[256]; // CSM1-swizzled, GS alpha. Must persist until the queue's DMA completes.
	};
}

class Ps2GsRenderBackend final : public RenderBackend
{
public:
	using VertexBuffer = Ps2Gs::VertexBuffer;
	using IndexBuffer = Ps2Gs::IndexBuffer;
	using UniformBuffer = Ps2Gs::UniformBuffer;
	using Texture = Ps2Gs::Texture;
	using Material = Ps2Gs::Material;
	using MaterialInstance = Ps2Gs::MaterialInstance;
	using GsVertex = Ps2Gs::GsVertex;
	using Batch = Ps2Gs::Batch;
	using VramSlot = Ps2Gs::VramSlot;
	using ClutSlot = Ps2Gs::ClutSlot;
private:
	const Window *window;
	gsGlobal *gs;

	KeyValuePool<VertexPositionBufferID, VertexBuffer> positionBuffers;
	KeyValuePool<VertexAttributeBufferID, VertexBuffer> attributeBuffers;
	KeyValuePool<IndexBufferID, IndexBuffer> indexBuffers;
	KeyValuePool<UniformBufferID, UniformBuffer> uniformBuffers;
	KeyValuePool<ObjectTextureID, Texture> objectTextures;
	KeyValuePool<UiTextureID, Texture> uiTextures;
	KeyValuePool<RenderMaterialID, Material> materials;
	KeyValuePool<RenderMaterialInstanceID, MaterialInstance> materialInsts;

	// VRAM management.
	uint32_t vramPoolStart;  // First byte address after framebuffers/Z.
	uint32_t vramPoolPages;
	std::vector<uint8_t> vramPageUsed;  // Per 8 KiB page.
	std::vector<VramSlot> vramSlots;
	uint32_t vramUsedPages;
	uint32_t vramPeakPages;
	static constexpr int CLUT_SLOT_COUNT = 32;
	std::unique_ptr<ClutSlot[]> clutSlots;

	// Per-frame scratch (reused; grows to a high-water mark, never per-frame allocated).
	std::vector<GsVertex> frameVertices;
	std::vector<Batch> frameBatches;
	std::vector<GsVertex> clipScratch;
	uint32_t frameIndex;
	bool vertexBudgetExceeded;

	// Stats.
	int statDrawCalls, statTriangles, statBatches, statTextureUploads;
	int64_t statUploadBytes;
	int64_t objectTextureBytes, uiTextureBytes;
	RendererProfilerData2D profiler2D;
	RendererProfilerData3D profiler3D;

	Texture *getTexture(bool isUi, int id);
	bool ensureResident(bool isUi, int id);
	bool allocVram(uint32_t bytes, bool isUi, int ownerID, int *outSlot);
	void freeVramSlot(int slot);
	bool evictOne();
	int acquireClut(uint64_t cacheKey, const uint32_t *paletteRGBA, const uint8_t *lightTableRow, bool firstIndexTransparent);

	void emitRegisters(const uint64_t *valueRegPairs, int pairCount);
	void emitTriangles(const GsVertex *vertices, int vertexCount, uint64_t primRegister);
	void emitClear(uint32_t color);

	void drawScene(const RenderDrawCommandList &renderCommandList, const RenderCamera &camera, const RenderFrameSettings &frameSettings);
	void drawUi(const UiDrawCommandList &uiCommandList);
	void convertTexelsToGsAlpha(Texture &texture);
	void convertTexelsToEngineAlpha(Texture &texture);
public:
	Ps2GsRenderBackend();
	~Ps2GsRenderBackend() override;

	bool initContext(const RenderContextSettings &contextSettings) override;
	bool initRendering(const RenderInitSettings &initSettings) override;
	void shutdown() override;

	void resize(int windowWidth, int windowHeight, int sceneViewWidth, int sceneViewHeight, int internalWidth, int internalHeight) override;
	void handleRenderTargetsReset(int windowWidth, int windowHeight, int sceneViewWidth, int sceneViewHeight, int internalWidth, int internalHeight) override;

	RendererProfilerData2D getProfilerData2D() const override;
	RendererProfilerData3D getProfilerData3D() const override;

	Surface getScreenshot() const override;

	int getBytesPerFloat() const override;

	VertexPositionBufferID createVertexPositionBuffer(int vertexCount, int componentsPerVertex, int bytesPerComponent) override;
	void freeVertexPositionBuffer(VertexPositionBufferID id) override;
	LockedBuffer lockVertexPositionBuffer(VertexPositionBufferID id) override;
	void unlockVertexPositionBuffer(VertexPositionBufferID id) override;

	VertexAttributeBufferID createVertexAttributeBuffer(int vertexCount, int componentsPerVertex, int bytesPerComponent) override;
	void freeVertexAttributeBuffer(VertexAttributeBufferID id) override;
	LockedBuffer lockVertexAttributeBuffer(VertexAttributeBufferID id) override;
	void unlockVertexAttributeBuffer(VertexAttributeBufferID id) override;

	IndexBufferID createIndexBuffer(int indexCount, int bytesPerIndex) override;
	void freeIndexBuffer(IndexBufferID id) override;
	LockedBuffer lockIndexBuffer(IndexBufferID id) override;
	void unlockIndexBuffer(IndexBufferID id) override;

	UniformBufferID createUniformBuffer(int elementCount, int bytesPerElement, int alignmentOfElement) override;
	void freeUniformBuffer(UniformBufferID id) override;
	LockedBuffer lockUniformBuffer(UniformBufferID id) override;
	LockedBuffer lockUniformBufferIndex(UniformBufferID id, int index) override;
	void unlockUniformBuffer(UniformBufferID id) override;
	void unlockUniformBufferIndex(UniformBufferID id, int index) override;

	ObjectTextureID createObjectTexture(int width, int height, int bytesPerTexel) override;
	void freeObjectTexture(ObjectTextureID id) override;
	std::optional<Int2> tryGetObjectTextureDims(ObjectTextureID id) const override;
	LockedTexture lockObjectTexture(ObjectTextureID id) override;
	void unlockObjectTexture(ObjectTextureID id) override;

	UiTextureID createUiTexture(int width, int height) override;
	void freeUiTexture(UiTextureID id) override;
	std::optional<Int2> tryGetUiTextureDims(UiTextureID id) const override;
	LockedTexture lockUiTexture(UiTextureID id) override;
	void unlockUiTexture(UiTextureID id) override;

	RenderMaterialID createMaterial(RenderMaterialKey key) override;
	void freeMaterial(RenderMaterialID id) override;

	RenderMaterialInstanceID createMaterialInstance() override;
	void freeMaterialInstance(RenderMaterialInstanceID id) override;
	void setMaterialInstanceMeshLightPercent(RenderMaterialInstanceID id, double value) override;
	void setMaterialInstanceTexCoordAnimPercent(RenderMaterialInstanceID id, double value) override;

	void submitFrame(const RenderDrawCommandList &renderCommandList, const UiDrawCommandList &uiCommandList,
		const RenderCamera &camera, const RenderFrameSettings &frameSettings) override;
};
