// game_fixes — a read-only Valve KeyValues (KV1) reader for game_fixes.ini.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): the hl2sdk KeyValues the port used to load
// through pisexcompat's shim is replaced by this reader, which lives in the plugin itself and
// depends on nothing but the standard library. The accepted grammar and the getter semantics are
// the ones the shim's reader had (so the production game_fixes.ini keeps loading byte for byte):
//
//   * quoted ("key" "value") and unquoted (key value) tokens, nested "key" { ... } blocks,
//   * a key may carry a value and a block, like Valve's KV1,
//   * // and /* */ comments, escapes \" \\ \n \r \t inside quoted strings,
//   * #base / #include / #define lines are parsed and dropped (there is no file inclusion),
//   * an unterminated string, a missing '}' or a stray top-level '}' makes the file invalid
//     (nullopt, so the plugin reports a config error instead of reading a half-parsed tree).
// Like Valve's loader (and the shim), a file that is a single "Root" { ... } block is returned as
// that block's contents, so `LoadKv1File("game_fixes.ini")->Find("beam_crash")` reads the fix block
// inside the file's "game_fixes" block. Key lookup is case-insensitive (that is how Valve's FindKey
// behaves; the original spelling is kept in KvNode::name). A leading UTF-8 BOM is skipped, so a
// file saved by a Windows editor still loads.
//
// Getters mirror the shim exactly, because the values in the live config are read by them:
//   * GetInt   — trimmed text, empty -> def, otherwise from_chars with Valve's atoi fallback,
//   * GetFloat — trimmed text, empty or not a number -> def, otherwise strtof,
//   * GetBool  — "true"/"yes"/"on" -> true, "false"/"no"/"off" -> false, otherwise GetInt != 0,
//   * GetString(key) — the child's value; an empty key reads the node's own value (the whitelist
//     entries of steam_ban are "Label" "SteamID64" pairs).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_fixes {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	KvNode* Find(std::string_view key);

	// The child's value; `def` when the child is missing or has no value (a block, an empty node).
	// An empty key reads this node's own value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The same value as an int, with Valve's atoi fallback ("5" -> 5, "1.5" -> 1, "abc" -> 0);
	// `def` when the child is missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
	// The same value as a float; `def` when the child is missing, empty or starts with no number.
	float GetFloat(std::string_view key, float def = 0.0f) const;
	// The same value as a bool with the shim's spelling rules (see the header comment).
	bool GetBool(std::string_view key, bool def = false) const;

	const std::vector<KvNode>& Children() const { return children; }
	bool HasValue() const { return hasValue; }
	bool HasChildren() const { return !children.empty(); }
};

// Compares two KV1 names the way Find() does (ASCII case-insensitively).
bool EqualNoCase(std::string_view a, std::string_view b);

// Parses a whole KV1 document; nullopt when the text is malformed (see the header comment).
std::optional<KvNode> ParseKv1(std::string_view text);
// Reads and parses a file; nullopt when the file cannot be opened or is malformed.
std::optional<KvNode> LoadKv1File(const std::string& path);

} // namespace game_fixes
