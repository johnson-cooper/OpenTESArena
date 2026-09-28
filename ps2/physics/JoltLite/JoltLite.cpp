// JoltLite solver. See JoltLite.h for the rationale.
//
// Model:
// - Static compound bodies hold Y-rotated boxes (voxel walls/doors/sensors from CollisionChunkManager).
// - Every other shape is a vertical capsule, treated as a round vertical cylinder of radius r spanning
//   [center - halfHeight - r, center + halfHeight + r]. This matches how Arena actors collide (upright capsules).
// - Dynamic bodies are integrated with sub-steps and pushed out of penetration (no rotation, no restitution).
// - Kinematic bodies move by velocity without being pushed. Static bodies never move.
// - Sensors never push; overlaps are reported through the ContactListener exactly once per overlap start.

#include <algorithm>
#include <cstdio>

#include "JoltLite.h"

namespace JPH
{
	Factory *Factory::sInstance = nullptr;

	void RegisterDefaultAllocator() { }
	void RegisterTypes() { }
	void UnregisterTypes() { }

	namespace
	{
		constexpr float kMinSeparation = 1.0e-4f;

		struct CapsuleExtents
		{
			float radius;
			float bottom, top; // World Y.
		};

		bool TryGetCapsule(const Body &body, CapsuleExtents *out)
		{
			const Shape *shape = body.GetShape();
			if ((shape == nullptr) || (shape->GetSubType() != EShapeSubType::Capsule))
			{
				return false;
			}

			const CapsuleShape *capsule = static_cast<const CapsuleShape*>(shape);
			const float halfTotal = capsule->mHalfHeightOfCylinder + capsule->mRadius;
			out->radius = capsule->mRadius;
			out->bottom = body.mPosition.y - halfTotal;
			out->top = body.mPosition.y + halfTotal;
			return true;
		}

		// Penetration of a vertical capsule (as cylinder) into a Y-rotated box. Returns false if not overlapping.
		// outNormal points from the box toward the capsule.
		bool CapsuleVsBox(const Vec3 &capsuleCenter, const CapsuleExtents &capsule, const Vec3 &boxCenter, float cosY, float sinY,
			const Vec3 &halfExtent, float stepHeight, Vec3 *outNormal, float *outDepth)
		{
			const float boxBottom = boxCenter.y - halfExtent.y;
			const float boxTop = boxCenter.y + halfExtent.y;
			if ((capsule.top <= boxBottom) || (capsule.bottom >= boxTop))
			{
				return false;
			}

			// Transform capsule center into box-local XZ (inverse Y rotation).
			const float dx = capsuleCenter.x - boxCenter.x;
			const float dz = capsuleCenter.z - boxCenter.z;
			const float lx = (cosY * dx) - (sinY * dz);
			const float lz = (sinY * dx) + (cosY * dz);

			const float qx = std::clamp(lx, -halfExtent.x, halfExtent.x);
			const float qz = std::clamp(lz, -halfExtent.z, halfExtent.z);
			const float ddx = lx - qx;
			const float ddz = lz - qz;
			const float distSq = (ddx * ddx) + (ddz * ddz);
			const float r = capsule.radius;
			if (distSq >= (r * r))
			{
				return false;
			}

			// Horizontal push (in box-local space).
			float localNx, localNz, horizontalDepth;
			if (distSq > 1.0e-12f)
			{
				const float dist = std::sqrt(distSq);
				localNx = ddx / dist;
				localNz = ddz / dist;
				horizontalDepth = r - dist;
			}
			else
			{
				// Center inside the rectangle: push out along the axis of least penetration.
				const float penX = (halfExtent.x - std::fabs(lx)) + r;
				const float penZ = (halfExtent.z - std::fabs(lz)) + r;
				if (penX < penZ)
				{
					localNx = (lx >= 0.0f) ? 1.0f : -1.0f;
					localNz = 0.0f;
					horizontalDepth = penX;
				}
				else
				{
					localNx = 0.0f;
					localNz = (lz >= 0.0f) ? 1.0f : -1.0f;
					horizontalDepth = penZ;
				}
			}

			const float upDepth = boxTop - capsule.bottom; // Push capsule up onto the box.
			const float downDepth = capsule.top - boxBottom; // Push capsule down under the box.

			// Rounded capsule bottom rides up small ledges (and the floor is always "up").
			const bool canStepUp = (upDepth <= stepHeight) || (upDepth <= horizontalDepth);
			if (canStepUp && (upDepth <= downDepth))
			{
				*outNormal = Vec3(0.0f, 1.0f, 0.0f);
				*outDepth = upDepth;
				return true;
			}

			if ((downDepth < horizontalDepth) && (downDepth <= r))
			{
				*outNormal = Vec3(0.0f, -1.0f, 0.0f);
				*outDepth = downDepth;
				return true;
			}

			// Rotate the local horizontal normal back to world space.
			*outNormal = Vec3((cosY * localNx) + (sinY * localNz), 0.0f, (-sinY * localNx) + (cosY * localNz));
			*outDepth = horizontalDepth;
			return true;
		}

