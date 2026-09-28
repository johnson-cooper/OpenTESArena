#pragma once

// JoltLite: PlayStation 2 physics backend for OpenTESArena.
//
// This is NOT Jolt Physics. It is a small, fixed-capacity collision/character system that implements only the
// subset of the JPH:: API surface OpenTESArena's engine code uses, so the game logic (Player, EntityChunkManager,
// CollisionChunkManager, PhysicsContactListener, ...) compiles unchanged for both desktop (real Jolt) and PS2.
//
// Why: real Jolt's desktop configuration (64 MiB temp allocator, 250k bodies, job system threads) exceeds the
// PS2's entire 32 MiB of EE RAM, and Jolt has no MIPS R5900 port. Arena's world is axis-aligned voxel boxes
// (plus Y-rotated diagonal walls) and vertical capsules, so a simple swept capsule vs oriented-box resolver with a
// coarse per-chunk grid is sufficient for: player/world collision, floor detection, step-up, gravity, jumping,
// entity collision, projectiles, sensor triggers and door colliders. Ray selection already uses the engine's own
// DDA voxel ray caster (Collision/Physics.cpp) and doesn't touch this backend.
//
// Everything is single precision (Jolt without JPH_DOUBLE_PRECISION is also single precision, so world-space
// precision matches desktop).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define JPH_NAMESPACE_BEGIN namespace JPH {
#define JPH_NAMESPACE_END }

namespace JPH
{
	using uint = unsigned int;
	using uint8 = uint8_t;
	using uint16 = uint16_t;
	using uint32 = uint32_t;
	using uint64 = uint64_t;

	// ---------------------------------------------------------------------------------------------
	// Math
	// ---------------------------------------------------------------------------------------------
	class Vec3
	{
	public:
		float x, y, z;

		constexpr Vec3() : x(0.0f), y(0.0f), z(0.0f) { }
		constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) { }

		static constexpr Vec3 sZero() { return Vec3(0.0f, 0.0f, 0.0f); }
		static constexpr Vec3 sReplicate(float v) { return Vec3(v, v, v); }
		static constexpr Vec3 sAxisX() { return Vec3(1.0f, 0.0f, 0.0f); }
		static constexpr Vec3 sAxisY() { return Vec3(0.0f, 1.0f, 0.0f); }
		static constexpr Vec3 sAxisZ() { return Vec3(0.0f, 0.0f, 1.0f); }

		float GetX() const { return this->x; }
		float GetY() const { return this->y; }
		float GetZ() const { return this->z; }
		void SetX(float v) { this->x = v; }
		void SetY(float v) { this->y = v; }
		void SetZ(float v) { this->z = v; }

		float Dot(const Vec3 &o) const { return (this->x * o.x) + (this->y * o.y) + (this->z * o.z); }
		Vec3 Cross(const Vec3 &o) const { return Vec3((this->y * o.z) - (this->z * o.y), (this->z * o.x) - (this->x * o.z), (this->x * o.y) - (this->y * o.x)); }
		float LengthSq() const { return this->Dot(*this); }
		float Length() const { return std::sqrt(this->LengthSq()); }
		Vec3 Normalized() const { const float len = this->Length(); return (len > 0.0f) ? Vec3(this->x / len, this->y / len, this->z / len) : Vec3(); }
		bool IsNearZero(float maxDistSq = 1.0e-12f) const { return this->LengthSq() <= maxDistSq; }

