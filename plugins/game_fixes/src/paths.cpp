#include "paths.hpp"

#include <fstream>

namespace game_fixes {

namespace {

// The plugin's directory name under the config/data roots (docs/PLUGIN_STANDARD.md §3).
constexpr const char* kPluginDir = "game_fixes";

} // namespace

std::string JoinPath(std::string_view a, std::string_view b) {
	if (a.empty()) {
		return std::string(b);
	}
	if (b.empty()) {
		return std::string(a);
	}
	std::string out(a);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (b.front() == '/') {
		b.remove_prefix(1);
	}
	out.append(b);
	return out;
}

bool FileExists(std::string_view path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(std::string(path), std::ios::binary);
	return file.good();
}

std::vector<std::string> ConfigCandidates(std::string_view fileName, std::string_view configsDir,
										  std::string_view dataDir) {
	std::vector<std::string> candidates;
	if (fileName.empty()) {
		return candidates;
	}
	const std::string configDir = JoinPath(configsDir, kPluginDir);
	const std::string dataPluginDir = JoinPath(dataDir, kPluginDir);

	candidates.emplace_back(fileName);								  // as given
	candidates.push_back(JoinPath(configDir, fileName));			  // <configs>/game_fixes/<file>
	candidates.push_back(JoinPath(configDir, JoinPath("translations", std::string(fileName))));
	candidates.push_back(JoinPath(dataPluginDir, fileName));		  // <data>/game_fixes/<file>
	return candidates;
}

std::string ResolveConfigPath(std::string_view fileName, std::string_view configsDir, std::string_view dataDir) {
	const std::vector<std::string> candidates = ConfigCandidates(fileName, configsDir, dataDir);
	for (const std::string& candidate : candidates) {
		if (!candidate.empty() && FileExists(candidate)) {
			return candidate;
		}
	}
	// Nothing on disk: hand back the packaged location so the caller logs where the file belongs.
	if (candidates.size() > 1) {
		return candidates[1];
	}
	return candidates.empty() ? std::string() : candidates.front();
}

} // namespace game_fixes