		bool CapsuleVsCapsule(const Vec3 &centerA, const CapsuleExtents &a, const Vec3 &centerB, const CapsuleExtents &b,
			Vec3 *outNormal, float *outDepth)
		{
			if ((a.top <= b.bottom) || (a.bottom >= b.top))
			{
				return false;
			}

			const float dx = centerA.x - centerB.x;
			const float dz = centerA.z - centerB.z;
			const float rr = a.radius + b.radius;
			const float distSq = (dx * dx) + (dz * dz);
			if (distSq >= (rr * rr))
			{
				return false;
			}

			const float dist = std::sqrt(distSq);
			if (dist > 1.0e-6f)
			{
				*outNormal = Vec3(dx / dist, 0.0f, dz / dist);
			}
			else
			{
				*outNormal = Vec3(1.0f, 0.0f, 0.0f);
			}

			*outDepth = rr - dist;
			return true;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Shapes
	// ---------------------------------------------------------------------------------------------
	TransformedShape Shape::GetSubShapeTransformedShape(const SubShapeID &subShapeID, Vec3Arg positionCOM, QuatArg rotation,
		Vec3Arg scale, SubShapeID &outRemainder) const
	{
		TransformedShape ts;
		ts.mShapePositionCOM = positionCOM;
		ts.mShapeRotation = rotation;
		ts.mShape = this;
		outRemainder = SubShapeID();
		return ts;
	}

	TransformedShape CompoundShape::GetSubShapeTransformedShape(const SubShapeID &subShapeID, Vec3Arg positionCOM, QuatArg rotation,
		Vec3Arg scale, SubShapeID &outRemainder) const
	{
		const uint32 index = subShapeID.GetValue();
		TransformedShape ts;
		if (index < this->mSubShapes.size())
		{
			const SubShape &sub = this->mSubShapes[index];
			// Compound bodies are always created with identity rotation in OpenTESArena.
			ts.mShapePositionCOM = positionCOM + sub.mPositionCOM;
			ts.mShapeRotation = sub.mRotation;
			ts.mShape = sub.mShape.GetPtr();
		}
		else
		{
			ts.mShapePositionCOM = positionCOM;
			ts.mShapeRotation = rotation;
			ts.mShape = this;
		}

		outRemainder = SubShapeID();
		return ts;
	}

	void StaticCompoundShape::BuildGrid()
	{
		this->mCellStarts.clear();
		this->mCellIndices.clear();
		this->mCellCountX = 0;
		this->mCellCountZ = 0;
		if (this->mBoxes.empty())
		{
			return;
		}

		const AABox &bounds = this->mLocalBounds;
		this->mGridMinX = bounds.mMin.x;
		this->mGridMinZ = bounds.mMin.z;
		const float inv = 1.0f / GRID_CELL_SIZE;
		this->mCellCountX = std::max(1, static_cast<int>(std::ceil((bounds.mMax.x - bounds.mMin.x) * inv)));
		this->mCellCountZ = std::max(1, static_cast<int>(std::ceil((bounds.mMax.z - bounds.mMin.z) * inv)));

		// Clamp absurd grids (shouldn't happen for 64x64 chunks).
		this->mCellCountX = std::min(this->mCellCountX, 64);
		this->mCellCountZ = std::min(this->mCellCountZ, 64);
		const int cellCount = this->mCellCountX * this->mCellCountZ;

		auto forEachCell = [this, inv](const AABox &b, auto &&func)
		{
			const int x0 = std::clamp(static_cast<int>(std::floor((b.mMin.x - this->mGridMinX) * inv)), 0, this->mCellCountX - 1);
			const int z0 = std::clamp(static_cast<int>(std::floor((b.mMin.z - this->mGridMinZ) * inv)), 0, this->mCellCountZ - 1);
			const int x1 = std::clamp(static_cast<int>(std::floor((b.mMax.x - this->mGridMinX) * inv)), 0, this->mCellCountX - 1);
			const int z1 = std::clamp(static_cast<int>(std::floor((b.mMax.z - this->mGridMinZ) * inv)), 0, this->mCellCountZ - 1);
			for (int z = z0; z <= z1; z++)
			{
				for (int x = x0; x <= x1; x++)
				{
					func(x + (z * this->mCellCountX));
				}
			}
		};

		// Counting sort into cells (two passes, no per-cell vectors).
		std::vector<uint16> counts(cellCount + 1, 0);
		for (const CompoundSubBox &box : this->mBoxes)
		{
			forEachCell(box.mBounds, [&counts](int cell) { counts[cell]++; });
		}

		this->mCellStarts.resize(cellCount + 1);
		uint32 running = 0;
		for (int i = 0; i < cellCount; i++)
		{
			this->mCellStarts[i] = static_cast<uint16>(running);
			running += counts[i];
		}

		this->mCellStarts[cellCount] = static_cast<uint16>(running);
		this->mCellIndices.resize(running);

		std::vector<uint16> cursor(this->mCellStarts.begin(), this->mCellStarts.end() - 1);
		for (size_t boxIndex = 0; boxIndex < this->mBoxes.size(); boxIndex++)
		{
			forEachCell(this->mBoxes[boxIndex].mBounds, [this, &cursor, boxIndex](int cell)
			{
				this->mCellIndices[cursor[cell]++] = static_cast<uint16>(boxIndex);
			});
		}
	}

	ShapeSettings::ShapeResult BoxShapeSettings::Create() const
	{
		ShapeResult result;
		result.Set(new BoxShape(this->mHalfExtent, this->mConvexRadius));
		return result;
	}

	ShapeSettings::ShapeResult CapsuleShapeSettings::Create() const
	{
		ShapeResult result;
		if ((this->mRadius <= 0.0f) || (this->mHalfHeightOfCylinder < 0.0f))
		{
			result.SetError("Invalid capsule dimensions.");
			return result;
		}

		result.Set(new CapsuleShape(this->mHalfHeightOfCylinder, this->mRadius));
		return result;
	}

	ShapeSettings::ShapeResult StaticCompoundShapeSettings::Create() const
	{
		ShapeResult result;
		if (this->mSubShapes.empty())
		{
			result.SetError("Compound shape needs at least one sub-shape.");
			return result;
		}

		if (this->mSubShapes.size() > 0xFFFF)
		{
			result.SetError("Too many sub-shapes for JoltLite compound.");
			return result;
		}

		StaticCompoundShape *compound = new StaticCompoundShape();
		compound->mSubShapes.reserve(this->mSubShapes.size());
		compound->mBoxes.reserve(this->mSubShapes.size());

		for (const SubShapeSettings &subSettings : this->mSubShapes)
		{
			ShapeResult childResult = subSettings.mShape->Create();
			if (childResult.HasError())
			{
				delete compound;
				return childResult;
			}

			const Shape *child = childResult.Get().GetPtr();
			if (child->GetSubType() != EShapeSubType::Box)
			{
				delete compound;
				result.SetError("JoltLite compounds only support box children.");
				return result;
			}

			const BoxShape *box = static_cast<const BoxShape*>(child);
			const float angle = subSettings.mRotation.GetYAngle();

			CompoundSubBox subBox;
			subBox.mPosition = subSettings.mPosition;
			subBox.mCosY = std::cos(angle);
			subBox.mSinY = std::sin(angle);
			subBox.mHalfExtent = box->mHalfExtent;
			const float ex = (std::fabs(subBox.mCosY) * box->mHalfExtent.x) + (std::fabs(subBox.mSinY) * box->mHalfExtent.z);
			const float ez = (std::fabs(subBox.mSinY) * box->mHalfExtent.x) + (std::fabs(subBox.mCosY) * box->mHalfExtent.z);
			const Vec3 e(ex, box->mHalfExtent.y, ez);
			subBox.mBounds = AABox(subBox.mPosition - e, subBox.mPosition + e);
			subBox.mShape = child;

			compound->mLocalBounds.Encapsulate(subBox.mBounds.mMin);
			compound->mLocalBounds.Encapsulate(subBox.mBounds.mMax);

			CompoundShape::SubShape sub;
			sub.mPositionCOM = subSettings.mPosition;
			sub.mRotation = subSettings.mRotation;
			sub.mShape = child;
			compound->mSubShapes.emplace_back(std::move(sub));
			compound->mBoxes.emplace_back(std::move(subBox));
		}

		compound->BuildGrid();
		result.Set(compound);
		return result;
	}

	const Shape *BodyCreationSettings::GetShape() const
	{
		if (this->mShapePtr.GetPtr() != nullptr)
		{
			return this->mShapePtr.GetPtr();
		}

		return nullptr;
	}

	void Body::UpdateWorldBounds()
	{
		const Shape *shape = this->mShape.GetPtr();
		if (shape == nullptr)
		{
			this->mWorldBounds = AABox(this->mPosition, this->mPosition);
			return;
		}

		const AABox local = shape->GetLocalBounds();
		this->mWorldBounds = AABox(local.mMin + this->mPosition, local.mMax + this->mPosition);
	}

	// ---------------------------------------------------------------------------------------------
	// Body interface
	// ---------------------------------------------------------------------------------------------
	Body *BodyLockInterface::TryGetBody(const BodyID &bodyID) const
	{
		return (this->mSystem != nullptr) ? this->mSystem->getBody(bodyID) : nullptr;
	}

	Body *BodyInterface::CreateBody(const BodyCreationSettings &settings)
	{
		PhysicsSystem &sys = *this->mSystem;
		if (sys.mFreeIndices.empty())
		{
			std::printf("[JoltLite] Body pool exhausted (%u bodies).\n", static_cast<unsigned>(sys.mBodies.size()));
			return nullptr;
		}

		RefConst<Shape> shape;
		if (settings.mShapePtr.GetPtr() != nullptr)
		{
			shape = settings.mShapePtr;
		}
		else if (settings.mShapeSettings.GetPtr() != nullptr)
		{
			ShapeSettings::ShapeResult result = settings.mShapeSettings->Create();
			if (result.HasError())
			{
				std::printf("[JoltLite] Shape creation failed: %s\n", result.GetError().c_str());
				return nullptr;
			}

			shape = result.Get();
		}
		else
		{
			return nullptr;
		}

		const uint32 index = sys.mFreeIndices.back();
		sys.mFreeIndices.pop_back();

		Body &body = sys.mBodies[index];
		const uint8 nextSequence = static_cast<uint8>((body.mID.IsInvalid() ? 0 : body.mID.GetSequenceNumber()) + 1);
		body = Body();
		body.mID = BodyID(index, nextSequence);
		body.mShape = shape;
		body.mPosition = settings.mPosition;
		body.mRotation = settings.mRotation;
		body.mMotionType = settings.mMotionType;
		body.mObjectLayer = settings.mObjectLayer;
		body.mAllowedDOFs = settings.mAllowedDOFs;
		body.mGravityFactor = settings.mGravityFactor;
		body.mLinearDamping = settings.mLinearDamping;
		body.mFriction = settings.mFriction;
		body.mUserData = settings.mUserData;
		body.mIsSensor = settings.mIsSensor;
		body.mAllowSleeping = settings.mAllowSleeping;
		body.mInUse = true;
		body.mInSystem = false;
		body.UpdateWorldBounds();

		if (shape->GetSubType() == EShapeSubType::StaticCompound)
		{
			sys.mStats.compoundBoxCount += static_cast<int>(static_cast<const StaticCompoundShape*>(shape.GetPtr())->mBoxes.size());
		}

		sys.mStats.bodyCount++;
		return &body;
	}

	BodyID BodyInterface::CreateAndAddBody(const BodyCreationSettings &settings, EActivation activation)
	{
		Body *body = this->CreateBody(settings);
		if (body == nullptr)
		{
			return BodyID();
		}

		this->AddBody(body->mID, activation);
		return body->mID;
	}

	void BodyInterface::AddBody(const BodyID &bodyID, EActivation activation)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if ((body == nullptr) || body->mInSystem)
		{
			return;
		}

		body->mInSystem = true;
		this->mSystem->mActiveList.push_back(bodyID.GetIndex());
	}

