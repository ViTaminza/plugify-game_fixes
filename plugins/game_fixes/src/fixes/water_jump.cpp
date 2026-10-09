// water_jump — touching the bottom of a body of water, the CategorizePosition after WaterMove
// counts the player as standing on it and a jump out of the water no longer takes. That one
// CategorizePosition is skipped, so the player stays in the water state and can jump out.
//
// The per-slot "skip the next CategorizePosition" flag lives here; the hooks themselves are the
// shared movement hooks (rampbug wants CategorizePosition too, so the plugin installs one detour
// and each listener gets the call).

#include "../fixes.hpp"

namespace game_fixes {
namespace {

class CWaterJumpFix final : public CFix, public IMovementListener {
public:
	explicit CWaterJumpFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "water_jump"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

	IMovementListener* Movement() override { return this; }

	polyhook::ResultType OnWaterMove(void* services, void* move) override;
	polyhook::ResultType OnCategorizePosition(void* services, void* move, bool stayOnGround) override;

private:
	// Which slot a movement service belongs to; -1 for anything that is not a player.
	int GetSlot(void* services) const;

	// Per slot: WaterMove ran, the next CategorizePosition is to be skipped.
	bool m_skipNextCategorize[64] = {};
};

bool CWaterJumpFix::Load() {
	if (!__s2sdk_PlayerServicesToPlayerSlot) {
		LogFix(Name(), "s2sdk client lookup is missing", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "installed (WaterMove and CategorizePosition hooks)");
	return true;
}

void CWaterJumpFix::Unload() {}

void CWaterJumpFix::OnStartupServer(const char* mapName) {
	(void)mapName;
	std::memset(m_skipNextCategorize, 0, sizeof(m_skipNextCategorize));
}

int CWaterJumpFix::GetSlot(void* services) const {
	if (!services) {
		return -1;
	}
	return s2sdk::PlayerServicesToPlayerSlot(services);
}

polyhook::ResultType CWaterJumpFix::OnWaterMove(void* services, void* move) {
	(void)move;
	const int slot = GetSlot(services);
	if (slot >= 0 && slot < 64) {
		m_skipNextCategorize[slot] = true;
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CWaterJumpFix::OnCategorizePosition(void* services, void* move, bool stayOnGround) {
	(void)move;
	(void)stayOnGround;
	const int slot = GetSlot(services);
	if (slot < 0 || slot >= 64 || !m_skipNextCategorize[slot]) {
		return polyhook::ResultType::Ignored;
	}
	m_skipNextCategorize[slot] = false;
	return polyhook::ResultType::Supercede;
}

} // namespace

std::unique_ptr<CFix> MakeWaterJumpFix(Environment& env) {
	return std::make_unique<CWaterJumpFix>(env);
}

} // namespace game_fixes
