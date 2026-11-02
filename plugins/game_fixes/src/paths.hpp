// game_fixes — where the plugin's files live (config resolution and file probing).
//
// The Pisex plugin read `addons/configs/game_fixes/game_fixes.ini` through the Metamod filesystem;
// the port reads it from the Plugify config directory (docs/PLUGIN_STANDARD.md §3:
// `<GetConfigsDir()>/<plugin>/<file>`). pisexcompat's ResolvePath used to hide that lookup; it is
// now these few functions, so the candidate order is visible and testable:
//
//   game_fixes.ini                                   as given (server working directory / absolute)
//   <configs>/game_fixes/game_fixes.ini              the packaged location (§3)
//   <configs>/game_fixes/translations/game_fixes.ini  the shim's translations fallback
//   <data>/game_fixes/game_fixes.ini                 the shim's data fallback
//
// The first existing candidate wins; when none exists the packaged location is returned, so the
// caller can log a path that tells the operator where the file belongs. `FileExists` is also what
// the gamedata loader uses to pick the plugin's gamedata.jsonc out of the package layout.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace game_fixes {

// Joins two path fragments with exactly one '/' (an empty left side returns the right one).
std::string JoinPath(std::string_view a, std::string_view b);

// True when the path can be opened for reading.
bool FileExists(std::string_view path);

// The plugin's config search order (pure: no filesystem access, no Plugify calls), so the order
// itself can be pinned by the tests. `configsDir` / `dataDir` are the host's GetConfigsDir() /
// GetDataDir() (empty when the host does not provide them).
std::vector<std::string> ConfigCandidates(std::string_view fileName, std::string_view configsDir,
										  std::string_view dataDir);

// First existing candidate, or the packaged `<configs>/game_fixes/<file>` when none exists (a
// useful path for the "could not be parsed" log line, exactly like the shim's fallback).
std::string ResolveConfigPath(std::string_view fileName, std::string_view configsDir, std::string_view dataDir);

} // namespace game_fixes