	void BodyInterface::RemoveBody(const BodyID &bodyID)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if ((body == nullptr) || !body->mInSystem)
		{
			return;
		}

		body->mInSystem = false;
		std::vector<uint32> &active = this->mSystem->mActiveList;
		const uint32 index = bodyID.GetIndex();
		for (size_t i = 0; i < active.size(); i++)
		{
			if (active[i] == index)
			{
				active[i] = active.back();
				active.pop_back();
				break;
			}
		}
	}

	void BodyInterface::DestroyBody(const BodyID &bodyID)
	{
		PhysicsSystem &sys = *this->mSystem;
		Body *body = sys.getBody(bodyID);
		if (body == nullptr)
		{
			return;
		}

		if (body->mInSystem)
		{
			this->RemoveBody(bodyID);
		}

		const Shape *shape = body->mShape.GetPtr();
		if ((shape != nullptr) && (shape->GetSubType() == EShapeSubType::StaticCompound))
		{
			sys.mStats.compoundBoxCount -= static_cast<int>(static_cast<const StaticCompoundShape*>(shape)->mBoxes.size());
		}

		const BodyID oldID = body->mID;
		body->mShape = nullptr;
		body->mInUse = false;
		body->mID = oldID; // Keep sequence so stale IDs are rejected.
		sys.mFreeIndices.push_back(bodyID.GetIndex());
		sys.mStats.bodyCount--;
	}

