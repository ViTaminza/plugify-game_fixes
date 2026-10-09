// game_fixes — shared infrastructure for the ported fixes.
//
// Everything here is bookkeeping the individual fixes (src/fixes/*.cpp) share: logging, the
// s2sdk gamedata handle, polyhook plumbing and the engine-structure offsets/signatures resolved
// once at plugin start. The hook *handlers* live in the fix files; this header only knows how to
// install them.
//
// All of it runs on the game thread.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <pisex/paths.hpp>

namespace game_fixes {

// ---------------------------------------------------------------- logging ------

inline void Log(std::string_view message, plg::Severity severity = plg::Severity::Info) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// "game_fixes: <fix>: <message>" — the prefix the original logs with through its own logger.
inline void LogFix(std::string_view fix, std::string_view message,
				   plg::Severity severity = plg::Severity::Info) {
	Log(std::string("game_fixes: ") + std::string(fix) + ": " + std::string(message), severity);
}

// printf-style on plg::string boundaries (avoids <format> for hot paths).
inline std::string Format(const char* fmt, ...) {
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return std::string(buffer);
}

// ------------------------------------------------------------ small helpers ----

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntPointerToEntHandle
// answers -1 for a pointer that is not a live entity). The sign means nothing - once the engine's
// serial counter passes 65535 bit 31 is set and a valid handle is a negative int; a `h > 0` test
// silently treats those entities as absent. (Not to be confused with the m_off* offsets in
// Environment, which are schema byte offsets and do use `< 0` as their "unresolved" value.)
inline bool IsValidHandle(std::int32_t h) { return h != -1 && h != 0; }

// Case-insensitive compare of the first n characters (the engine's own command lookup is
// case-insensitive, so anything comparing names has to be too).
inline bool EqualsIgnoreCaseN(std::string_view left, std::string_view right, std::size_t count) {
	if (left.size() < count || right.size() < count) {
		return false;
	}
	for (std::size_t i = 0; i < count; ++i) {
		unsigned char a = static_cast<unsigned char>(left[i]);
		unsigned char b = static_cast<unsigned char>(right[i]);
		if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
		if (a != b) {
			return false;
		}
	}
	return true;
}

// Sets the "handled" return value a Supercede needs: polyhook skips the original function, so
// without this the caller reads whatever the return register happened to hold. Every Supersede
// on a non-void function must call it (the fork's Supersede always hands back true).
inline void SetSupersededReturn(polyhook::ReturnHandle ret, bool value = true) {
	if (ret == nullptr || __polyhook_SetReturnBool == nullptr) {
		return;
	}
	try {
		polyhook::SetReturnBool(ret, value);
	} catch (...) {
	}
}

// Raw memory accessors for the engine structures that have no s2sdk accessor (plain structs the
// fixes read at fixed, gamedata-verified offsets).
inline void* PtrAt(void* base, std::int32_t offset) {
	return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset);
}
inline std::int32_t Int32At(const void* base, std::int32_t offset) {
	return *reinterpret_cast<const std::int32_t*>(static_cast<const std::uint8_t*>(base) + offset);
}
inline void SetInt32At(void* base, std::int32_t offset, std::int32_t value) {
	*reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(base) + offset) = value;
}
inline std::uint64_t UInt64At(const void* base, std::int32_t offset) {
	return *reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(base) + offset);
}
inline void SetUInt64At(void* base, std::int32_t offset, std::uint64_t value) {
	*reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(base) + offset) = value;
}
inline bool BoolAt(const void* base, std::int32_t offset) {
	return *reinterpret_cast<const bool*>(static_cast<const std::uint8_t*>(base) + offset);
}

// ------------------------------------------------------------- gamedata --------

// Resolved once at plugin start from assets/gamedata.jsonc through the s2sdk game config API.
struct Gamedata {
	s2sdk::ConfigId id = 0;

	void* Signature(const char* name) const {
		if (!id || !__s2sdk_GetGameConfigSignature) {
			return nullptr;
		}
		return s2sdk::GetGameConfigSignature(id, name);
	}
	void* VTable(const char* name) const {
		if (!id || !__s2sdk_GetGameConfigVTable) {
			return nullptr;
		}
		return s2sdk::GetGameConfigVTable(id, name);
	}
	std::int32_t Offset(const char* name, std::int32_t fallback = -1) const {
		if (!id || !__s2sdk_GetGameConfigOffset) {
			return fallback;
		}
		return s2sdk::GetGameConfigOffset(id, name);
	}

