#pragma once

#include "RenderShaderUtils.h"
#include "../Math/Matrix4.h"

#include "components/utilities/FixedPool.h"

// Unique ID for a mesh allocated in the renderer's internal format.
using VertexPositionBufferID = int;

// Unique ID for mesh attributes allocated in the renderer's internal format.
using VertexAttributeBufferID = int;

// Unique ID for a set of mesh indices allocated in the renderer's internal format.
using IndexBufferID = int;

// One per uniform buffer.
struct RenderTransformHeap
{
#if defined(__PS2__)
	// PS2 has 32 MiB of RAM total; each slot costs a Matrix4d here plus a float copy in the render backend.
	static constexpr int MAX_TRANSFORMS = 2048;
#else
	static constexpr int MAX_TRANSFORMS = 8192;
#endif

	UniformBufferID uniformBufferID;
	FixedPool<Matrix4d, MAX_TRANSFORMS> pool; // Copied into uniform buffer every frame.

	RenderTransformHeap();

	int alloc();
	void free(int transformIndex);
	void clear();

	// Number of leading slots that may be in use (highest allocated index + 1). Only this prefix needs uploading.
	int getHighWaterCount() const;
};
