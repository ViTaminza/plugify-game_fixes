// input_activator_crash — CBaseFilter::InputTestActivator dereferences the input's activator
// without checking it; a TestActivator input fired with none (a map's output wired straight to a
// filter) takes the server down. Calls without an activator are dropped.
//
// The fork hooked CEntityIdentity::AcceptInput by signature. This port hooks the *entry* point
// CEntityInstance::AcceptInput instead: s2sdk maintains that signature per game build (it is what
// s2sdk's own AcceptInput binding calls), its arguments are the documented
// (self, inputName, activator, caller, value, outputID, ...) and the class name comes from
// s2sdk's GetEntityClassname, so no engine layout is involved.

#include "../fixes.hpp"

namespace game_fixes {
namespace {

bool StartsWithFilter(std::string_view name) {
	return EqualsIgnoreCaseN(name, "filter_", 7);
}

class CInputActivatorCrashFix final : public CFix {
public:
	explicit CInputActivatorCrashFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "input_activator_crash"; }

	bool Load() override;
	void Unload() override;

private:
	static polyhook::ResultType AcceptInputPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
											   int32_t count, polyhook::ReturnHandle ret,
											   polyhook::CallbackType type);

	void* m_address = nullptr;
	hook::HookRef m_hook;
};

polyhook::ResultType CInputActivatorCrashFix::AcceptInputPre(polyhook::HookHandle, polyhook::ParametersHandle params,
															 int32_t, polyhook::ReturnHandle,
															 polyhook::CallbackType) {
	try {
		if (!params || !__s2sdk_GetEntityClassname || !__s2sdk_EntPointerToEntHandle) {
			return polyhook::ResultType::Ignored;
		}
		auto* self = polyhook::GetArgumentPointer(params, 0);
		const auto* inputName = static_cast<const char*>(polyhook::GetArgumentPointer(params, 1));
		auto* activator = polyhook::GetArgumentPointer(params, 2);
		if (!self || !inputName || activator) {
			return polyhook::ResultType::Ignored;
		}
		// The engine's string compare is case-insensitive; do the same or the filter is bypassed.
		// Checked before the class lookup: this is the cheap half of the test.
		std::string_view input(inputName);
		if (input.size() != std::string_view("TestActivator").size() ||
			!EqualsIgnoreCaseN(input, "TestActivator", input.size())) {
			return polyhook::ResultType::Ignored;
		}
		// EntPointerToEntHandle returns INVALID_EHANDLE_INDEX (-1) for a pointer that is not a
		// live entity. IsValidHandle (not `< 0`) because a live entity's handle is negative once
		// the engine's serial counter has passed 65535.
		const int handle = s2sdk::EntPointerToEntHandle(self);
		if (!IsValidHandle(handle)) {
			return polyhook::ResultType::Ignored;
		}
		const plg::string className = s2sdk::GetEntityClassname(handle);
		if (!StartsWithFilter(std::string_view(className))) {
			return polyhook::ResultType::Ignored;
		}
		// CEntityInstance::AcceptInput returns void; dropping the call is all this fix has to do.
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

bool CInputActivatorCrashFix::Load() {
	// The signature is in this plugin's own gamedata: s2sdk's GetGameConfigSignatureAll gives up
	// at the first loaded config that does not carry the name, so which config it finds the
	// signature in depends on the order the configs are iterated in.
	m_address = m_env.gamedata.Signature("CEntityInstance_AcceptInput");
	if (!m_address) {
		LogFix(Name(), "CEntityInstance::AcceptInput not found in the game config", plg::Severity::Warning);
		return false;
	}
	// void CEntityInstance::AcceptInput(CEntityInstance* self, const char* inputName,
	//     CEntityInstance* activator, CEntityInstance* caller, variant_t* value, int outputId,
	//     void*) - s2sdk's own typedef for this signature (globals.hpp).
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
											   polyhook::DataType::Pointer, polyhook::DataType::Pointer,
											   polyhook::DataType::Pointer, polyhook::DataType::Int32,
											   polyhook::DataType::Pointer};
	m_hook = hook::InstallDetour(m_address, polyhook::DataType::Void, args,
								 &CInputActivatorCrashFix::AcceptInputPre, nullptr, "GameFixes_input_AcceptInput");
	if (!m_hook) {
		LogFix(Name(), "hooking CEntityInstance::AcceptInput failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("hooked CEntityInstance::AcceptInput (%p)", m_address));
	return true;
}

void CInputActivatorCrashFix::Unload() {
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CInputActivatorCrashFix::AcceptInputPre);
	hook::UnhookDetour(m_hook, m_address);
	m_hook = {};
}

} // namespace

std::unique_ptr<CFix> MakeInputActivatorCrashFix(Environment& env) {
	return std::make_unique<CInputActivatorCrashFix>(env);
}

} // namespace game_fixes