		Vec3 operator+(const Vec3 &o) const { return Vec3(this->x + o.x, this->y + o.y, this->z + o.z); }
		Vec3 operator-(const Vec3 &o) const { return Vec3(this->x - o.x, this->y - o.y, this->z - o.z); }
		Vec3 operator*(float s) const { return Vec3(this->x * s, this->y * s, this->z * s); }
		Vec3 operator/(float s) const { return Vec3(this->x / s, this->y / s, this->z / s); }
		Vec3 operator-() const { return Vec3(-this->x, -this->y, -this->z); }
		Vec3 &operator+=(const Vec3 &o) { this->x += o.x; this->y += o.y; this->z += o.z; return *this; }
		Vec3 &operator-=(const Vec3 &o) { this->x -= o.x; this->y -= o.y; this->z -= o.z; return *this; }
		Vec3 &operator*=(float s) { this->x *= s; this->y *= s; this->z *= s; return *this; }
		bool operator==(const Vec3 &o) const { return (this->x == o.x) && (this->y == o.y) && (this->z == o.z); }
		bool operator!=(const Vec3 &o) const { return !(*this == o); }
	};

	inline Vec3 operator*(float s, const Vec3 &v) { return v * s; }

	using Vec3Arg = const Vec3;
	using RVec3 = Vec3;
	using RVec3Arg = const Vec3;
	using Real = float;

	// Only Y-axis rotations are meaningful in Arena, so rotations are stored as a Y angle plus a generic quaternion
	// for API compatibility.
	class Quat
	{
	public:
		float x, y, z, w;

		constexpr Quat() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) { }
		constexpr Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) { }

		static constexpr Quat sIdentity() { return Quat(); }
		static Quat sRotation(const Vec3 &axis, float angle)
		{
			const float halfAngle = angle * 0.5f;
			const float s = std::sin(halfAngle);
			return Quat(axis.x * s, axis.y * s, axis.z * s, std::cos(halfAngle));
		}

		// Returns the rotation angle around +Y (valid for Y-axis quaternions, which is all Arena uses).
		float GetYAngle() const { return 2.0f * std::atan2(this->y, this->w); }

		bool IsIdentityY() const { return std::fabs(this->y) < 1.0e-6f; }
	};

	using QuatArg = const Quat;

	class Color
	{
	public:
		uint8 r, g, b, a;
		constexpr Color() : r(0), g(0), b(0), a(255) { }
		constexpr Color(uint8 r, uint8 g, uint8 b, uint8 a = 255) : r(r), g(g), b(b), a(a) { }
	};

	using ColorArg = Color;

	class AABox
	{
	public:
		Vec3 mMin, mMax;

		AABox() : mMin(1.0e30f, 1.0e30f, 1.0e30f), mMax(-1.0e30f, -1.0e30f, -1.0e30f) { }
		AABox(const Vec3 &min, const Vec3 &max) : mMin(min), mMax(max) { }

		Vec3 GetSize() const { return this->mMax - this->mMin; }
		Vec3 GetCenter() const { return (this->mMin + this->mMax) * 0.5f; }
		Vec3 GetExtent() const { return this->GetSize() * 0.5f; }
		bool IsValid() const { return (this->mMin.x <= this->mMax.x) && (this->mMin.y <= this->mMax.y) && (this->mMin.z <= this->mMax.z); }
		void Encapsulate(const Vec3 &p)
		{
			this->mMin = Vec3(std::fmin(this->mMin.x, p.x), std::fmin(this->mMin.y, p.y), std::fmin(this->mMin.z, p.z));
			this->mMax = Vec3(std::fmax(this->mMax.x, p.x), std::fmax(this->mMax.y, p.y), std::fmax(this->mMax.z, p.z));
		}
		bool Overlaps(const AABox &o) const
		{
			return (this->mMin.x <= o.mMax.x) && (this->mMax.x >= o.mMin.x) && (this->mMin.y <= o.mMax.y) && (this->mMax.y >= o.mMin.y) &&
				(this->mMin.z <= o.mMax.z) && (this->mMax.z >= o.mMin.z);
		}
	};

	class Plane
	{
	public:
		Vec3 mNormal;
		float mConstant;
		Plane() : mConstant(0.0f) { }
		Plane(const Vec3 &normal, float constant) : mNormal(normal), mConstant(constant) { }
	};

	// ---------------------------------------------------------------------------------------------
	// Reference counting (subset of Jolt's intrusive Ref/RefConst).
	// ---------------------------------------------------------------------------------------------
	class RefTargetBase
	{
	protected:
		mutable uint32 mRefCount = 0;
	public:
		static constexpr uint32 cEmbedded = 0x0EBEDDED;

		RefTargetBase() = default;
		RefTargetBase(const RefTargetBase&) : mRefCount(0) { }
		RefTargetBase &operator=(const RefTargetBase&) { return *this; }
		virtual ~RefTargetBase() = default;

		void SetEmbedded() const { this->mRefCount += cEmbedded; }
		void AddRef() const { this->mRefCount++; }
		void Release() const
		{
			if (--this->mRefCount == 0)
			{
				delete this;
			}
		}
		uint32 GetRefCount() const { return this->mRefCount; }
	};

	template<class T>
	class RefTarget : public RefTargetBase { };

	template<class T>
	class Ref
	{
	private:
		T *mPtr;
	public:
		Ref() : mPtr(nullptr) { }
		Ref(T *ptr) : mPtr(ptr) { if (mPtr != nullptr) mPtr->AddRef(); }
		Ref(const Ref &o) : mPtr(o.mPtr) { if (mPtr != nullptr) mPtr->AddRef(); }
		Ref(Ref &&o) noexcept : mPtr(o.mPtr) { o.mPtr = nullptr; }
		~Ref() { if (mPtr != nullptr) mPtr->Release(); }
		Ref &operator=(T *ptr) { if (ptr != nullptr) ptr->AddRef(); if (mPtr != nullptr) mPtr->Release(); mPtr = ptr; return *this; }
		Ref &operator=(const Ref &o) { return (*this = o.mPtr); }
		Ref &operator=(Ref &&o) noexcept { if (this != &o) { if (mPtr != nullptr) mPtr->Release(); mPtr = o.mPtr; o.mPtr = nullptr; } return *this; }
		T *GetPtr() const { return mPtr; }
		T *operator->() const { return mPtr; }
		T &operator*() const { return *mPtr; }
		operator T*() const { return mPtr; }
	};

	template<class T>
	class RefConst
	{
	private:
		const T *mPtr;
	public:
		RefConst() : mPtr(nullptr) { }
		RefConst(const T *ptr) : mPtr(ptr) { if (mPtr != nullptr) mPtr->AddRef(); }
		RefConst(const RefConst &o) : mPtr(o.mPtr) { if (mPtr != nullptr) mPtr->AddRef(); }
		RefConst(const Ref<T> &o) : RefConst(o.GetPtr()) { }
		RefConst(RefConst &&o) noexcept : mPtr(o.mPtr) { o.mPtr = nullptr; }
		~RefConst() { if (mPtr != nullptr) mPtr->Release(); }
		RefConst &operator=(const T *ptr) { if (ptr != nullptr) ptr->AddRef(); if (mPtr != nullptr) mPtr->Release(); mPtr = ptr; return *this; }
		RefConst &operator=(const RefConst &o) { return (*this = o.mPtr); }
		RefConst &operator=(RefConst &&o) noexcept { if (this != &o) { if (mPtr != nullptr) mPtr->Release(); mPtr = o.mPtr; o.mPtr = nullptr; } return *this; }
		const T *GetPtr() const { return mPtr; }
		const T *operator->() const { return mPtr; }
		const T &operator*() const { return *mPtr; }
		operator const T*() const { return mPtr; }
	};

	// ---------------------------------------------------------------------------------------------
	// IDs and layers
	// ---------------------------------------------------------------------------------------------
	class BodyID
	{
	public:
		static constexpr uint32 cInvalidBodyID = 0xFFFFFFFF;
		static constexpr uint32 cMaxBodyIndex = 0x7FFFFF;

		uint32 mID;

		constexpr BodyID() : mID(cInvalidBodyID) { }
		constexpr explicit BodyID(uint32 id) : mID(id) { }
		constexpr BodyID(uint32 index, uint8 sequence) : mID((uint32(sequence) << 24) | index) { }

		uint32 GetIndex() const { return this->mID & cMaxBodyIndex; }
		uint8 GetSequenceNumber() const { return static_cast<uint8>(this->mID >> 24); }
		uint32 GetIndexAndSequenceNumber() const { return this->mID; }
		bool IsInvalid() const { return this->mID == cInvalidBodyID; }

		bool operator==(const BodyID &o) const { return this->mID == o.mID; }
		bool operator!=(const BodyID &o) const { return this->mID != o.mID; }
		bool operator<(const BodyID &o) const { return this->mID < o.mID; }
	};

	class SubShapeID
	{
	public:
		uint32 mValue;
		constexpr SubShapeID() : mValue(0xFFFFFFFF) { }
		constexpr explicit SubShapeID(uint32 v) : mValue(v) { }
		uint32 GetValue() const { return this->mValue; }
		bool IsEmpty() const { return this->mValue == 0xFFFFFFFF; }
		bool operator==(const SubShapeID &o) const { return this->mValue == o.mValue; }
		bool operator!=(const SubShapeID &o) const { return this->mValue != o.mValue; }
	};

	class SubShapeIDPair
	{
	private:
		BodyID mBody1ID, mBody2ID;
		SubShapeID mSubShapeID1, mSubShapeID2;
	public:
		SubShapeIDPair() = default;
		SubShapeIDPair(BodyID b1, SubShapeID s1, BodyID b2, SubShapeID s2) : mBody1ID(b1), mBody2ID(b2), mSubShapeID1(s1), mSubShapeID2(s2) { }
		const BodyID &GetBody1ID() const { return this->mBody1ID; }
		const BodyID &GetBody2ID() const { return this->mBody2ID; }
		const SubShapeID &GetSubShapeID1() const { return this->mSubShapeID1; }
		const SubShapeID &GetSubShapeID2() const { return this->mSubShapeID2; }
	};

	using ObjectLayer = uint16;

	class BroadPhaseLayer
	{
	public:
		using Type = uint8;
	private:
		Type mValue;
	public:
		constexpr BroadPhaseLayer() : mValue(0xFF) { }
		constexpr explicit BroadPhaseLayer(Type value) : mValue(value) { }
		constexpr Type GetValue() const { return this->mValue; }
		constexpr explicit operator Type() const { return this->mValue; }
		constexpr bool operator==(const BroadPhaseLayer &o) const { return this->mValue == o.mValue; }
		constexpr bool operator!=(const BroadPhaseLayer &o) const { return this->mValue != o.mValue; }
	};

	class ObjectLayerPairFilter
	{
	public:
		virtual ~ObjectLayerPairFilter() = default;
		virtual bool ShouldCollide(ObjectLayer object1, ObjectLayer object2) const { return true; }
	};

	class BroadPhaseLayerInterface
	{
	public:
		virtual ~BroadPhaseLayerInterface() = default;
		virtual uint32 GetNumBroadPhaseLayers() const = 0;
		virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const = 0;
	};

	class ObjectVsBroadPhaseLayerFilter
	{
	public:
		virtual ~ObjectVsBroadPhaseLayerFilter() = default;
		virtual bool ShouldCollide(ObjectLayer layer1, BroadPhaseLayer layer2) const { return true; }
	};

	class BroadPhaseLayerFilter
	{
	public:
		virtual ~BroadPhaseLayerFilter() = default;
		virtual bool ShouldCollide(BroadPhaseLayer layer) const { return true; }
	};

	class ObjectLayerFilter
	{
	public:
		virtual ~ObjectLayerFilter() = default;
		virtual bool ShouldCollide(ObjectLayer layer) const { return true; }
	};

	class Body;

	class BodyFilter
	{
	public:
		virtual ~BodyFilter() = default;
		virtual bool ShouldCollide(const BodyID &bodyID) const { return true; }
	};

	class ShapeFilter
	{
	public:
		virtual ~ShapeFilter() = default;
	};

	// ---------------------------------------------------------------------------------------------
	// Enums
	// ---------------------------------------------------------------------------------------------
	enum class EMotionType : uint8 { Static, Kinematic, Dynamic };
	enum class EActivation { Activate, DontActivate };
	enum class EShapeType : uint8 { Convex, Compound, Decorated, Mesh, HeightField, SoftBody, User1, User2, User3, User4, Plane, Empty };
	enum class EShapeSubType : uint8 { Sphere, Box, Triangle, Capsule, TaperedCapsule, Cylinder, ConvexHull, StaticCompound, MutableCompound };
	enum class EBackFaceMode : uint8 { IgnoreBackFaces, CollideWithBackFaces };
	enum class EGroundState { OnGround, OnSteepGround, NotSupported, InAir };
	enum class EPhysicsUpdateError : uint32 { None = 0 };
	enum class ValidateResult { AcceptAllContactsForThisBodyPair, AcceptContact, RejectContact, RejectAllContactsForThisBodyPair };

	enum class EAllowedDOFs : uint8
	{
		None = 0,
		All = 0b111111,
		TranslationX = 0b000001,
		TranslationY = 0b000010,
		TranslationZ = 0b000100,
		RotationX = 0b001000,
		RotationY = 0b010000,
		RotationZ = 0b100000,
		Plane2D = TranslationX | TranslationY | RotationZ
	};

	constexpr EAllowedDOFs operator|(EAllowedDOFs a, EAllowedDOFs b) { return static_cast<EAllowedDOFs>(static_cast<uint8>(a) | static_cast<uint8>(b)); }
	constexpr EAllowedDOFs operator&(EAllowedDOFs a, EAllowedDOFs b) { return static_cast<EAllowedDOFs>(static_cast<uint8>(a) & static_cast<uint8>(b)); }

	// ---------------------------------------------------------------------------------------------
	// Shapes
	// ---------------------------------------------------------------------------------------------
	class Shape;

	class TransformedShape
	{
	public:
		RVec3 mShapePositionCOM;
		Quat mShapeRotation;
		const Shape *mShape = nullptr;
	};

	class Shape : public RefTarget<Shape>
	{
	private:
		EShapeType mType;
		EShapeSubType mSubType;
	public:
		Shape(EShapeType type, EShapeSubType subType) : mType(type), mSubType(subType) { }
		virtual ~Shape() = default;

		EShapeType GetType() const { return this->mType; }
		EShapeSubType GetSubType() const { return this->mSubType; }

		virtual AABox GetLocalBounds() const = 0;

		// For compound shapes, resolves the transformed child shape. For convex shapes, returns itself.
		virtual TransformedShape GetSubShapeTransformedShape(const SubShapeID &subShapeID, Vec3Arg positionCOM, QuatArg rotation,
			Vec3Arg scale, SubShapeID &outRemainder) const;
	};

	using ShapeRefC = RefConst<Shape>;

	class BoxShape final : public Shape
	{
	public:
		Vec3 mHalfExtent;
		float mConvexRadius;

		BoxShape(const Vec3 &halfExtent, float convexRadius)
			: Shape(EShapeType::Convex, EShapeSubType::Box), mHalfExtent(halfExtent), mConvexRadius(convexRadius) { }

		AABox GetLocalBounds() const override { return AABox(-this->mHalfExtent, this->mHalfExtent); }
		Vec3 GetHalfExtent() const { return this->mHalfExtent; }
	};

	class CapsuleShape final : public Shape
	{
	public:
		float mHalfHeightOfCylinder;
		float mRadius;

		CapsuleShape(float halfHeightOfCylinder, float radius)
			: Shape(EShapeType::Convex, EShapeSubType::Capsule), mHalfHeightOfCylinder(halfHeightOfCylinder), mRadius(radius) { }

		AABox GetLocalBounds() const override
		{
			const Vec3 extent(this->mRadius, this->mHalfHeightOfCylinder + this->mRadius, this->mRadius);
			return AABox(-extent, extent);
		}

		float GetRadius() const { return this->mRadius; }
		float GetHalfHeightOfCylinder() const { return this->mHalfHeightOfCylinder; }
	};

	// A child box in a static compound. World-space XZ bounds are cached for the per-compound grid broadphase.
	struct CompoundSubBox
	{
		Vec3 mPosition; // Relative to compound body.
		float mCosY, mSinY; // Y rotation.
		Vec3 mHalfExtent;
		AABox mBounds; // Relative to compound body, rotation-inclusive.
		RefConst<Shape> mShape;
	};

	class CompoundShape : public Shape
	{
	public:
		struct SubShape
		{
			Vec3 mPositionCOM;
			Quat mRotation;
			RefConst<Shape> mShape;
			const Shape *GetShape() const { return this->mShape.GetPtr(); }
			Vec3 GetPositionCOM() const { return this->mPositionCOM; }
			Quat GetRotation() const { return this->mRotation; }
		};

		std::vector<SubShape> mSubShapes;
		AABox mLocalBounds;

		CompoundShape(EShapeSubType subType) : Shape(EShapeType::Compound, subType) { }

		AABox GetLocalBounds() const override { return this->mLocalBounds; }
		uint GetNumSubShapes() const { return static_cast<uint>(this->mSubShapes.size()); }
		const SubShape &GetSubShape(uint index) const { return this->mSubShapes[index]; }

		TransformedShape GetSubShapeTransformedShape(const SubShapeID &subShapeID, Vec3Arg positionCOM, QuatArg rotation,
			Vec3Arg scale, SubShapeID &outRemainder) const override;
	};

	class StaticCompoundShape final : public CompoundShape
	{
	public:
		// Broadphase acceleration: uniform XZ grid over the compound's bounds, each cell listing overlapping sub-box indices.
		static constexpr float GRID_CELL_SIZE = 4.0f;

		std::vector<CompoundSubBox> mBoxes; // Parallel to mSubShapes; only box sub-shapes are supported.
		std::vector<uint16> mCellStarts; // Size = cellCountX * cellCountZ + 1.
		std::vector<uint16> mCellIndices;
		float mGridMinX = 0.0f, mGridMinZ = 0.0f;
		int mCellCountX = 0, mCellCountZ = 0;

		StaticCompoundShape() : CompoundShape(EShapeSubType::StaticCompound) { }

		void BuildGrid();

		// Calls func(boxIndex) for every sub-box whose cell overlaps the given compound-local XZ range.
		template<typename Func>
		void ForEachBoxInRange(float minX, float minZ, float maxX, float maxZ, Func &&func) const
		{
			if ((this->mCellCountX <= 0) || (this->mCellCountZ <= 0))
			{
				return;
			}

			const float inv = 1.0f / GRID_CELL_SIZE;
			int x0 = static_cast<int>(std::floor((minX - this->mGridMinX) * inv));
			int z0 = static_cast<int>(std::floor((minZ - this->mGridMinZ) * inv));
			int x1 = static_cast<int>(std::floor((maxX - this->mGridMinX) * inv));
			int z1 = static_cast<int>(std::floor((maxZ - this->mGridMinZ) * inv));
			if ((x1 < 0) || (z1 < 0) || (x0 >= this->mCellCountX) || (z0 >= this->mCellCountZ))
			{
				return;
			}

			x0 = (x0 < 0) ? 0 : x0;
			z0 = (z0 < 0) ? 0 : z0;
			x1 = (x1 >= this->mCellCountX) ? (this->mCellCountX - 1) : x1;
			z1 = (z1 >= this->mCellCountZ) ? (this->mCellCountZ - 1) : z1;

			// A box can span several cells; dedupe via a small visited stamp list local to the query.
			uint16 visited[64];
			int visitedCount = 0;
			for (int cz = z0; cz <= z1; cz++)
			{
				for (int cx = x0; cx <= x1; cx++)
				{
					const int cellIndex = cx + (cz * this->mCellCountX);
					const int start = this->mCellStarts[cellIndex];
					const int end = this->mCellStarts[cellIndex + 1];
					for (int i = start; i < end; i++)
					{
						const uint16 boxIndex = this->mCellIndices[i];
						bool seen = false;
						for (int v = 0; v < visitedCount; v++)
						{
							if (visited[v] == boxIndex)
							{
								seen = true;
								break;
							}
						}

						if (seen)
						{
							continue;
						}

						if (visitedCount < 64)
						{
							visited[visitedCount++] = boxIndex;
						}

						func(static_cast<int>(boxIndex));
					}
				}
			}
		}
	};

	class ShapeSettings : public RefTarget<ShapeSettings>
	{
	public:
		class ShapeResult
		{
		private:
			RefConst<Shape> mShape;
			std::string mError;
		public:
			bool HasError() const { return !this->mError.empty(); }
			bool IsValid() const { return this->mShape.GetPtr() != nullptr; }
			const std::string &GetError() const { return this->mError; }
			const RefConst<Shape> &Get() const { return this->mShape; }
			void Set(const Shape *shape) { this->mShape = shape; }
			void SetError(const std::string &error) { this->mError = error; }
		};

		virtual ~ShapeSettings() = default;
		virtual ShapeResult Create() const = 0;
	};

	class BoxShapeSettings final : public ShapeSettings
	{
	public:
		Vec3 mHalfExtent;
		float mConvexRadius = 0.05f;

		BoxShapeSettings() = default;
		BoxShapeSettings(const Vec3 &halfExtent, float convexRadius = 0.05f) : mHalfExtent(halfExtent), mConvexRadius(convexRadius) { }

		ShapeResult Create() const override;
	};

	class CapsuleShapeSettings final : public ShapeSettings
	{
	public:
		float mHalfHeightOfCylinder = 0.0f;
		float mRadius = 0.0f;

		CapsuleShapeSettings() = default;
		CapsuleShapeSettings(float halfHeightOfCylinder, float radius) : mHalfHeightOfCylinder(halfHeightOfCylinder), mRadius(radius) { }

		ShapeResult Create() const override;
	};

	class StaticCompoundShapeSettings final : public ShapeSettings
	{
	public:
		struct SubShapeSettings
		{
			RefConst<ShapeSettings> mShape;
			Vec3 mPosition;
			Quat mRotation;
		};

		std::vector<SubShapeSettings> mSubShapes;

		void AddShape(Vec3Arg position, QuatArg rotation, const ShapeSettings *shape)
		{
			SubShapeSettings s;
			s.mShape = shape;
			s.mPosition = position;
			s.mRotation = rotation;
			this->mSubShapes.emplace_back(std::move(s));
		}

		ShapeResult Create() const override;
	};

	// ---------------------------------------------------------------------------------------------
	// Bodies
	// ---------------------------------------------------------------------------------------------
	class BodyCreationSettings
	{
	public:
		RVec3 mPosition;
		Quat mRotation;
		EMotionType mMotionType = EMotionType::Dynamic;
		ObjectLayer mObjectLayer = 0;
		bool mIsSensor = false;
		bool mEnhancedInternalEdgeRemoval = false;
		bool mAllowSleeping = true;
		EAllowedDOFs mAllowedDOFs = EAllowedDOFs::All;
		float mFriction = 0.2f;
		float mLinearDamping = 0.05f;
		float mGravityFactor = 1.0f;
		uint64 mUserData = 0;

		RefConst<Shape> mShapePtr;
		RefConst<ShapeSettings> mShapeSettings;

		BodyCreationSettings() = default;
		BodyCreationSettings(const ShapeSettings *shape, RVec3Arg position, QuatArg rotation, EMotionType motionType, ObjectLayer objectLayer)
			: mPosition(position), mRotation(rotation), mMotionType(motionType), mObjectLayer(objectLayer), mShapeSettings(shape) { }
		BodyCreationSettings(const Shape *shape, RVec3Arg position, QuatArg rotation, EMotionType motionType, ObjectLayer objectLayer)
			: mPosition(position), mRotation(rotation), mMotionType(motionType), mObjectLayer(objectLayer), mShapePtr(shape) { }

		const Shape *GetShape() const;
	};

	class Body
	{
	public:
		BodyID mID;
		RefConst<Shape> mShape;
		RVec3 mPosition; // Center of mass (JoltLite shapes are centered on their origin).
		Quat mRotation;
		Vec3 mLinearVelocity;
		EMotionType mMotionType = EMotionType::Static;
		ObjectLayer mObjectLayer = 0;
		EAllowedDOFs mAllowedDOFs = EAllowedDOFs::All;
		float mGravityFactor = 1.0f;
		float mLinearDamping = 0.0f;
		float mFriction = 0.2f;
		uint64 mUserData = 0;
		bool mIsSensor = false;
		bool mAllowSleeping = true;
		bool mInSystem = false;
		bool mInUse = false;

		// Cached world-space AABB, recomputed on move.
		AABox mWorldBounds;

		const BodyID &GetID() const { return this->mID; }
		bool IsSensor() const { return this->mIsSensor; }
		bool IsStatic() const { return this->mMotionType == EMotionType::Static; }
		bool IsKinematic() const { return this->mMotionType == EMotionType::Kinematic; }
		bool IsDynamic() const { return this->mMotionType == EMotionType::Dynamic; }
		bool IsActive() const { return this->mInSystem && (this->mMotionType != EMotionType::Static); }
		const Shape *GetShape() const { return this->mShape.GetPtr(); }
		RVec3 GetPosition() const { return this->mPosition; }
		RVec3 GetCenterOfMassPosition() const { return this->mPosition; }
		Quat GetRotation() const { return this->mRotation; }
		Vec3 GetLinearVelocity() const { return this->mLinearVelocity; }
		ObjectLayer GetObjectLayer() const { return this->mObjectLayer; }
		EMotionType GetMotionType() const { return this->mMotionType; }
		uint64 GetUserData() const { return this->mUserData; }
		void SetAllowSleeping(bool allow) { this->mAllowSleeping = allow; }
		void SetUserData(uint64 data) { this->mUserData = data; }

		void UpdateWorldBounds();
	};

	class PhysicsSystem;

	class BodyInterface
	{
	private:
		PhysicsSystem *mSystem = nullptr;
	public:
		void Init(PhysicsSystem *system) { this->mSystem = system; }

		Body *CreateBody(const BodyCreationSettings &settings);
		BodyID CreateAndAddBody(const BodyCreationSettings &settings, EActivation activation);
		void AddBody(const BodyID &bodyID, EActivation activation);
		void RemoveBody(const BodyID &bodyID);
		void DestroyBody(const BodyID &bodyID);
		bool IsAdded(const BodyID &bodyID) const;

		void SetLinearVelocity(const BodyID &bodyID, Vec3Arg velocity);
		Vec3 GetLinearVelocity(const BodyID &bodyID) const;
		RVec3 GetPosition(const BodyID &bodyID) const;
		RVec3 GetCenterOfMassPosition(const BodyID &bodyID) const;
		void SetPosition(const BodyID &bodyID, RVec3Arg position, EActivation activation);
		RefConst<Shape> GetShape(const BodyID &bodyID) const;
		void SetObjectLayer(const BodyID &bodyID, ObjectLayer layer);
		ObjectLayer GetObjectLayer(const BodyID &bodyID) const;
		void SetMotionType(const BodyID &bodyID, EMotionType motionType, EActivation activation);
		void SetGravityFactor(const BodyID &bodyID, float gravityFactor);
		void ActivateBody(const BodyID &bodyID) { }
	};

	class BodyLockInterface
	{
	private:
		PhysicsSystem *mSystem = nullptr;
	public:
		void Init(PhysicsSystem *system) { this->mSystem = system; }
		Body *TryGetBody(const BodyID &bodyID) const;
	};

	template<bool Write>
	class BodyLockBase
	{
	private:
		Body *mBody;
	public:
		BodyLockBase(const BodyLockInterface &iface, const BodyID &bodyID) : mBody(iface.TryGetBody(bodyID)) { }
		bool Succeeded() const { return this->mBody != nullptr; }
		bool SucceededAndIsInBroadPhase() const { return (this->mBody != nullptr) && this->mBody->mInSystem; }
		void ReleaseLock() { }
		Body &GetBody() const { return *this->mBody; }
	};

	using BodyLockRead = BodyLockBase<false>;
	using BodyLockWrite = BodyLockBase<true>;

	// ---------------------------------------------------------------------------------------------
	// Listeners
	// ---------------------------------------------------------------------------------------------
	class CollideShapeResult
	{
	public:
		Vec3 mContactPointOn1, mContactPointOn2;
		Vec3 mPenetrationAxis;
		float mPenetrationDepth = 0.0f;
		SubShapeID mSubShapeID1, mSubShapeID2;
		BodyID mBodyID2;
	};

	class ContactManifold
	{
	public:
		RVec3 mBaseOffset;
		Vec3 mWorldSpaceNormal;
		float mPenetrationDepth = 0.0f;
		SubShapeID mSubShapeID1, mSubShapeID2;
	};

	class ContactSettings
	{
	public:
		float mCombinedFriction = 0.0f;
		float mCombinedRestitution = 0.0f;
		bool mIsSensor = false;
	};

	class ContactListener
	{
	public:
		virtual ~ContactListener() = default;
		virtual ValidateResult OnContactValidate(const Body &body1, const Body &body2, RVec3Arg baseOffset, const CollideShapeResult &collisionResult) { return ValidateResult::AcceptAllContactsForThisBodyPair; }
		virtual void OnContactAdded(const Body &body1, const Body &body2, const ContactManifold &manifold, ContactSettings &settings) { }
		virtual void OnContactPersisted(const Body &body1, const Body &body2, const ContactManifold &manifold, ContactSettings &settings) { }
		virtual void OnContactRemoved(const SubShapeIDPair &subShapePair) { }
	};

	class BodyActivationListener
	{
	public:
		virtual ~BodyActivationListener() = default;
		virtual void OnBodyActivated(const BodyID &bodyID, uint64 bodyUserData) = 0;
		virtual void OnBodyDeactivated(const BodyID &bodyID, uint64 bodyUserData) = 0;
	};

	// ---------------------------------------------------------------------------------------------
	// Allocators / jobs (no-ops on PS2: the solver is single-threaded and uses fixed pools).
	// ---------------------------------------------------------------------------------------------
	class TempAllocator
	{
	public:
		virtual ~TempAllocator() = default;
	};

	class TempAllocatorImpl final : public TempAllocator
	{
	public:
		// Intentionally does NOT allocate the requested size. JoltLite never uses temp memory.
		explicit TempAllocatorImpl(uint32 size) { static_cast<void>(size); }
	};

	class TempAllocatorMalloc final : public TempAllocator { };

	class JobSystem
	{
	public:
		virtual ~JobSystem() = default;
	};

	class JobSystemThreadPool final : public JobSystem
	{
	public:
		JobSystemThreadPool(uint maxJobs, uint maxBarriers, int numThreads = -1)
		{
			static_cast<void>(maxJobs);
			static_cast<void>(maxBarriers);
			static_cast<void>(numThreads);
		}
	};

	class JobSystemSingleThreaded final : public JobSystem { };

	constexpr uint cMaxPhysicsJobs = 1;
	constexpr uint cMaxPhysicsBarriers = 1;

	class Factory
	{
	public:
		static Factory *sInstance;
	};

	void RegisterDefaultAllocator();
	void RegisterTypes();
	void UnregisterTypes();

	// ---------------------------------------------------------------------------------------------
	// Physics system
	// ---------------------------------------------------------------------------------------------
	class DefaultBroadPhaseLayerFilter final : public BroadPhaseLayerFilter { };
	class DefaultObjectLayerFilter final : public ObjectLayerFilter { };

	struct PhysicsStats
	{
		int bodyCount = 0;
		int maxBodyCount = 0;
		int activeDynamicCount = 0;
		int compoundBoxCount = 0;
		int boxTestsLastUpdate = 0;
		int pairTestsLastUpdate = 0;
		int contactsAddedLastUpdate = 0;
		int trackedContactCount = 0;
	};

	class Character;

	class PhysicsSystem
	{
	private:
		friend class BodyInterface;
		friend class BodyLockInterface;
		friend class Character;

		std::vector<Body> mBodies; // Fixed capacity, reserved at Init().
		std::vector<uint32> mFreeIndices;
		std::vector<uint32> mActiveList; // Indices of bodies currently added to the system.
		BodyInterface mBodyInterface;
		BodyLockInterface mBodyLockInterface;
		ContactListener *mContactListener = nullptr;
		BodyActivationListener *mActivationListener = nullptr;
		const ObjectLayerPairFilter *mObjectLayerPairFilter = nullptr;
		DefaultBroadPhaseLayerFilter mDefaultBroadPhaseLayerFilter;
		DefaultObjectLayerFilter mDefaultObjectLayerFilter;
		Vec3 mGravity = Vec3(0.0f, -9.81f, 0.0f);

		// Persistent contact pair tracking, so OnContactAdded() fires once per overlap start.
		struct ContactKey
		{
			uint32 bodyA, bodyB; // Body IDs (full, incl. sequence).
			uint32 subShapeB;
			bool operator==(const ContactKey &o) const { return (bodyA == o.bodyA) && (bodyB == o.bodyB) && (subShapeB == o.subShapeB); }
		};

		std::vector<ContactKey> mPrevContacts, mCurContacts; // Fixed capacity.
		size_t mMaxContacts = 0;
		std::vector<Character*> mCharacters;
		PhysicsStats mStats;

		Body *getBody(const BodyID &id);
		const Body *getBody(const BodyID &id) const;

		void stepBodies(float dt);
		void resolveMovingBody(Body &body, float dt, bool isCharacter, Character *character);
		void recordContact(const Body &a, const Body &b, SubShapeID subShapeB, const Vec3 &normal, float depth);
		void flushContacts();
	public:
		PhysicsSystem();

		void Init(uint maxBodies, uint numBodyMutexes, uint maxBodyPairs, uint maxContactConstraints,
			const BroadPhaseLayerInterface &broadPhaseLayerInterface, const ObjectVsBroadPhaseLayerFilter &objectVsBroadPhaseLayerFilter,
			const ObjectLayerPairFilter &objectLayerPairFilter);

		EPhysicsUpdateError Update(float deltaTime, int collisionSteps, TempAllocator *tempAllocator, JobSystem *jobSystem);

		BodyInterface &GetBodyInterface() { return this->mBodyInterface; }
		const BodyInterface &GetBodyInterface() const { return this->mBodyInterface; }
		BodyInterface &GetBodyInterfaceNoLock() { return this->mBodyInterface; }
		const BodyLockInterface &GetBodyLockInterface() const { return this->mBodyLockInterface; }
		const BodyLockInterface &GetBodyLockInterfaceNoLock() const { return this->mBodyLockInterface; }

		uint GetNumBodies() const;
		uint GetMaxBodies() const { return static_cast<uint>(this->mBodies.size()); }
		Vec3 GetGravity() const { return this->mGravity; }
		void SetGravity(Vec3Arg gravity) { this->mGravity = gravity; }

		void SetContactListener(ContactListener *listener) { this->mContactListener = listener; }
		void SetBodyActivationListener(BodyActivationListener *listener) { this->mActivationListener = listener; }

		const BroadPhaseLayerFilter &GetDefaultBroadPhaseLayerFilter(ObjectLayer layer) const { return this->mDefaultBroadPhaseLayerFilter; }
		const ObjectLayerFilter &GetDefaultLayerFilter(ObjectLayer layer) const { return this->mDefaultObjectLayerFilter; }

		// Debug/instrumentation.
		const PhysicsStats &GetStats() const { return this->mStats; }

		// Used by Character.
		void registerCharacter(Character *character);
		void unregisterCharacter(Character *character);
	};

	// ---------------------------------------------------------------------------------------------
	// Characters
	// ---------------------------------------------------------------------------------------------
	class CharacterBaseSettings : public RefTarget<CharacterBaseSettings>
	{
	public:
		Vec3 mUp = Vec3::sAxisY();
		Plane mSupportingVolume;
		float mMaxSlopeAngle = 0.7853982f;
		EBackFaceMode mBackFaceMode = EBackFaceMode::CollideWithBackFaces;
		RefConst<Shape> mShape;
		bool mEnhancedInternalEdgeRemoval = false;
	};

	class CharacterSettings final : public CharacterBaseSettings
	{
	public:
		ObjectLayer mLayer = 0;
		float mMass = 80.0f;
		float mFriction = 0.2f;
		float mGravityFactor = 1.0f;
		EAllowedDOFs mAllowedDOFs = EAllowedDOFs::TranslationX | EAllowedDOFs::TranslationY | EAllowedDOFs::TranslationZ;
	};

	class Character
	{
	private:
		friend class PhysicsSystem;

		PhysicsSystem *mSystem;
		BodyID mBodyID;
		RefConst<Shape> mShape;
		Vec3 mUp;
		float mCosMaxSlopeAngle;
		float mStepHeight; // How tall a ledge the capsule's rounded bottom can ride up onto.

		// Ground state computed during the physics update.
		EGroundState mGroundState = EGroundState::InAir;
		Vec3 mGroundNormal;
		BodyID mGroundBodyID;
		bool mSupportedThisUpdate = false;
	public:
		Character(const CharacterSettings *settings, RVec3Arg position, QuatArg rotation, uint64 userData, PhysicsSystem *system);
		~Character();

		void AddToPhysicsSystem(EActivation activation = EActivation::Activate, bool lockBodies = true);
		void RemoveFromPhysicsSystem(bool lockBodies = true);

		const BodyID &GetBodyID() const { return this->mBodyID; }
		RVec3 GetPosition(bool lockBodies = true) const;
		void SetPosition(RVec3Arg position, EActivation activation = EActivation::Activate, bool lockBodies = true);
		Vec3 GetLinearVelocity(bool lockBodies = true) const;
		void SetLinearVelocity(Vec3Arg velocity, bool lockBodies = true);
		const Shape *GetShape() const { return this->mShape.GetPtr(); }
		Vec3 GetUp() const { return this->mUp; }

		void PostSimulation(float maxSeparationDistance, bool lockBodies = true);

		EGroundState GetGroundState() const { return this->mGroundState; }
		bool IsSupported() const { return (this->mGroundState == EGroundState::OnGround) || (this->mGroundState == EGroundState::OnSteepGround); }
		Vec3 GetGroundNormal() const { return this->mGroundNormal; }
		BodyID GetGroundBodyID() const { return this->mGroundBodyID; }

		float GetStepHeight() const { return this->mStepHeight; }
		void SetStepHeight(float h) { this->mStepHeight = h; }

		// Called by the solver.
		void notifyContact(const Vec3 &normal, const BodyID &otherBody);

		void Release() { delete this; }
	};

	class CharacterContactListener
	{
	public:
		virtual ~CharacterContactListener() = default;
	};

	class CharacterVirtual;

	class CharacterVsCharacterCollision
	{
	public:
		virtual ~CharacterVsCharacterCollision() = default;
	};

	class CharacterVsCharacterCollisionSimple final : public CharacterVsCharacterCollision
	{
	public:
		std::vector<CharacterVirtual*> mCharacters;
		void Add(CharacterVirtual *character) { this->mCharacters.push_back(character); }
		void Remove(const CharacterVirtual *character);
	};

	class CharacterVirtualSettings final : public CharacterBaseSettings
	{
	public:
		float mMass = 70.0f;
		float mMaxStrength = 100.0f;
		Vec3 mShapeOffset;
		float mPredictiveContactDistance = 0.1f;
		uint mMaxCollisionIterations = 5;
		uint mMaxConstraintIterations = 15;
		float mMinTimeRemaining = 1.0e-4f;
		float mCollisionTolerance = 1.0e-3f;
		float mCharacterPadding = 0.02f;
		uint mMaxNumHits = 256;
		float mHitReductionCosMaxAngle = 0.999f;
		float mPenetrationRecoverySpeed = 1.0f;
		RefConst<Shape> mInnerBodyShape;
		ObjectLayer mInnerBodyLayer = 0;
	};

	// Lightweight stand-in: OpenTESArena pairs its Character with a CharacterVirtual but only drives the Character.
	// Position/velocity are stored so the API behaves consistently; ExtendedUpdate() is a simple integrate.
	class CharacterVirtual
	{
	private:
		RVec3 mPosition;
		Vec3 mLinearVelocity;
		RefConst<Shape> mShape;
		CharacterVsCharacterCollision *mCharVsChar = nullptr;
		CharacterContactListener *mListener = nullptr;
	public:
		struct ExtendedUpdateSettings
		{
			Vec3 mStickToFloorStepDown = Vec3(0.0f, -0.5f, 0.0f);
			Vec3 mWalkStairsStepUp = Vec3(0.0f, 0.4f, 0.0f);
			float mWalkStairsMinStepForward = 0.02f;
			float mWalkStairsStepForwardTest = 0.15f;
			float mWalkStairsCosAngleForwardContact = 0.9961947f;
			Vec3 mWalkStairsStepDownExtra = Vec3::sZero();
		};

		CharacterVirtual(const CharacterVirtualSettings *settings, RVec3Arg position, QuatArg rotation, uint64 userData, PhysicsSystem *system);

		void SetCharacterVsCharacterCollision(CharacterVsCharacterCollision *c) { this->mCharVsChar = c; }
		void SetListener(CharacterContactListener *listener) { this->mListener = listener; }
		RVec3 GetPosition() const { return this->mPosition; }
		void SetPosition(RVec3Arg position) { this->mPosition = position; }
		Vec3 GetLinearVelocity() const { return this->mLinearVelocity; }
		void SetLinearVelocity(Vec3Arg velocity) { this->mLinearVelocity = velocity; }
		const Shape *GetShape() const { return this->mShape.GetPtr(); }

		void ExtendedUpdate(float deltaTime, Vec3Arg gravity, const ExtendedUpdateSettings &settings,
			const BroadPhaseLayerFilter &bpFilter, const ObjectLayerFilter &olFilter, const BodyFilter &bodyFilter,
			const ShapeFilter &shapeFilter, TempAllocator &allocator)
		{
			this->mLinearVelocity += gravity * deltaTime;
			this->mPosition += this->mLinearVelocity * deltaTime;
		}

		void Release() { delete this; }
	};
}
