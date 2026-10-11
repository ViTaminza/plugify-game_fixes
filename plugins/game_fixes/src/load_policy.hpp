// When a fix may be loaded.
//
// Plugin start is too early for anything that touches the schema system: s2sdk's schema cache
// walks the entity network serializer info, which does not exist before the first server start
// (calling into it then segfaults inside CollectSchemaFields -> CUtlRBTree::Find on a null
// class-info node), and it is torn down again on shutdown/changelevel. A fix that needs schema
// offsets is therefore *not* loaded at plugin start - it is put aside and loaded once the offsets
// are resolved, which only happens while a map is running.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the decision table.

#pragma once

namespace game_fixes {

enum class LoadDecision {
	Load,		   // load now
	DeferSchema,   // needs the schema and the offsets are not resolved yet: retry when they are
	Skip		   // disabled in the config (or its block is missing)
};

// `enabled` is the block's "enable" key, `needsSchema` the fix's own CFix::NeedsSchema(),
// `schemaResolved` whether every schema offset this plugin reads has been resolved already.
constexpr LoadDecision DecideLoad(bool enabled, bool needsSchema, bool schemaResolved) {
	if (!enabled) {
		return LoadDecision::Skip;
	}
	if (needsSchema && !schemaResolved) {
		return LoadDecision::DeferSchema;
	}
	return LoadDecision::Load;
}

} // namespace game_fixes
