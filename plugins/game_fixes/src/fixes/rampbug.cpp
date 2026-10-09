// rampbug — surf ramps: a movement trace that stops short of a ramp or lands on its edge feeds
// TryPlayerMove a plane the player never hit, and the player is flung off. The game's own walk is
// redone here with every trace nudged off the last valid plane, and a bad result is put back.
// This is CS2KZ's fix as carried by CS2Fixes-RampbugFix; it lessens rampbugs, it does not end
// them.
//
// The port keeps the fork's state machine, thresholds and walk. The engine interfaces it needs
// are rebuilt from the fork's own binary:
//  * CMoveData's m_vecVelocity / m_vecAbsOrigin at the byte offsets its compiled code uses
//    (assets/gamedata.jsonc),
//  * CGameTrace with the field offsets of its DWARF (start/end/normal/fraction/...),
//  * Ray_t(mins, maxs) as a 48-byte hull ray (40-byte union + type byte at 0x28, DWARF): type 2
//    when the box has extents, 0 when it does not,
//  * the movement CTraceFilter as a byte image built from the writes of the fork's inlined
//    constructor (vtable = the game's CTraceFilter class), because the engine only reads its
//    fields and calls its vtable.
// Traces go through CNavPhysicsInterface::Nav_TraceShape (vtable index 5), the way the fork
// reaches it (the interface has no instance; a pointer to its vtable stands in for one).

#include "../fixes.hpp"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace game_fixes {
namespace {

constexpr int kMaxBumps = 4;
constexpr float kRampPierceDistance = 0.0625f;
constexpr float kRampBugThreshold = 0.98f;
constexpr float kRampBugVelocityThreshold = 0.95f;
constexpr float kNewRampThreshold = 0.95f;
constexpr float kStandableNormalZ = 0.7f;
constexpr std::uint32_t kFlagOnGround = 1u << 0; // FL_ONGROUND
constexpr std::int32_t kMoveTypeWalk = 2;
constexpr std::int32_t kInvalidEntityHandle = -1; // CEntityHandle: 0xFFFFFFFF means "no entity"

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
	Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
	Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
	Vec3 operator-(float value) const { return {x - value, y - value, z - value}; }
	Vec3& operator+=(const Vec3& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}
	Vec3& operator-=(const Vec3& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}
	bool operator==(const Vec3& other) const { return x == other.x && y == other.y && z == other.z; }
	bool operator!=(const Vec3& other) const { return !(*this == other); }

	float Dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }
	float Length() const { return std::sqrt(Dot(*this)); }
	float Length2D() const { return std::sqrt(x * x + y * y); }
	Vec3 Normalized() const {
		const float length = Length();
		return length > 0.0f ? Vec3{x / length, y / length, z / length} : Vec3{};
	}
	void NormalizeInPlace() { *this = Normalized(); }
};

float Clamp(float value, float low, float high) {
	return value < low ? low : (value > high ? high : value);
}

bool CloseEnough(float a, float b, float epsilon) {
	return std::fabs(a - b) <= epsilon;
}

