// player_ghost — a player can end up listed as a spectator while their body stays alive on the
// map (the controller moved to spectate, the pawn did not), letting them walk around and be shot
// like a normal player. At round_freeze_end every alive player whose PAWN team is Spectator is
// pushed through ChangeTeam(None) and then ChangeTeam(Spectator), which clears the leftover body
// and puts the player properly into spectate.
//
// This is the reference PlayerGhostFix (based on Shmitzas' AntiPlayerGhost) and uses the same
// two calls: CCSPlayerController::ChangeTeam, which s2sdk exposes as ChangeClientTeam with its
// own gamedata offset. The pawn team is read through s2sdk's handle API (GetEntityTeam reads
// m_iTeamNum on the pawn, not the controller's team), and the client is re-checked between the
// two calls: a disconnect or map change in between must not reach the second one.

#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../player_ghost_logic.hpp"

namespace game_fixes {
namespace {

class CPlayerGhostFix final : public CFix {
public:
	explicit CPlayerGhostFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "player_ghost"; }

	bool Load() override;
	void Unload() override;

private:
	static s2sdk::ResultType OnRoundFreezeEnd(const plg::string& name, void* event, bool dontBroadcast);

	static CPlayerGhostFix* s_instance;
	bool m_hooked = false;
};

CPlayerGhostFix* CPlayerGhostFix::s_instance = nullptr;

s2sdk::ResultType CPlayerGhostFix::OnRoundFreezeEnd(const plg::string&, void*, bool) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return s2sdk::ResultType::Continue;
		}
		// The event also fires around a changelevel; client queries would walk the emptied list.
		if (!IsMapReady()) {
			return s2sdk::ResultType::Continue;
		}
		if (!__s2sdk_GetMaxClients || !__s2sdk_PlayerSlotToClientPtr || !__s2sdk_IsClientInGame ||
			!__s2sdk_IsClientAlive || !__s2sdk_GetClientPawn || !__s2sdk_EntPointerToEntHandle ||
			!__s2sdk_GetEntityTeam || !__s2sdk_ChangeClientTeam) {
			return s2sdk::ResultType::Continue;
		}
		const int maxClients = s2sdk::GetMaxClients();
		int fixed = 0;
		for (int slot = 0; slot < maxClients; ++slot) {
			// Silent, Count-checked lookup first; every other s2sdk client helper is unchecked.
			void* client = s2sdk::PlayerSlotToClientPtr(slot);
			if (!client || !s2sdk::IsClientInGame(slot) || !s2sdk::IsClientAlive(slot)) {
				continue;
			}
			void* pawn = s2sdk::GetClientPawn(slot);
			if (!pawn) {
				continue;
			}
			const int handle = s2sdk::EntPointerToEntHandle(pawn);
			if (!IsValidHandle(handle)) {
				continue;
			}
			const auto pawnTeam = static_cast<std::int32_t>(s2sdk::GetEntityTeam(handle));
			if (!playerghost::IsGhostCandidate(true, true, true, pawnTeam)) {
				continue;
			}
			// None first: the engine removes the body and puts the controller into the spectator
			// flow; Spectator then makes the state consistent for the scoreboard.
			s2sdk::ChangeClientTeam(slot, s2sdk::CSTeam::None);
			// Re-check between the calls: the same client must still hold the slot. IsClientConnected
			// rather than IsClientInGame, because the None transition itself may briefly drop the
			// in-game state and the player would then be left on team None.
			void* afterFirst = s2sdk::PlayerSlotToClientPtr(slot);
			if (afterFirst != client || (__s2sdk_IsClientConnected && !s2sdk::IsClientConnected(slot))) {
				continue;
			}
			s2sdk::ChangeClientTeam(slot, s2sdk::CSTeam::Spectator);
			++fixed;
		}
		if (fixed > 0) {
			LogFix(fix->Name(), Format("moved %d ghost player(s) out of their leftover body", fixed));
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

bool CPlayerGhostFix::Load() {
	if (!__s2sdk_HookEvent || !__s2sdk_ChangeClientTeam) {
		LogFix(Name(), "event/client bindings are missing", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		const s2sdk::EventHookError error =
			s2sdk::HookEvent("round_freeze_end", &CPlayerGhostFix::OnRoundFreezeEnd, s2sdk::HookMode::Pre);
		m_hooked = error == s2sdk::EventHookError::Okay;
	} catch (...) {
		m_hooked = false;
	}
	if (!m_hooked) {
		s_instance = nullptr;
		LogFix(Name(), "hooking round_freeze_end failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "listening for round_freeze_end");
	return true;
}

void CPlayerGhostFix::Unload() {
	if (m_hooked && __s2sdk_UnhookEvent) {
		s2sdk::UnhookEvent("round_freeze_end", &CPlayerGhostFix::OnRoundFreezeEnd, s2sdk::HookMode::Pre);
	}
	m_hooked = false;
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakePlayerGhostFix(Environment& env) {
	return std::make_unique<CPlayerGhostFix>(env);
}

} // namespace game_fixes
