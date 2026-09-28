#pragma once

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyID.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Jolt/Physics/PhysicsSystem.h"

#include "../Entities/EntityInstance.h"
#include "../Math/Vector2.h"
#include "../Math/Vector3.h"
#include "../Voxels/VoxelUtils.h"

class CollisionChunkManager;
class EntityChunkManager;
class VoxelChunkManager;

struct RayCastHit;

namespace Physics
{
	// Jolt init values.
#if defined(__PS2__)
	// PS2 physics backend (ps2/physics/JoltLite): fixed pools sized for ChunkDistance ~1-2 within 32 MiB of RAM.
	constexpr int TempAllocatorByteCount = 0; // Unused by JoltLite.
	constexpr int MaxBodies = 4096;
	constexpr int BodyMutexCount = 0;
	constexpr int MaxBodyPairs = 2048;
	constexpr int MaxContactConstraints = MaxBodyPairs / 4;
	constexpr double DeltaTime = 1.0 / 120.0; // JoltLite's push-out resolver is stable at this rate; halves EE cost.
#else
	constexpr int TempAllocatorByteCount = 64 * 1024 * 1024;
	constexpr int MaxBodies = 250000;
	constexpr int BodyMutexCount = 0; // Use default settings.
	constexpr int MaxBodyPairs = 1 << 16;
	constexpr int MaxContactConstraints = MaxBodyPairs / 4;
	constexpr double DeltaTime = 1.0 / 240.0; // Very high # of updates per frame to help prevent bumpy road feeling at lower FPS.
#endif

	int getThreadCount(int platformThreadCount);

	// Shape creation tweaks.
	constexpr double BoxConvexRadius = 0.020;

	constexpr double GRAVITY = 9.81;

	const JPH::BodyID INVALID_BODY_ID;
	const JPH::SubShapeID INVALID_SUB_SHAPE_ID;

	// @todo: bit mask elements for each voxel type.

	// Casts a ray through the world and writes any intersection data into the output parameter. Returns true
	// if the ray hit something.
	bool rayCast(const CoordDouble3 &rayStart, const VoxelDouble3 &rayDirection, double ceilingScale,
		const VoxelDouble3 &cameraForward, bool includeEntities, const VoxelChunkManager &voxelChunkManager,
		const EntityChunkManager &entityChunkManager, const CollisionChunkManager &collisionChunkManager,
		const EntityDefinitionLibrary &entityDefLibrary, RayCastHit &hit);
	bool rayCast(const CoordDouble3 &rayStart, const VoxelDouble3 &rayDirection, const VoxelDouble3 &cameraForward,
		bool includeEntities, const VoxelChunkManager &voxelChunkManager, const EntityChunkManager &entityChunkManager,
		const CollisionChunkManager &collisionChunkManager, const EntityDefinitionLibrary &entityDefLibrary,
		RayCastHit &hit);

	JPH::CompoundShape *getCompoundShapeFromBody(const JPH::Body &body, JPH::PhysicsSystem &physicsSystem);
	JPH::CompoundShape *getCompoundShapeFromBodyID(JPH::BodyID bodyID, JPH::PhysicsSystem &physicsSystem);
	JPH::StaticCompoundShape *getStaticCompoundShapeFromBody(const JPH::Body &body, JPH::PhysicsSystem &physicsSystem);
	JPH::StaticCompoundShape *getStaticCompoundShapeFromBodyID(JPH::BodyID bodyID, JPH::PhysicsSystem &physicsSystem);
};