Vec3 CrossProduct(const Vec3& a, const Vec3& b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

void VectorMA(const Vec3& start, float scale, const Vec3& direction, Vec3& dest) {
	dest = start + direction * scale;
}

// ---- engine structures (layout from the fork's own build) ----------------------

struct BBox {
	Vec3 mins;
	Vec3 maxs;
};

// Ray_t: 48 bytes — a 40-byte union (m_Hull: mins/maxs at +0/+0xC) plus m_eType at +0x28.
// Ray_t(bounds.mins, bounds.maxs) leaves the union's tail untouched (it is only read for the
// mesh/capsule kinds) and sets the type: 2 (hull) unless mins == maxs, then 0 (line).
struct Ray {
	Vec3 mins;
	Vec3 maxs;
	std::uint8_t unused[16];
	std::uint8_t type;
	std::uint8_t pad[7];

	Ray(const Vec3& boxMins, const Vec3& boxMaxs) : mins(boxMins), maxs(boxMaxs), type((boxMins == boxMaxs) ? 0 : 2) {
		std::memset(unused, 0, sizeof(unused));
		std::memset(pad, 0, sizeof(pad));
	}
};

// CGameTrace: 192 bytes; only the fields the fix uses are named (offsets from the fork's DWARF).
struct GameTrace {
	std::uint8_t raw[192];

	Vec3& startPos() { return *reinterpret_cast<Vec3*>(raw + 120); }
	Vec3& endPos() { return *reinterpret_cast<Vec3*>(raw + 132); }
	Vec3& hitNormal() { return *reinterpret_cast<Vec3*>(raw + 144); }
	float& fraction() { return *reinterpret_cast<float*>(raw + 172); }
	bool& startInSolid() { return *reinterpret_cast<bool*>(raw + 187); }

	const Vec3& startPos() const { return *reinterpret_cast<const Vec3*>(raw + 120); }
	const Vec3& endPos() const { return *reinterpret_cast<const Vec3*>(raw + 132); }
	const Vec3& hitNormal() const { return *reinterpret_cast<const Vec3*>(raw + 144); }
	float fraction() const { return *reinterpret_cast<const float*>(raw + 172); }
	bool startInSolid() const { return *reinterpret_cast<const bool*>(raw + 187); }
};

// The movement filter, as the byte image the fork's constructor produces. Every field is written
// at the offset its inlined base/derived constructor used; the engine reads them and calls the
// vtable's ShouldHitEntity.
struct MovementFilter {
	std::uint8_t bytes[64];
};

void WriteU16(std::uint8_t* at, std::uint16_t value) { std::memcpy(at, &value, sizeof(value)); }
void WriteU32(std::uint8_t* at, std::uint32_t value) { std::memcpy(at, &value, sizeof(value)); }
void WriteU64(std::uint8_t* at, std::uint64_t value) { std::memcpy(at, &value, sizeof(value)); }

// ---- the fix ------------------------------------------------------------------

class CRampbugFix final : public CFix, public IMovementListener {
public:
	explicit CRampbugFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "rampbug"; }

	bool Load() override;

	// Load() resolves its own schema fields (m_bDucked, m_fFlags, ...) and the shared offsets
	// (body/scene node/collision) it reads at runtime, so it waits for the schema to answer.
	bool NeedsSchema() const override { return true; }
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

	IMovementListener* Movement() override { return this; }

	polyhook::ResultType OnProcessMovement(void* services, void* move) override;
	polyhook::ResultType OnProcessMovementPost(void* services, void* move) override;
	polyhook::ResultType OnTryPlayerMove(void* services, void* move, void* firstDest, void* firstTrace,
										 bool* isSurfing) override;
	polyhook::ResultType OnTryPlayerMovePost(void* services, void* move, void* firstDest, void* firstTrace,
											 bool* isSurfing) override;
	polyhook::ResultType OnCategorizePosition(void* services, void* move, bool stayOnGround) override;

private:
	struct PlayerState {
		void* move = nullptr;
		bool processingMovement = false;
		bool previousOnGround = false;
		bool didTPM = false;
		bool overrideTPM = false;
		Vec3 tpmVelocity;
		Vec3 tpmOrigin;
		Vec3 lastValidPlane;
		Vec3 takeoffVelocity;
		Vec3 landingOrigin;
		Vec3 landingVelocity;

		void Reset() { *this = PlayerState(); }
	};

	bool ResolveSlot(void* services, int* slot) const;
	PlayerState* GetState(void* services, void** pawn);

	Vec3 GetOrigin(const PlayerState& state, void* pawn) const;
	Vec3 GetVelocity(const PlayerState& state, void* pawn) const;
	void SetOrigin(PlayerState& state, void* pawn, const Vec3& origin);
	void SetVelocity(PlayerState& state, void* pawn, const Vec3& velocity);
	BBox GetBounds(void* services) const;
	std::uint32_t FlagsOf(void* pawn) const;
	std::int32_t MoveTypeOf(void* pawn) const;

	void BuildFilter(void* pawn, MovementFilter& filter) const;
	void TracePlayerBBox(const Vec3& start, const Vec3& end, const BBox& bounds, const MovementFilter& filter,
						 GameTrace& trace) const;
	bool IsValidMovementTrace(GameTrace& trace, const BBox& bounds, const MovementFilter& filter) const;

	void RegisterLanding(PlayerState& state, void* pawn, const Vec3& velocity);
	void ApplySlopeFix(PlayerState& state, void* pawn);

	Vec3 MoveVelocity(const void* move) const {
		return *reinterpret_cast<const Vec3*>(static_cast<const std::uint8_t*>(move) + m_env.offMoveVelocity);
	}
	Vec3 MoveAbsOrigin(const void* move) const {
		return *reinterpret_cast<const Vec3*>(static_cast<const std::uint8_t*>(move) + m_env.offMoveAbsOrigin);
	}
	void SetMoveVelocity(void* move, const Vec3& value) const {
		*reinterpret_cast<Vec3*>(static_cast<std::uint8_t*>(move) + m_env.offMoveVelocity) = value;
	}
	void SetMoveAbsOrigin(void* move, const Vec3& value) const {
		*reinterpret_cast<Vec3*>(static_cast<std::uint8_t*>(move) + m_env.offMoveAbsOrigin) = value;
	}

	PlayerState m_states[64];

	// schema offsets this fix needs on top of the shared ones
	std::int32_t m_offServicesDucked = -1;	 // CCSPlayer_MovementServices::m_bDucked
	std::int32_t m_offPawnFlags = -1;		 // CBaseEntity::m_fFlags
	std::int32_t m_offPawnMoveType = -1;	 // CBaseEntity::m_MoveType
	std::int32_t m_offPawnGroundEntity = -1; // CBaseEntity::m_hGroundEntity
	std::int32_t m_offPawnVelocity = -1;	 // CBaseEntity::m_vecAbsVelocity
};

