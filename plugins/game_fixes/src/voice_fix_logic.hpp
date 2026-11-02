// voice_fix — the pure part: which incoming CCLCMsg_VoiceData messages must be dropped.
//
// The reference VoiceFix (CS2VoiceFix) stops every client voice message whose protobuf has no
// `audio` field: the game/engine expects one, and clients that send empty packets are what the
// fix is about. CCLCMsg_VoiceData::_has_bits_ bit 0 is the "audio present" bit.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the bit test.

#pragma once

#include <cstdint>

namespace game_fixes::voicefix {

// _has_bits_ bit 0 == CCLCMsg_VoiceData.audio is set.
constexpr std::uint32_t kHasAudioBit = 1u;

constexpr bool HasAudioField(std::uint32_t hasBits) {
	return (hasBits & kHasAudioBit) != 0;
}

// A voice message with no audio field: drop it (the reference returns HookResult.Stop).
constexpr bool ShouldDrop(std::uint32_t hasBits) {
	return !HasAudioField(hasBits);
}

// The svc_VoiceData id used by the outgoing half is a protocol constant; kept here so the
// message-id decision is testable too.
constexpr bool IsVoiceDataMessage(std::int32_t messageId, std::int32_t voiceDataId) {
	return messageId > 0 && messageId == voiceDataId;
}

} // namespace game_fixes::voicefix
