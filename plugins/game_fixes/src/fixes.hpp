// game_fixes — the fix interface, mirroring the fork's fix.h.
//
// One class per fix; plugin.cpp creates them, reads each one's block from game_fixes.ini and
// calls Load(). A fix that cannot find what it needs logs why and stays off, the rest still load.

#pragma once

#include <memory>

#include "api.hpp"

namespace pisex {
class KeyValues;
}

namespace game_fixes {

// Movement hooks: ProcessMovement / TryPlayerMove / CategorizePosition / WaterMove are engine
// functions several fixes want (rampbug needs all but WaterMove, water_jump needs WaterMove and
// CategorizePosition). The plugin installs one detour per function and dispatches to every
// listener, so two fixes on the same function never race for the hook.
class IMovementListener {
public:
	virtual ~IMovementListener() = default;

	virtual polyhook::ResultType OnProcessMovement(void* services, void* move) {
		(void)services;
		(void)move;
		return polyhook::ResultType::Ignored;
	}
	virtual polyhook::ResultType OnProcessMovementPost(void* services, void* move) {
		(void)services;
		(void)move;
		return polyhook::ResultType::Ignored;
	}
	virtual polyhook::ResultType OnTryPlayerMove(void* services, void* move, void* firstDest, void* firstTrace,
												 bool* isSurfing) {
		(void)services;
		(void)move;
		(void)firstDest;
		(void)firstTrace;
		(void)isSurfing;
		return polyhook::ResultType::Ignored;
	}
	virtual polyhook::ResultType OnTryPlayerMovePost(void* services, void* move, void* firstDest, void* firstTrace,
													 bool* isSurfing) {
		(void)services;
		(void)move;
		(void)firstDest;
		(void)firstTrace;
		(void)isSurfing;
		return polyhook::ResultType::Ignored;
	}
	virtual polyhook::ResultType OnCategorizePosition(void* services, void* move, bool stayOnGround) {
		(void)services;
		(void)move;
		(void)stayOnGround;
		return polyhook::ResultType::Ignored;
	}
	virtual polyhook::ResultType OnWaterMove(void* services, void* move) {
		(void)services;
		(void)move;
		return polyhook::ResultType::Ignored;
	}
};

class CFix {
public:
	explicit CFix(Environment& env) : m_env(env) {}
	virtual ~CFix() = default;

	// Non-null when the fix wants the shared movement hooks.
	virtual IMovementListener* Movement() { return nullptr; }

	// True when Load() needs the schema offsets (CBaseEntity/CGameSceneNode/... fields). The
	// schema answers only once a map is running, so such a fix is not loaded at plugin start but
	// retried from OnPluginUpdate as soon as the offsets resolve (the original Metamod plugin had
	// them available at load).
	virtual bool NeedsSchema() const { return false; }

	// Key of this fix's block in game_fixes.ini; also its prefix in the log.
	virtual const char* Name() const = 0;

	// Reads the fix's own options from its ini block (only called when enabled).
	virtual void ReadConfig(const pisex::KeyValues& config) { (void)config; }

	// Installs the fix; on failure logs why and returns false (the plugin keeps loading).
	virtual bool Load() = 0;
	virtual void Unload() {}

	// Forwarded from the plugin's own engine hooks.
	// OnStartupServerConfig runs on the Pre hook of INetworkServerService::StartupServer while the
	// session configuration is still editable (that is where demo_record raises maxplayers), then
	// OnStartupServer hands every fix the new map name for its per-map resets.
	virtual void OnStartupServerConfig(void* config) { (void)config; }
	virtual void OnStartupServer(const char* mapName) { (void)mapName; }

protected:
	Environment& m_env;
};

// Factories (one TU per fix).
std::unique_ptr<CFix> MakeBeamCrashFix(Environment& env);
std::unique_ptr<CFix> MakeDemoRecordFix(Environment& env);
std::unique_ptr<CFix> MakeHammerIdFix(Environment& env);
std::unique_ptr<CFix> MakeSlowAnimationFix(Environment& env);
std::unique_ptr<CFix> MakeWorkshopVoiceFix(Environment& env);
std::unique_ptr<CFix> MakeSteamBanFix(Environment& env);
std::unique_ptr<CFix> MakeTeamLimitFix(Environment& env);
std::unique_ptr<CFix> MakeInputActivatorCrashFix(Environment& env);
std::unique_ptr<CFix> MakeSvCheatsFix(Environment& env);
std::unique_ptr<CFix> MakeRampbugFix(Environment& env);
std::unique_ptr<CFix> MakeWaterJumpFix(Environment& env);
std::unique_ptr<CFix> MakeKeyValuesFloodFix(Environment& env);
std::unique_ptr<CFix> MakeTeamSwitchSpamFix(Environment& env);
std::unique_ptr<CFix> MakeVoiceFloodFix(Environment& env);

} // namespace game_fixes