bool CRampbugFix::Load() {
	auto offset = [](const char* className, const char* member) -> std::int32_t {
		return __s2sdk_GetSchemaOffset ? s2sdk::GetSchemaOffset(plg::string(className), plg::string(member)) : -1;
	};
	m_offServicesDucked = offset("CCSPlayer_MovementServices", "m_bDucked");
	m_offPawnFlags = offset("CBaseEntity", "m_fFlags");
	m_offPawnMoveType = offset("CBaseEntity", "m_MoveType");
	m_offPawnGroundEntity = offset("CBaseEntity", "m_hGroundEntity");
	m_offPawnVelocity = offset("CBaseEntity", "m_vecAbsVelocity");

	if (!m_env.vtableNavPhysics || m_env.idxNavTraceShape < 0 || !m_env.vtableTraceFilter) {
		LogFix(Name(), "CNavPhysicsInterface or CTraceFilter vtable unavailable", plg::Severity::Warning);
		return false;
	}
	if (m_env.offMoveVelocity < 0 || m_env.offMoveAbsOrigin < 0) {
		LogFix(Name(), "CMoveData offsets missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	if (m_env.offEntityCollision < 0 || m_env.offCollisionAttribute < 0 || m_env.offAttributeHierarchyId < 0 ||
		m_env.offAttributeInteractsWith < 0 || m_env.offEntityOwnerHandle < 0 || m_offServicesDucked < 0 ||
		m_offPawnFlags < 0 || m_offPawnMoveType < 0 || m_offPawnGroundEntity < 0 || m_offPawnVelocity < 0) {
		LogFix(Name(), "schema offsets for the movement filter are incomplete", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("installed: Nav_TraceShape index %d on vtable %p, CTraceFilter vtable %p",
						  m_env.idxNavTraceShape, m_env.vtableNavPhysics, m_env.vtableTraceFilter));
	return true;
}

void CRampbugFix::Unload() {
	// nothing owned here: the movement detours live in the plugin and are removed with it
}

void CRampbugFix::OnStartupServer(const char* mapName) {
	(void)mapName;
	for (PlayerState& state : m_states) {
		state.Reset();
	}
}

bool CRampbugFix::ResolveSlot(void* services, int* slot) const {
	if (!services || !__s2sdk_PlayerServicesToPlayerSlot) {
		return false;
	}
	const int resolved = s2sdk::PlayerServicesToPlayerSlot(services);
	if (resolved < 0 || resolved >= 64) {
		return false;
	}
	*slot = resolved;
	return true;
}

CRampbugFix::PlayerState* CRampbugFix::GetState(void* services, void** pawn) {
	int slot = -1;
	if (!ResolveSlot(services, &slot)) {
		return nullptr;
	}
	void* resolvedPawn = __s2sdk_GetClientPawn ? s2sdk::GetClientPawn(slot) : nullptr;
	if (!resolvedPawn) {
		return nullptr;
	}
	if (pawn) {
		*pawn = resolvedPawn;
	}
	return &m_states[slot];
}

std::uint32_t CRampbugFix::FlagsOf(void* pawn) const {
	if (!pawn || m_offPawnFlags < 0) {
		return 0;
	}
	return static_cast<std::uint32_t>(Int32At(pawn, m_offPawnFlags));
}

std::int32_t CRampbugFix::MoveTypeOf(void* pawn) const {
	if (!pawn || m_offPawnMoveType < 0) {
		return -1;
	}
	return Int32At(pawn, m_offPawnMoveType);
}

Vec3 CRampbugFix::GetOrigin(const PlayerState& state, void* pawn) const {
	// m_CBodyComponent / m_pSceneNode are stored-pointer ("ptr") fields: each hop reads the
	// pointer value out of the field (the original's m_CBodyComponent()->m_pSceneNode()), it is
	// not the field address itself.
	void* body = nullptr;
	if (pawn) {
		body = PtrAt(pawn, m_env.offEntityBodyComponent);
	}
	void* sceneNode = body ? PtrAt(body, m_env.offBodySceneNode) : nullptr;
	if (!sceneNode) {
		return MoveAbsOrigin(state.move);
	}
	return *reinterpret_cast<const Vec3*>(static_cast<const std::uint8_t*>(sceneNode) + m_env.offSceneNodeAbsOrigin);
}

Vec3 CRampbugFix::GetVelocity(const PlayerState& state, void* pawn) const {
	if (state.processingMovement && state.move) {
		return MoveVelocity(state.move);
	}
	if (pawn && m_offPawnVelocity >= 0) {
		return *reinterpret_cast<const Vec3*>(static_cast<const std::uint8_t*>(pawn) + m_offPawnVelocity);
	}
	return Vec3{};
}

void CRampbugFix::SetOrigin(PlayerState& state, void* pawn, const Vec3& origin) {
	if (state.processingMovement && state.move) {
		SetMoveAbsOrigin(state.move, origin);
		return;
	}
	void* body = pawn ? PtrAt(pawn, m_env.offEntityBodyComponent) : nullptr;
	void* sceneNode = body ? PtrAt(body, m_env.offBodySceneNode) : nullptr;
	if (sceneNode) {
		*reinterpret_cast<Vec3*>(static_cast<std::uint8_t*>(sceneNode) + m_env.offSceneNodeAbsOrigin) = origin;
	}
}

void CRampbugFix::SetVelocity(PlayerState& state, void* pawn, const Vec3& velocity) {
	if (state.processingMovement && state.move) {
		SetMoveVelocity(state.move, velocity);
		return;
	}
	if (pawn && m_offPawnVelocity >= 0) {
		*reinterpret_cast<Vec3*>(static_cast<std::uint8_t*>(pawn) + m_offPawnVelocity) = velocity;
	}
}

BBox CRampbugFix::GetBounds(void* services) const {
	BBox bounds;
	bounds.mins = {-16.0f, -16.0f, 0.0f};
	const bool ducked = services && m_offServicesDucked >= 0 && BoolAt(services, m_offServicesDucked);
	bounds.maxs = {16.0f, 16.0f, ducked ? 54.0f : 72.0f};
	return bounds;
}

void CRampbugFix::BuildFilter(void* pawn, MovementFilter& filter) const {
	std::memset(filter.bytes, 0, sizeof(filter.bytes));
	WriteU64(filter.bytes, reinterpret_cast<std::uint64_t>(m_env.vtableTraceFilter)); // vptr
	// m_nInteractsExclude (+0x10) stays 0.
	WriteU64(filter.bytes + 0x18, 0x40000); // m_nInteractsAs = 1 << LAYER_INDEX_CONTENTS_PLAYER
	WriteU32(filter.bytes + 0x20, 0xFFFFFFFFu); // m_nEntityIdsToIgnore[1]
	WriteU32(filter.bytes + 0x24, 0xFFFFFFFFu); // m_nEntityIdsToIgnore[0]
	WriteU32(filter.bytes + 0x28, 0xFFFFFFFFu); // m_nOwnerIdsToIgnore[1]
	WriteU32(filter.bytes + 0x2C, 0xFFFFFFFFu); // m_nOwnerIdsToIgnore[0]
	WriteU16(filter.bytes + 0x32, 0xFFFFu);		// m_nHierarchyIds[1]
	// The derived constructor's byte image (+0x34 onward).
	filter.bytes[0x36] = 0x0F;
	filter.bytes[0x37] = 0x0F; // m_nObjectSetMask = RNQUERY_OBJECTS_ALL
	filter.bytes[0x38] = 0x08; // m_nCollisionGroup = COLLISION_GROUP_PLAYER
	filter.bytes[0x39] = 0x4B; // m_bHitSolid | m_bHitSolidRequiresGenerateContacts | m_bShouldIgnoreDisabledPairs | m_bUnknown
	filter.bytes[0x3A] = 0x01; // m_bIterateEntities

	if (!pawn) {
		return;
	}
	// Pass entity 1: the pawn; pass owner: whatever owns it; plus the pawn's collision attribute.
	const std::uint32_t pawnHandle = __s2sdk_EntPointerToEntHandle
										 ? static_cast<std::uint32_t>(s2sdk::EntPointerToEntHandle(pawn))
										 : 0u;
	WriteU32(filter.bytes + 0x20, pawnHandle);

	// The owner handle comes from the schema field (a plain 4-byte read, the same value
	// GetEntityOwner() would walk out through the script VM - not something to do per movement).
	int ownerHandle = 0;
	if (m_env.offEntityOwnerHandle >= 0) {
		ownerHandle = Int32At(pawn, m_env.offEntityOwnerHandle);
	} else if (__s2sdk_GetEntityOwner && IsValidHandle(static_cast<std::int32_t>(pawnHandle))) {
		ownerHandle = s2sdk::GetEntityOwner(static_cast<int>(pawnHandle));
	}
	WriteU32(filter.bytes + 0x28, static_cast<std::uint32_t>(ownerHandle));

	// CBaseEntity::m_pCollision is a stored-pointer ("ptr") field: read the pointer value (the
	// original's m_pCollision()), not the field address.
	void* collision = PtrAt(pawn, m_env.offEntityCollision);
	if (!collision) {
		return;
	}
	const auto* attribute = static_cast<const std::uint8_t*>(collision) + m_env.offCollisionAttribute;
	WriteU16(filter.bytes + 0x30,
			 *reinterpret_cast<const std::uint16_t*>(attribute + m_env.offAttributeHierarchyId));
	WriteU64(filter.bytes + 0x08,
			 *reinterpret_cast<const std::uint64_t*>(attribute + m_env.offAttributeInteractsWith));
}

void CRampbugFix::TracePlayerBBox(const Vec3& start, const Vec3& end, const BBox& bounds,
								  const MovementFilter& filter, GameTrace& trace) const {
	if (!m_env.vtableNavPhysics || m_env.idxNavTraceShape < 0) {
		return;
	}
	// The interface has no state of its own: a pointer to its vtable stands in for the object.
	void** vtable = static_cast<void**>(m_env.vtableNavPhysics);
	auto* function = reinterpret_cast<void (*)(void*, const Ray*, const Vec3*, const Vec3*, const void*, GameTrace*)>(
		vtable[m_env.idxNavTraceShape]);
	if (!function) {
		return;
	}
	const Ray ray(bounds.mins, bounds.maxs);
	function(&m_env.vtableNavPhysics, &ray, &start, &end, filter.bytes, &trace);
}

bool CRampbugFix::IsValidMovementTrace(GameTrace& trace, const BBox& bounds,
									   const MovementFilter& filter) const {
	if (trace.startInSolid()) {
		return false;
	}
	// Hit something, but no plane to show for it.
	if (trace.fraction() < 1.0f && std::fabs(trace.hitNormal().x) < FLT_EPSILON &&
		std::fabs(trace.hitNormal().y) < FLT_EPSILON && std::fabs(trace.hitNormal().z) < FLT_EPSILON) {
		return false;
	}
	// A deformed plane.
	if (std::fabs(trace.hitNormal().x) > 1.0f || std::fabs(trace.hitNormal().y) > 1.0f ||
		std::fabs(trace.hitNormal().z) > 1.0f) {
		return false;
	}
	// An unswept trace and a backward one, to be sure.
	GameTrace stuck{};
	TracePlayerBBox(trace.endPos(), trace.endPos(), bounds, filter, stuck);
	if (stuck.startInSolid() || stuck.fraction() < 1.0f - FLT_EPSILON) {
		return false;
	}
	TracePlayerBBox(trace.endPos(), trace.startPos(), bounds, filter, stuck);
	if (stuck.startInSolid()) {
		return false;
	}
	return true;
}

void CRampbugFix::RegisterLanding(PlayerState& state, void* pawn, const Vec3& velocity) {
	if (state.processingMovement && state.move) {
		state.landingOrigin = MoveAbsOrigin(state.move);
	} else {
		state.landingOrigin = GetOrigin(state, pawn);
	}
	state.landingVelocity = velocity;
}

// Landing on a slope: the game clips the landing velocity against the slope and loses speed the
// player should keep.
void CRampbugFix::ApplySlopeFix(PlayerState& state, void* pawn) {
	if (!state.processingMovement || !state.move) {
		return;
	}
	const BBox bounds = GetBounds(pawn ? pawn : nullptr);

	MovementFilter filter{};
	BuildFilter(pawn, filter);
	Vec3 ground = MoveAbsOrigin(state.move);
	ground.z -= 2.0f;
	GameTrace trace{};

	TracePlayerBBox(MoveAbsOrigin(state.move), ground, bounds, filter, trace);

	if (trace.startInSolid() || trace.fraction() == 1.0f || trace.hitNormal().z < kStandableNormalZ ||
		trace.hitNormal().z >= 1.0f) {
		return;
	}
	const float backoff = state.landingVelocity.Dot(trace.hitNormal());
	const Vec3& normal = trace.hitNormal();
	Vec3 newVelocity{state.landingVelocity.x - normal.x * backoff,
						   state.landingVelocity.y - normal.y * backoff,
						   state.landingVelocity.z - normal.z * backoff};

	const float adjust = newVelocity.Dot(trace.hitNormal());
	if (adjust < 0.0f) {
		newVelocity -= trace.hitNormal() * adjust;
	}

	if (newVelocity.Length2D() >= state.landingVelocity.Length2D()) {
		Vec3 velocity = MoveVelocity(state.move);
		velocity.x = newVelocity.x;
		velocity.y = newVelocity.y;
		SetMoveVelocity(state.move, velocity);
		state.landingVelocity.x = newVelocity.x;
		state.landingVelocity.y = newVelocity.y;
	}
}

polyhook::ResultType CRampbugFix::OnProcessMovement(void* services, void* move) {
	void* pawn = nullptr;
	PlayerState* state = GetState(services, &pawn);
	if (!state) {
		return polyhook::ResultType::Ignored;
	}

	state->move = move;
	state->didTPM = false;
	state->processingMovement = true;

	const bool onGround = (FlagsOf(pawn) & kFlagOnGround) != 0;
	if (!state->previousOnGround && onGround) {
		const Vec3 velocity = GetVelocity(*state, pawn);
		RegisterLanding(*state, pawn, velocity);
		ApplySlopeFix(*state, pawn);
	} else if (state->previousOnGround && !onGround) {
		state->takeoffVelocity = GetVelocity(*state, pawn);
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CRampbugFix::OnProcessMovementPost(void* services, void* move) {
	(void)move;
	void* pawn = nullptr;
	PlayerState* state = GetState(services, &pawn);
	if (!state) {
		return polyhook::ResultType::Ignored;
	}
	if (!state->didTPM) {
		state->lastValidPlane = Vec3{};
	}
	state->processingMovement = false;
	state->move = nullptr;
	state->previousOnGround = (FlagsOf(pawn) & kFlagOnGround) != 0;
	return polyhook::ResultType::Ignored;
}

// The pass's own walk along the ramp, redone with every trace nudged off the last valid plane, so
// a trace that stops a hair short of the ramp (or on its edge) cannot hand the game a plane the
// player never hit.
polyhook::ResultType CRampbugFix::OnTryPlayerMove(void* services, void* move, void* firstDest, void* firstTrace,
												  bool* isSurfing) {
	(void)firstDest;
	(void)firstTrace;
	(void)isSurfing;
	void* pawn = nullptr;
	PlayerState* statePtr = GetState(services, &pawn);
	if (!statePtr || !move) {
		return polyhook::ResultType::Ignored;
	}
	PlayerState& state = *statePtr;
	state.overrideTPM = false;
	state.didTPM = true;

	const float frameTime = __s2sdk_GetGameFrameTime ? s2sdk::GetGameFrameTime() : 0.0f;
	float timeLeft = frameTime;

	Vec3 start = GetOrigin(state, pawn);
	Vec3 velocity = GetVelocity(state, pawn);
	Vec3 end;

	if (velocity.Length() == 0.0f) {
		return polyhook::ResultType::Ignored;
	}

	const Vec3 primalVelocity = velocity;
	bool validPlane = false;

	float allFraction = 0.0f;
	GameTrace pm{};
	unsigned bumpCount = 0;
	Vec3 planes[5];
	unsigned numPlanes = 0;
	GameTrace pierce{};

	const BBox bounds = GetBounds(services);
	MovementFilter filter{};
	BuildFilter(pawn, filter);

	bool potentiallyStuck = false;

	for (bumpCount = 0; bumpCount < kMaxBumps; ++bumpCount) {
		// Assume the whole way from the origin to the end point is free.
		VectorMA(start, timeLeft, velocity, end);

		// The game's own first trace may already cover this segment.
		const auto* firstDestPoint = static_cast<const Vec3*>(firstDest);
		if (firstDestPoint && firstTrace && end == *firstDestPoint) {
			pm = *static_cast<const GameTrace*>(firstTrace);
		} else {
			TracePlayerBBox(start, end, bounds, filter, pm);
			if (end == start) {
				continue;
			}

			if (IsValidMovementTrace(pm, bounds, filter) && pm.fraction() == 1.0f) {
				break; // nothing in the way
			}

		if (state.lastValidPlane.Length() > FLT_EPSILON &&
			(!IsValidMovementTrace(pm, bounds, filter) ||
			 pm.hitNormal().Dot(state.lastValidPlane) < kRampBugThreshold ||
			 (potentiallyStuck && pm.fraction() == 0.0f))) {
			// A plane that would change the velocity a lot: make sure it is a real one.
			Vec3 offsetDirection;
			const float offsets[] = {0.0f, -1.0f, 1.0f};
			bool success = false;
			for (unsigned i = 0; i < 3 && !success; ++i) {
				for (unsigned j = 0; j < 3 && !success; ++j) {
					for (unsigned k = 0; k < 3 && !success; ++k) {
						if (i == 0 && j == 0 && k == 0) {
							offsetDirection = state.lastValidPlane;
						} else {
							offsetDirection = Vec3{offsets[i], offsets[j], offsets[k]};
							if (state.lastValidPlane.Dot(offsetDirection) <= 0.0f) {
								continue;
							}
							GameTrace test{};
							TracePlayerBBox(start + offsetDirection * kRampPierceDistance, start, bounds, filter,
											test);
							if (!IsValidMovementTrace(test, bounds, filter)) {
								continue;
							}
						}

						bool goodTrace = false;
						float ratio = 0.0f;
						bool hitNewPlane = false;
						for (ratio = 0.25f; ratio <= 1.0f; ratio += 0.25f) {
							TracePlayerBBox(start + offsetDirection * (kRampPierceDistance * ratio),
											end + offsetDirection * (kRampPierceDistance * ratio), bounds, filter,
											pierce);
							if (!IsValidMovementTrace(pierce, bounds, filter)) {
								continue;
							}
							// Until a similar plane is hit.
							validPlane = pierce.fraction() < 1.0f && pierce.fraction() > 0.1f &&
										 pierce.hitNormal().Dot(state.lastValidPlane) >= kRampBugThreshold;
							hitNewPlane = pm.hitNormal().Dot(pierce.hitNormal()) < kNewRampThreshold &&
										  state.lastValidPlane.Dot(pierce.hitNormal()) > kNewRampThreshold;
							goodTrace = CloseEnough(pierce.fraction(), 1.0f, FLT_EPSILON) || validPlane;
							if (goodTrace) {
								break;
							}
						}

						if (goodTrace || hitNewPlane) {
							// Back to the original end point, for its normal.
							GameTrace test{};
							TracePlayerBBox(pierce.endPos(), end, bounds, filter, test);
							pm = pierce;
							pm.startPos() = start;
							const float startToEnd = (end - start).Length();
							pm.fraction() = startToEnd > 0.0f
												? Clamp((pierce.endPos() - pierce.startPos()).Length() / startToEnd,
														0.0f, 1.0f)
												: 0.0f;
							pm.endPos() = test.endPos();
							if (pierce.hitNormal().Length() > 0.0f) {
								pm.hitNormal() = pierce.hitNormal();
								state.lastValidPlane = pierce.hitNormal();
							} else {
								pm.hitNormal() = test.hitNormal();
								state.lastValidPlane = test.hitNormal();
							}
							success = true;
							state.overrideTPM = true;
						}
					}
				}
			}
		}

		if (pm.hitNormal().Length() > 0.99f) {
			state.lastValidPlane = pm.hitNormal();
		}

		potentiallyStuck = pm.fraction() == 0.0f;
		}

		if (pm.fraction() * velocity.Length() > 0.03125f || pm.fraction() > 0.03125f) {
			allFraction += pm.fraction();
			start = pm.endPos();
			numPlanes = 0;
		}

		if (allFraction == 1.0f) {
			break;
		}

		timeLeft -= frameTime * pm.fraction();

		if (numPlanes >= 5 || (pm.hitNormal().z >= kStandableNormalZ && velocity.Length2D() < 1.0f)) {
			velocity = Vec3{};
			break;
		}

		planes[numPlanes] = pm.hitNormal();
		numPlanes++;

		if (numPlanes == 1 && MoveTypeOf(pawn) == kMoveTypeWalk &&
			(!pawn || m_offPawnGroundEntity < 0 ||
			 Int32At(pawn, m_offPawnGroundEntity) == kInvalidEntityHandle)) { // no ground entity
			// ClipVelocity along a single plane.
			float backoff = -velocity.Dot(planes[0]);
			backoff = std::fmax(backoff, 0.0f) + 0.03125f;
			velocity = planes[0] * backoff + velocity;
		} else {
			unsigned i = 0;
			unsigned j = 0;
			for (i = 0; i < numPlanes; i++) {
				{
					float backoff = -velocity.Dot(planes[i]);
					backoff = std::fmax(backoff, 0.0f) + 0.03125f;
					velocity = planes[i] * backoff + velocity;
				}
				for (j = 0; j < numPlanes; j++) {
					if (j != i && velocity.Dot(planes[j]) < 0) {
						break; // moving against this plane now
					}
				}
				if (j == numPlanes) {
					break; // no clip needed
				}
			}

			if (i == numPlanes) {
				// Along the crease.
				if (numPlanes != 2) {
					velocity = Vec3{};
					break;
				}
				Vec3 dir = CrossProduct(planes[0], planes[1]);
				dir.NormalizeInPlace();
				const float d = dir.Dot(velocity);
				velocity = dir * d;
				if (velocity.Dot(primalVelocity) <= 0.0f) {
					velocity = Vec3{};
					break;
				}
			}
		}
	}

	state.tpmOrigin = pm.endPos();
	state.tpmVelocity = velocity;
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CRampbugFix::OnTryPlayerMovePost(void* services, void* move, void* firstDest, void* firstTrace,
													  bool* isSurfing) {
	(void)move;
	(void)firstDest;
	(void)firstTrace;
	(void)isSurfing;
	void* pawn = nullptr;
	PlayerState* state = GetState(services, &pawn);
	if (!state) {
		return polyhook::ResultType::Ignored;
	}

	const Vec3 velocity = GetVelocity(*state, pawn);

	// The game took the player somewhere our own walk did not: put ours back.
	const bool velocityHeavilyModified =
		state->tpmVelocity.Normalized().Dot(velocity.Normalized()) < kRampBugThreshold ||
		(state->tpmVelocity.Length() > 50.0f && velocity.Length() / state->tpmVelocity.Length() <
													kRampBugVelocityThreshold);

	if (state->overrideTPM && velocityHeavilyModified) {
		SetOrigin(*state, pawn, state->tpmOrigin);
		SetVelocity(*state, pawn, state->tpmVelocity);
	}
	return polyhook::ResultType::Ignored;
}

// Going down a ramp fast, the ground check can find a standable plane the player is not really
// on; nudge them onto the last valid plane first.
polyhook::ResultType CRampbugFix::OnCategorizePosition(void* services, void* move, bool stayOnGround) {
	(void)move;
	void* pawn = nullptr;
	PlayerState* statePtr = GetState(services, &pawn);
	if (!statePtr) {
		return polyhook::ResultType::Ignored;
	}
	PlayerState& state = *statePtr;

	// Already colliding with a standable, valid plane: nothing to check.
	if (stayOnGround || state.lastValidPlane.Length() < 0.000001f || state.lastValidPlane.z > kStandableNormalZ) {
		return polyhook::ResultType::Ignored;
	}

	const Vec3 velocity = GetVelocity(state, pawn);

	// Only worth it going down at some speed.
	if (velocity.z > -64.0f) {
		return polyhook::ResultType::Ignored;
	}

	const BBox bounds = GetBounds(services);
	MovementFilter filter{};
	BuildFilter(pawn, filter);
	GameTrace trace{};

	Vec3 origin = GetOrigin(state, pawn);
	Vec3 groundOrigin = origin;
	groundOrigin.z -= 2.0f;

	TracePlayerBBox(origin, groundOrigin, bounds, filter, trace);

	if (trace.fraction() == 1.0f) {
		return polyhook::ResultType::Ignored;
	}

	// Something the player could actually stand on?
	if (trace.fraction() < 0.95f && trace.hitNormal().z > kStandableNormalZ &&
		state.lastValidPlane.Dot(trace.hitNormal()) < kRampBugThreshold) {
		origin += state.lastValidPlane * 0.0625f;
		groundOrigin = origin;
		groundOrigin.z -= 2.0f;
		TracePlayerBBox(origin, groundOrigin, bounds, filter, trace);

		if (trace.startInSolid()) {
			return polyhook::ResultType::Ignored;
		}
		if (trace.fraction() == 1.0f || state.lastValidPlane.Dot(trace.hitNormal()) >= kRampBugThreshold) {
			SetOrigin(state, pawn, origin);
		}
	}
	return polyhook::ResultType::Ignored;
}

} // namespace

std::unique_ptr<CFix> MakeRampbugFix(Environment& env) {
	return std::make_unique<CRampbugFix>(env);
}

} // namespace game_fixes
