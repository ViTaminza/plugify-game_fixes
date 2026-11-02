// cstv_bot_quota — the pure part: when the candidate predicate may reject an HLTV controller.
//
// TeamCandidate is a shared predicate: the bot-quota selector calls it, and it must only reject
// HLTV while the selector itself is on the stack (thread-local depth > 0). Everywhere else the
// original predicate must run unchanged.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the depth rule.

#pragma once

namespace game_fixes::cstv {

constexpr bool ShouldRejectCandidate(int selectionDepth, bool controllerPresent, bool isHltv) {
	return selectionDepth > 0 && controllerPresent && isHltv;
}

constexpr int EnterSelection(int depth) {
	return depth + 1;
}

// Never goes below zero: a stray Post (a failed Pre registration) must not arm the predicate.
constexpr int LeaveSelection(int depth) {
	return depth > 0 ? depth - 1 : 0;
}

} // namespace game_fixes::cstv