	bool Load() {
		if (!__s2sdk_LoadGameConfigFile || !__s2sdk_GetGameConfigOffset) {
			return false;
		}
		// The plugin's own gamedata ships inside the package; where that is depends on how the
		// plugin was installed, so the paths are derived from the module location the way the
		// other plugins of this repo do it (GetLocation may be the package root or bin/). The
		// plain relative names stay as a fallback for a hand-rolled layout.
		plg::vector<plg::string> paths;
		std::string location;
		try {
			if (plg::plugin::GetLocation) {
				location = std::string(std::string_view(plg::plugin::GetLocation(plg::plugin::handle)));
			}
		} catch (...) {
		}
		if (!location.empty()) {
			const std::string base = location.substr(0, location.find_last_of('/') + 1);
			for (const std::string& candidate : {
					 base + "gamedata.jsonc",
					 base + "assets/gamedata.jsonc",
					 base + "../gamedata.jsonc",
					 base + "../assets/gamedata.jsonc",
					 base + "game_fixes/gamedata.jsonc",
				 }) {
				if (pisex::FileExists(candidate)) {
					paths.push_back(candidate);
				}
			}
		}
		paths.push_back("game_fixes/gamedata.jsonc");
		paths.push_back("assets/gamedata.jsonc");
		try {
			id = s2sdk::LoadGameConfigFile(paths);
		} catch (...) {
			id = 0;
		}
		return id != 0;
	}
	// ref/docs/live/plugins_s2sdk_guides_gamedata.txt:1331 - "Always close gamedata files - Use
	// CloseGameConfigFile() in OnPluginEnd() to prevent memory leaks".
	void Close() {
		if (id != 0 && __s2sdk_CloseGameConfigFile) {
			try {
				s2sdk::CloseGameConfigFile(id);
			} catch (...) {
			}
		}
		id = 0;
	}
};

// -------------------------------------------------------------- polyhook -------

// polyhook keys virtual hooks on (class pointer, index). Two families exist and the plugin uses
// them exactly like s2sdk does:
//   * HookVirtualFunc*  — "class pointer" is the *vtable address* (resolved from RTTI through
//                         the game config); the entry in the shared vtable is swapped in place.
//                         Used for the CServerSideClient virtuals.
//   * HookVirtualTable* — "class pointer" is an *object*; a copy of its vtable is swapped in for
//                         that object. Used for the engine interfaces (INetworkServerService).
// A hook that already exists (created by s2sdk or an earlier fix of ours) is reused and only
// gets the callback added, which is what polyhook's Find* functions are for.
namespace hook {

using CallbackHandler = polyhook::CallbackHandler;

// Itanium ABI encoded pointer-to-virtual-member-function: 1 + 8*index. polyhook's Hook*2
// functions decode the index from exactly this (see polyhook 2.5.6 getVirtualIndex()).
inline void* EncodedIndex(std::int32_t index) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(8ull * static_cast<std::uint32_t>(index) + 1u));
}

// What an install produced. `created` is true only when this plugin created the hook (the same
// vtable slot or address may already be hooked by s2sdk or another plugin, in which case the
// hook is shared). Teardown must remove our callback either way but may only *unhook* what it
// created - unhooking a shared hook would disarm the other owner.
struct HookRef {
	polyhook::HookHandle handle = nullptr;
	bool created = false;

	explicit operator bool() const { return handle != nullptr; }
};

// Virtual hook on a vtable address; adds Pre/Post callbacks (either may be null).
HookRef InstallVFunc(void* vtableAddress, std::int32_t index, polyhook::DataType returnType,
					 const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					 CallbackHandler post, const char* name);

// Virtual hook on an object (interface); reuses s2sdk's hook when it already exists.
HookRef InstallVTable(void* object, std::int32_t index, polyhook::DataType returnType,
					  const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					  CallbackHandler post, const char* name);

// Plain function detour (signature-resolved address).
HookRef InstallDetour(void* address, polyhook::DataType returnType,
					  const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					  CallbackHandler post, const char* name);