	bool BodyInterface::IsAdded(const BodyID &bodyID) const
	{
		const Body *body = this->mSystem->getBody(bodyID);
		return (body != nullptr) && body->mInSystem;
	}

	void BodyInterface::SetLinearVelocity(const BodyID &bodyID, Vec3Arg velocity)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if ((body != nullptr) && !body->IsStatic())
		{
			body->mLinearVelocity = velocity;
		}
	}

	Vec3 BodyInterface::GetLinearVelocity(const BodyID &bodyID) const
	{
		const Body *body = this->mSystem->getBody(bodyID);
		return (body != nullptr) ? body->mLinearVelocity : Vec3::sZero();
	}

	RVec3 BodyInterface::GetPosition(const BodyID &bodyID) const
	{
		const Body *body = this->mSystem->getBody(bodyID);
		return (body != nullptr) ? body->mPosition : RVec3::sZero();
	}

	RVec3 BodyInterface::GetCenterOfMassPosition(const BodyID &bodyID) const
	{
		return this->GetPosition(bodyID);
	}

	void BodyInterface::SetPosition(const BodyID &bodyID, RVec3Arg position, EActivation activation)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if (body != nullptr)
		{
			body->mPosition = position;
			body->UpdateWorldBounds();
		}
	}

	RefConst<Shape> BodyInterface::GetShape(const BodyID &bodyID) const
	{
		const Body *body = this->mSystem->getBody(bodyID);
		return (body != nullptr) ? body->mShape : RefConst<Shape>();
	}

	void BodyInterface::SetObjectLayer(const BodyID &bodyID, ObjectLayer layer)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if (body != nullptr)
		{
			body->mObjectLayer = layer;
		}
	}

	ObjectLayer BodyInterface::GetObjectLayer(const BodyID &bodyID) const
	{
		const Body *body = this->mSystem->getBody(bodyID);
		return (body != nullptr) ? body->mObjectLayer : ObjectLayer(0);
	}

	void BodyInterface::SetMotionType(const BodyID &bodyID, EMotionType motionType, EActivation activation)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if (body != nullptr)
		{
			body->mMotionType = motionType;
			if (motionType == EMotionType::Static)
			{
				body->mLinearVelocity = Vec3::sZero();
			}
		}
	}

	void BodyInterface::SetGravityFactor(const BodyID &bodyID, float gravityFactor)
	{
		Body *body = this->mSystem->getBody(bodyID);
		if (body != nullptr)
		{
			body->mGravityFactor = gravityFactor;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Physics system
	// ---------------------------------------------------------------------------------------------
	PhysicsSystem::PhysicsSystem()
	{
		this->mBodyInterface.Init(this);
		this->mBodyLockInterface.Init(this);
	}

	Body *PhysicsSystem::getBody(const BodyID &id)
	{
		if (id.IsInvalid())
		{
			return nullptr;
		}

		const uint32 index = id.GetIndex();
		if (index >= this->mBodies.size())
		{
			return nullptr;
		}

		Body &body = this->mBodies[index];
		return (body.mInUse && (body.mID == id)) ? &body : nullptr;
	}

	const Body *PhysicsSystem::getBody(const BodyID &id) const
	{
		return const_cast<PhysicsSystem*>(this)->getBody(id);
	}

	void PhysicsSystem::Init(uint maxBodies, uint numBodyMutexes, uint maxBodyPairs, uint maxContactConstraints,
		const BroadPhaseLayerInterface &broadPhaseLayerInterface, const ObjectVsBroadPhaseLayerFilter &objectVsBroadPhaseLayerFilter,
		const ObjectLayerPairFilter &objectLayerPairFilter)
	{
		// Note: the filters are owned by the caller (Game::loop() stack) and outlive the simulation.
		this->mObjectLayerPairFilter = &objectLayerPairFilter;

		this->mBodies.clear();
		this->mBodies.shrink_to_fit();
		this->mBodies.resize(maxBodies);
		this->mFreeIndices.clear();
		this->mFreeIndices.reserve(maxBodies);
		for (uint i = maxBodies; i > 0; i--)
		{
			this->mFreeIndices.push_back(i - 1);
		}

		this->mActiveList.clear();
		this->mActiveList.reserve(maxBodies);

		this->mMaxContacts = std::max<uint>(64, maxContactConstraints);
		this->mPrevContacts.clear();
		this->mCurContacts.clear();
		this->mPrevContacts.reserve(this->mMaxContacts);
		this->mCurContacts.reserve(this->mMaxContacts);

		this->mStats = PhysicsStats();
		this->mStats.maxBodyCount = static_cast<int>(maxBodies);

		std::printf("[JoltLite] Init: %u bodies (%u KiB), %u tracked contacts.\n", maxBodies,
			static_cast<unsigned>((maxBodies * sizeof(Body)) / 1024), static_cast<unsigned>(this->mMaxContacts));
	}

	uint PhysicsSystem::GetNumBodies() const
	{
		return static_cast<uint>(this->mStats.bodyCount);
	}

	void PhysicsSystem::registerCharacter(Character *character)
	{
		this->mCharacters.push_back(character);
	}

	void PhysicsSystem::unregisterCharacter(Character *character)
	{
		this->mCharacters.erase(std::remove(this->mCharacters.begin(), this->mCharacters.end(), character), this->mCharacters.end());
	}

	void PhysicsSystem::recordContact(const Body &a, const Body &b, SubShapeID subShapeB, const Vec3 &normal, float depth)
	{
		ContactKey key;
		key.bodyA = a.mID.GetIndexAndSequenceNumber();
		key.bodyB = b.mID.GetIndexAndSequenceNumber();
		key.subShapeB = subShapeB.GetValue();

		for (const ContactKey &existing : this->mCurContacts)
		{
			if (existing == key)
			{
				return;
			}
		}

		if (this->mCurContacts.size() >= this->mMaxContacts)
		{
			return; // Bounded: drop extra contacts rather than grow.
		}

		this->mCurContacts.push_back(key);
	}

	void PhysicsSystem::flushContacts()
	{
		int added = 0;
		if (this->mContactListener != nullptr)
		{
			for (const ContactKey &key : this->mCurContacts)
			{
				const bool existedBefore = std::find(this->mPrevContacts.begin(), this->mPrevContacts.end(), key) != this->mPrevContacts.end();
				const Body *a = this->getBody(BodyID(key.bodyA));
				const Body *b = this->getBody(BodyID(key.bodyB));
				if ((a == nullptr) || (b == nullptr))
				{
					continue;
				}

				ContactManifold manifold;
				manifold.mBaseOffset = a->mPosition;
				manifold.mSubShapeID1 = SubShapeID();
				manifold.mSubShapeID2 = SubShapeID(key.subShapeB);
				ContactSettings settings;
				settings.mIsSensor = a->mIsSensor || b->mIsSensor;

				if (!existedBefore)
				{
					this->mContactListener->OnContactAdded(*a, *b, manifold, settings);
					added++;
				}
				else
				{
					this->mContactListener->OnContactPersisted(*a, *b, manifold, settings);
				}
			}

			for (const ContactKey &key : this->mPrevContacts)
			{
				const bool stillExists = std::find(this->mCurContacts.begin(), this->mCurContacts.end(), key) != this->mCurContacts.end();
				if (!stillExists)
				{
					this->mContactListener->OnContactRemoved(SubShapeIDPair(BodyID(key.bodyA), SubShapeID(), BodyID(key.bodyB), SubShapeID(key.subShapeB)));
				}
			}
		}

		this->mStats.contactsAddedLastUpdate = added;
		this->mStats.trackedContactCount = static_cast<int>(this->mCurContacts.size());
		std::swap(this->mPrevContacts, this->mCurContacts);
		this->mCurContacts.clear();
	}

	void PhysicsSystem::resolveMovingBody(Body &body, float dt, bool isCharacter, Character *character)
	{
		CapsuleExtents self;
		if (!TryGetCapsule(body, &self))
		{
			return;
		}

		const bool canMoveY = (static_cast<uint8>(body.mAllowedDOFs & EAllowedDOFs::TranslationY) != 0);
		const float stepHeight = (character != nullptr) ? character->GetStepHeight() : (self.radius * 0.25f);
		const bool isSensor = body.mIsSensor;

		constexpr int kMaxIterations = 4;
		for (int iteration = 0; iteration < kMaxIterations; iteration++)
		{
			bool anyPush = false;
			TryGetCapsule(body, &self);
			const AABox selfBounds(Vec3(body.mPosition.x - self.radius, self.bottom, body.mPosition.z - self.radius),
				Vec3(body.mPosition.x + self.radius, self.top, body.mPosition.z + self.radius));

			for (const uint32 otherIndex : this->mActiveList)
			{
				Body &other = this->mBodies[otherIndex];
				if ((&other == &body) || !other.mInUse)
				{
					continue;
				}

				if ((this->mObjectLayerPairFilter != nullptr) && !this->mObjectLayerPairFilter->ShouldCollide(body.mObjectLayer, other.mObjectLayer))
				{
					continue;
				}

				if (!selfBounds.Overlaps(other.mWorldBounds))
				{
					continue;
				}

				const bool pairIsSensor = isSensor || other.mIsSensor;
				const Shape *otherShape = other.GetShape();
				if (otherShape == nullptr)
				{
					continue;
				}

				if (otherShape->GetSubType() == EShapeSubType::StaticCompound)
				{
					const StaticCompoundShape &compound = *static_cast<const StaticCompoundShape*>(otherShape);
					const Vec3 localMin = selfBounds.mMin - other.mPosition;
					const Vec3 localMax = selfBounds.mMax - other.mPosition;
					compound.ForEachBoxInRange(localMin.x, localMin.z, localMax.x, localMax.z,
						[&](int boxIndex)
					{
						this->mStats.boxTestsLastUpdate++;
						const CompoundSubBox &box = compound.mBoxes[boxIndex];
						if ((box.mBounds.mMax.y + other.mPosition.y) <= self.bottom || (box.mBounds.mMin.y + other.mPosition.y) >= self.top)
						{
							return;
						}

						Vec3 normal;
						float depth;
						TryGetCapsule(body, &self);
						if (!CapsuleVsBox(body.mPosition, self, box.mPosition + other.mPosition, box.mCosY, box.mSinY, box.mHalfExtent,
							stepHeight, &normal, &depth))
						{
							return;
						}

						this->recordContact(body, other, SubShapeID(static_cast<uint32>(boxIndex)), normal, depth);
						if (pairIsSensor || (depth <= kMinSeparation))
						{
							return;
						}

						if (!canMoveY && (normal.y != 0.0f))
						{
							return;
						}

						body.mPosition += normal * depth;
						const float vn = body.mLinearVelocity.Dot(normal);
						if (vn < 0.0f)
						{
							body.mLinearVelocity -= normal * vn;
						}

						if (character != nullptr)
						{
							character->notifyContact(normal, other.mID);
						}

						anyPush = true;
					});
				}
				else
				{
					CapsuleExtents otherCapsule;
					if (!TryGetCapsule(other, &otherCapsule))
					{
						continue;
					}

					this->mStats.pairTestsLastUpdate++;
					TryGetCapsule(body, &self);
					Vec3 normal;
					float depth;
					if (!CapsuleVsCapsule(body.mPosition, self, other.mPosition, otherCapsule, &normal, &depth))
					{
						continue;
					}

					this->recordContact(body, other, SubShapeID(), normal, depth);
					if (pairIsSensor || (depth <= kMinSeparation))
					{
						continue;
					}

					if (other.IsDynamic())
					{
						// Split the push between two dynamic bodies.
						const float half = depth * 0.5f;
						body.mPosition += normal * half;
						other.mPosition -= normal * half;
						other.UpdateWorldBounds();
					}
					else
					{
						body.mPosition += normal * depth;
					}

					const float vn = body.mLinearVelocity.Dot(normal);
					if (vn < 0.0f)
					{
						body.mLinearVelocity -= normal * vn;
					}

					anyPush = true;
				}
			}

			body.UpdateWorldBounds();
			if (!anyPush)
			{
				break;
			}
		}
	}

	void PhysicsSystem::stepBodies(float dt)
	{
		// Characters (and anything else dynamic) are resolved after integration. Kinematic bodies simply move.
		for (const uint32 index : this->mActiveList)
		{
			Body &body = this->mBodies[index];
			if (!body.mInUse || body.IsStatic())
			{
				continue;
			}

			if (body.IsDynamic())
			{
				body.mLinearVelocity += this->mGravity * (body.mGravityFactor * dt);
				if (body.mLinearDamping > 0.0f)
				{
					body.mLinearVelocity *= std::max(0.0f, 1.0f - (body.mLinearDamping * dt));
				}

				const uint8 dofs = static_cast<uint8>(body.mAllowedDOFs);
				if ((dofs & static_cast<uint8>(EAllowedDOFs::TranslationX)) == 0) body.mLinearVelocity.x = 0.0f;
				if ((dofs & static_cast<uint8>(EAllowedDOFs::TranslationY)) == 0) body.mLinearVelocity.y = 0.0f;
				if ((dofs & static_cast<uint8>(EAllowedDOFs::TranslationZ)) == 0) body.mLinearVelocity.z = 0.0f;
			}

			body.mPosition += body.mLinearVelocity * dt;
			body.UpdateWorldBounds();
		}

		for (const uint32 index : this->mActiveList)
		{
			Body &body = this->mBodies[index];
			if (!body.mInUse || !body.IsDynamic())
			{
				continue;
			}

			Character *character = nullptr;
			for (Character *c : this->mCharacters)
			{
				if (c->mBodyID == body.mID)
				{
					character = c;
					break;
				}
			}

			this->resolveMovingBody(body, dt, character != nullptr, character);
		}
	}

	EPhysicsUpdateError PhysicsSystem::Update(float deltaTime, int collisionSteps, TempAllocator *tempAllocator, JobSystem *jobSystem)
	{
		static_cast<void>(tempAllocator);
		static_cast<void>(jobSystem);

		this->mStats.boxTestsLastUpdate = 0;
		this->mStats.pairTestsLastUpdate = 0;

		int activeDynamic = 0;
		for (const uint32 index : this->mActiveList)
		{
			if (this->mBodies[index].IsDynamic())
			{
				activeDynamic++;
			}
		}

		this->mStats.activeDynamicCount = activeDynamic;

		for (Character *c : this->mCharacters)
		{
			c->mSupportedThisUpdate = false;
		}

		if ((deltaTime > 0.0f) && (activeDynamic > 0))
		{
			const int steps = std::clamp(collisionSteps, 1, 8);
			const float stepDt = deltaTime / static_cast<float>(steps);
			for (int i = 0; i < steps; i++)
			{
				this->stepBodies(stepDt);
			}
		}

		this->flushContacts();
		return EPhysicsUpdateError::None;
	}

	// ---------------------------------------------------------------------------------------------
	// Characters
	// ---------------------------------------------------------------------------------------------
	Character::Character(const CharacterSettings *settings, RVec3Arg position, QuatArg rotation, uint64 userData, PhysicsSystem *system)
		: mSystem(system), mShape(settings->mShape), mUp(settings->mUp)
	{
		this->mCosMaxSlopeAngle = std::cos(settings->mMaxSlopeAngle);

		float radius = 0.2f;
		if ((this->mShape.GetPtr() != nullptr) && (this->mShape->GetSubType() == EShapeSubType::Capsule))
		{
			radius = static_cast<const CapsuleShape*>(this->mShape.GetPtr())->mRadius;
		}

		// A capsule's rounded bottom lets it ride onto ledges up to roughly half its radius.
		this->mStepHeight = radius * 0.5f;

		BodyCreationSettings bodySettings(this->mShape.GetPtr(), position, rotation, EMotionType::Dynamic, settings->mLayer);
		bodySettings.mFriction = settings->mFriction;
		bodySettings.mGravityFactor = settings->mGravityFactor;
		bodySettings.mAllowedDOFs = settings->mAllowedDOFs;
		bodySettings.mLinearDamping = 0.0f;
		bodySettings.mUserData = userData;

		Body *body = system->GetBodyInterface().CreateBody(bodySettings);
		if (body != nullptr)
		{
			this->mBodyID = body->mID;
		}

		system->registerCharacter(this);
	}

	Character::~Character()
	{
		if (this->mSystem != nullptr)
		{
			this->mSystem->unregisterCharacter(this);
			BodyInterface &bodyInterface = this->mSystem->GetBodyInterface();
			if (!this->mBodyID.IsInvalid())
			{
				bodyInterface.RemoveBody(this->mBodyID);
				bodyInterface.DestroyBody(this->mBodyID);
			}
		}
	}

	void Character::AddToPhysicsSystem(EActivation activation, bool lockBodies)
	{
		this->mSystem->GetBodyInterface().AddBody(this->mBodyID, activation);
	}

	void Character::RemoveFromPhysicsSystem(bool lockBodies)
	{
		this->mSystem->GetBodyInterface().RemoveBody(this->mBodyID);
	}

	RVec3 Character::GetPosition(bool lockBodies) const
	{
		return this->mSystem->GetBodyInterface().GetPosition(this->mBodyID);
	}

	void Character::SetPosition(RVec3Arg position, EActivation activation, bool lockBodies)
	{
		this->mSystem->GetBodyInterface().SetPosition(this->mBodyID, position, activation);
	}

	Vec3 Character::GetLinearVelocity(bool lockBodies) const
	{
		return this->mSystem->GetBodyInterface().GetLinearVelocity(this->mBodyID);
	}

	void Character::SetLinearVelocity(Vec3Arg velocity, bool lockBodies)
	{
		this->mSystem->GetBodyInterface().SetLinearVelocity(this->mBodyID, velocity);
	}

	void Character::notifyContact(const Vec3 &normal, const BodyID &otherBody)
	{
		if (normal.Dot(this->mUp) >= this->mCosMaxSlopeAngle)
		{
			this->mSupportedThisUpdate = true;
			this->mGroundNormal = normal;
			this->mGroundBodyID = otherBody;
		}
	}

	void Character::PostSimulation(float maxSeparationDistance, bool lockBodies)
	{
		// Probe slightly below the feet for supporting (non-sensor) geometry, like Jolt's post-simulation collide.
		Body *body = this->mSystem->getBody(this->mBodyID);
		const Vec3 solverGroundNormal = this->mGroundNormal;
		const BodyID solverGroundBodyID = this->mGroundBodyID;
		this->mGroundState = EGroundState::InAir;
		this->mGroundNormal = Vec3::sZero();
		this->mGroundBodyID = BodyID();
		if ((body == nullptr) || !body->mInSystem)
		{
			return;
		}

		CapsuleExtents self;
		if (!TryGetCapsule(*body, &self))
		{
			return;
		}

		const float probeDistance = std::max(maxSeparationDistance, 0.02f);
		CapsuleExtents probe = self;
		probe.bottom -= probeDistance;
		const Vec3 probeCenter = body->mPosition - Vec3(0.0f, probeDistance * 0.5f, 0.0f);
		const AABox probeBounds(Vec3(body->mPosition.x - self.radius, probe.bottom, body->mPosition.z - self.radius),
			Vec3(body->mPosition.x + self.radius, self.top, body->mPosition.z + self.radius));

		const ObjectLayerPairFilter *filter = this->mSystem->mObjectLayerPairFilter;
		for (const uint32 otherIndex : this->mSystem->mActiveList)
		{
			const Body &other = this->mSystem->mBodies[otherIndex];
			if ((&other == body) || !other.mInUse || other.mIsSensor)
			{
				continue;
			}

			if ((filter != nullptr) && !filter->ShouldCollide(body->mObjectLayer, other.mObjectLayer))
			{
				continue;
			}

			if (!probeBounds.Overlaps(other.mWorldBounds))
			{
				continue;
			}

			const Shape *otherShape = other.GetShape();
			if ((otherShape == nullptr) || (otherShape->GetSubType() != EShapeSubType::StaticCompound))
			{
				continue;
			}

			const StaticCompoundShape &compound = *static_cast<const StaticCompoundShape*>(otherShape);
			const Vec3 localMin = probeBounds.mMin - other.mPosition;
			const Vec3 localMax = probeBounds.mMax - other.mPosition;
			bool found = false;
			compound.ForEachBoxInRange(localMin.x, localMin.z, localMax.x, localMax.z, [&](int boxIndex)
			{
				if (found)
				{
					return;
				}

				const CompoundSubBox &box = compound.mBoxes[boxIndex];
				Vec3 normal;
				float depth;
				if (CapsuleVsBox(probeCenter, probe, box.mPosition + other.mPosition, box.mCosY, box.mSinY, box.mHalfExtent,
					probeDistance + this->mStepHeight, &normal, &depth))
				{
					if (normal.Dot(this->mUp) >= this->mCosMaxSlopeAngle)
					{
						found = true;
						this->mGroundNormal = normal;
						this->mGroundBodyID = other.mID;
					}
				}
			});

			if (found)
			{
				this->mGroundState = EGroundState::OnGround;
				return;
			}
		}

		if (this->mSupportedThisUpdate)
		{
			// Supported by something the probe doesn't cover (e.g. standing on a static entity capsule).
			this->mGroundState = EGroundState::OnGround;
			this->mGroundNormal = solverGroundNormal;
			this->mGroundBodyID = solverGroundBodyID;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// CharacterVirtual
	// ---------------------------------------------------------------------------------------------
	CharacterVirtual::CharacterVirtual(const CharacterVirtualSettings *settings, RVec3Arg position, QuatArg rotation, uint64 userData, PhysicsSystem *system)
		: mPosition(position), mShape(settings->mShape)
	{
		static_cast<void>(rotation);
		static_cast<void>(userData);
		static_cast<void>(system);
	}

	void CharacterVsCharacterCollisionSimple::Remove(const CharacterVirtual *character)
	{
		this->mCharacters.erase(std::remove(this->mCharacters.begin(), this->mCharacters.end(), character), this->mCharacters.end());
	}
}