// Teardown helpers: the callback is dropped from the shared hook; the hook itself is only
// unhooked when this plugin created it (and when it still exists).
void RemoveCallback(const HookRef& ref, polyhook::CallbackType type, CallbackHandler callback);
void UnhookVFunc(const HookRef& ref, void* vtableAddress, std::int32_t index);
void UnhookVTable(const HookRef& ref, void* object, std::int32_t index);
void UnhookDetour(const HookRef& ref, void* address);

} // namespace hook

// ---------------------------------------------------------------- hooks.h ------

// What the plugin resolved at load time, shared by every fix.
struct Environment {
	Gamedata gamedata;
	// vtable addresses / function addresses (null when the resolution failed; the fix is then off)
	void* vtableServerSideClient = nullptr;
	void* vtableTraceFilter = nullptr;
	void* vtableNavPhysics = nullptr;
	void* interfaceNetworkServerService = nullptr;
	void* fnProcessMovement = nullptr;
	void* fnTryPlayerMove = nullptr;
	void* fnCategorizePosition = nullptr;
	void* fnWaterMove = nullptr;
	void* fnAcceptInput = nullptr;
	void* fnCheckSteamBan = nullptr;
	void* banMap = nullptr;
	void* fnSetBeamOrigin = nullptr;
	void* fnSetBeamEndPos = nullptr;
	// offsets
	std::int32_t idxStartupServer = -1;
	std::int32_t idxDisconnect = -1;
	std::int32_t idxSendNetMessage = -1;
	std::int32_t idxProcessStringCmd = -1;
	std::int32_t idxProcessCmdKeyValues = -1;
	std::int32_t idxProcessVoiceData = -1;
	std::int32_t idxNavTraceShape = -1;
	std::int32_t offClientSlot = -1;
	std::int32_t offSignonState = -1;
	std::int32_t offFakePlayer = -1;
	std::int32_t offUserId = -1;
	std::int32_t offIsHLTV = -1;
	std::int32_t signonStateConnected = -1;
	// SVC_Messages::svc_VoiceData (protocol constant; the registry lookup is preferred when it
	// answers, this is the fallback for the window before the server registers its messages)
	std::int32_t offVoiceDataMessageId = 47;
	std::int32_t offSessionMaxPlayers = -1;
	std::int32_t offMoveVelocity = -1;
	std::int32_t offMoveAbsOrigin = -1;
	// protobuf message layouts of the client messages the flood fixes inspect (offsets relative
	// to the message object the vtable method receives; see README.md for the provenance)
	std::int32_t offVoiceHasBits = -1;
	std::int32_t offVoiceAudio = -1;
	std::int32_t offVoicePacketOffsets = -1;
	std::int32_t offVoiceVoiceData = -1;
	std::int32_t offCmdKeyValuesData = -1;
	// schema offsets used by the trace filter and other fixes (resolved lazily where possible)
	std::int32_t offEntityBodyComponent = -1; // CBaseEntity::m_CBodyComponent
	std::int32_t offBodySceneNode = -1;		  // CBodyComponent::m_pSceneNode
	std::int32_t offSceneNodeParent = -1;	  // CGameSceneNode::m_pParent
	std::int32_t offSceneNodeAbsOrigin = -1;  // CGameSceneNode::m_vecAbsOrigin
	std::int32_t offEntityOwnerHandle = -1;	  // CBaseEntity::m_hOwnerEntity
	std::int32_t offEntityCollision = -1;	  // CBaseEntity::m_pCollision
	std::int32_t offCollisionAttribute = -1;  // CCollisionProperty::m_collisionAttribute
	std::int32_t offAttributeHierarchyId = -1; // VPhysicsCollisionAttribute_t::m_nHierarchyId
	std::int32_t offAttributeInteractsWith = -1; // VPhysicsCollisionAttribute_t::m_nInteractsWith

	bool Resolve();
};

// Fills the schema offsets above (they need the schema system, which is only up after a map).
void ResolveSchemaOffsets(Environment& env);

// True once every schema offset the fixes read has been resolved. They resolve together (one
// call fills them all), and s2sdk caches a failed class lookup as empty for the process, so a
// partial result is a failure, not a progress state.
bool SchemaOffsetsReady(const Environment& env);

} // namespace game_fixes
