// game_fixes package regression — the defect this test exists for.
//
// package.sh used to prefer the fork work tree's config
// (ref/src/game_fixes-slowmo/configs/addons/game_fixes/game_fixes.ini, falling back to
// ref/src/GameFixes/...). That file carries only the original 14 fix blocks: it has no
// voice_fix / blank_map / player_ghost / fake_messages entries, although the plugin's checked-in
// assets/game_fixes.ini enables all four. A fresh standalone install therefore silently shipped a
// package in which those four fixes never loaded (the plugin logs "disabled" for a missing block).
//
// The test runs the real package.sh in a throwaway BUILD_DIR/OUT_DIR with fake build artifacts and
// pins the contract:
//   * the packaged config is byte-identical to plugins/game_fixes/assets/game_fixes.ini - the
//     single source of truth, independent of ref/;
//   * that config, parsed with the plugin's own KV1 reader, has every fix block (the full
//     20-fix list), the shipped enable flags (all on except cstv_bot_quota/jump_spam, and the four
//     regression blocks in particular on), and the shipped option values;
//   * the package layout is complete (manifest, .so, gamedata in all three shipped locations);
//   * package.sh's source no longer references the fork tree and does point at the shipped file;
//   * an existing server config is never modified, and an OUT_DIR outside BUILD_DIR is refused
//     *before* anything is deleted - the script only writes its build output, so a deploy that
//     copies configs with `cp -n` can never lose the operator's settings;
//   * missing build artifacts fail with a non-zero exit and leave no half-built package.
//
// No engine, no s2sdk/polyhook bindings, no network. Runs from build/tests via ./test.sh.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"
#include "kv1.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s\n", what.c_str());
	}
}

std::string Quote(const std::string& text) {
	std::string quoted = "'";
	for (const char c : text) {
		if (c == '\'') {
			quoted += "'\\''";
		} else {
			quoted += c;
		}
	}
	quoted += "'";
	return quoted;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << content;
}

// Runs package.sh with an explicit build root and output directory. `envPrefix` is prepended to
// the command (the known-server-root test extends the denial list with it); a non-empty `pathDir`
// replaces PATH, which is how the python3 fallback of absolute() is exercised - that PATH has no
// realpath, so the script must take the os.path.realpath branch.
int RunPackage(const std::filesystem::path& buildDir, const std::filesystem::path& outDir,
			   const std::filesystem::path& logFile, const std::string& envPrefix = {},
			   const std::filesystem::path& pathDir = {}) {
	std::string command = envPrefix;
	if (!pathDir.empty()) {
		command += "PATH=" + Quote(pathDir.string()) + " ";
	}
	command += "BUILD_DIR=" + Quote(buildDir.string()) + " OUT_DIR=" + Quote(outDir.string()) + " /bin/bash " +
			   Quote(std::string(GAME_FIXES_TEST_SOURCE_DIR) + "/package.sh") + " >" + Quote(logFile.string()) +
			   " 2>&1";
	const int status = std::system(command.c_str());
	if (status == -1) {
		return -1;
	}
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return 128;
}

// A PATH directory holding the external commands package.sh needs but deliberately WITHOUT
// `realpath`, so the script takes its python3 os.path.realpath fallback. Returns false (with the
// reason) when one of the commands is not installed - the caller reports that as a test failure.
bool MakePathWithoutRealpath(const std::filesystem::path& dir, std::string* error) {
	std::error_code filesystemError;
	std::filesystem::remove_all(dir, filesystemError);
	std::filesystem::create_directories(dir, filesystemError);
	for (const char* name : {"python3", "cp", "rm", "mkdir", "find", "sort", "dirname"}) {
		std::filesystem::path found;
		for (const char* base : {"/usr/bin", "/bin", "/usr/local/bin"}) {
			const std::filesystem::path candidate = std::filesystem::path(base) / name;
			if (std::filesystem::is_regular_file(candidate)) {
				found = candidate;
				break;
			}
		}
		if (found.empty()) {
			*error = std::string("command not found: ") + name;
			return false;
		}
		std::filesystem::create_symlink(found, dir / name, filesystemError);
		if (filesystemError) {
			*error = std::string("cannot symlink ") + name + ": " + filesystemError.message();
			return false;
		}
	}
	return true;
}

void PrintLogOnFailure(int exitCode, const std::filesystem::path& logFile, std::string_view what) {
	if (exitCode == 0) {
		return;
	}
	std::printf("package.sh failed for %s (exit %d):\n", std::string(what).c_str(), exitCode);
	if (const auto log = ReadFile(logFile)) {
		std::printf("%s\n", log->c_str());
	}
}

// The config checks: the full fix set, the shipped enable flags and the shipped option values.
void TestPackagedConfig(const std::filesystem::path& packagedConfig, const std::filesystem::path& assetsConfig) {
	using namespace game_fixes;

	const auto packaged = ReadFile(packagedConfig);
	Check(packaged.has_value(), "the package ships configs/game_fixes/game_fixes.ini");
	const auto assets = ReadFile(assetsConfig);
	Check(assets.has_value(), "the plugin ships assets/game_fixes.ini");
	if (!packaged || !assets) {
		return;
	}
	Check(*packaged == *assets,
		  "the packaged config is byte-identical to plugins/game_fixes/assets/game_fixes.ini "
		  "(and therefore does not come from a fork work tree)");

	const auto tree = ParseKv1(*packaged);
	Check(tree.has_value(), "the packaged config parses with the plugin's own KV1 reader");
	if (!tree) {
		return;
	}
	Check(tree->Children().size() == config::kFixNames.size() + 1,
		  "the packaged config has one block per fix plus server_list_players");

	for (const char* name : config::kFixNames) {
		const KvNode* block = tree->Find(name);
		Check(block != nullptr, std::string("the packaged config has the block for ") + name);
		if (block == nullptr) {
			continue;
		}
		const bool expectedOn = std::string_view(name) != "cstv_bot_quota" && std::string_view(name) != "jump_spam";
		Check(block->GetBool("enable") == expectedOn,
			  std::string("the packaged ") + name + (expectedOn ? " is enabled by default" : " is off by default"));
	}

	// The four blocks the fork config lacked (all enabled in the shipped file). This is the exact
	// regression: a package built from ref/src/... would fail these four checks.
	for (const char* name : {"voice_fix", "blank_map", "player_ghost", "fake_messages"}) {
		const KvNode* block = tree->Find(name);
		Check(block != nullptr && block->GetBool("enable"),
			  std::string("the packaged config enables ") + name + " (missing from the fork work tree)");
	}
	Check(config::ReadFixBlock(*tree, config::kServerListPlayersBlock).enabled,
		  "the packaged config enables server_list_players (logged as not ported)");

	// The shipped option values, i.e. the fork's own keys are all present (the fork's slow_animation
	// work tree added max_map_hours/match_wait_minutes/warn_minutes; the assets file carries them).
	const config::FixBlock slow = config::ReadFixBlock(*tree, "slow_animation");
	Check(slow.options != nullptr, "the packaged config has the slow_animation block");
	if (slow.options != nullptr) {
		const config::SlowAnimationOptions options = config::ReadSlowAnimation(*slow.options);
		Check(options.reloadInterval == 1800.0f, "slow_animation reload_interval is 1800 s");
		Check(options.watch.maxMapHours == 3.0f, "slow_animation max_map_hours is 3 (fork option kept)");
		Check(options.watch.matchWaitMinutes == 120.0f, "slow_animation match_wait_minutes is 120 (fork option kept)");
		Check(options.watch.warnMinutes == 2.0f, "slow_animation warn_minutes is 2 (fork option kept)");
	}
	const config::FixBlock keyvalues = config::ReadFixBlock(*tree, "keyvalues_flood");
	Check(keyvalues.options != nullptr, "the packaged config has the keyvalues_flood block");
	if (keyvalues.options != nullptr) {
		const config::KeyValuesFloodOptions options = config::ReadKeyValuesFlood(*keyvalues.options);
		Check(options.maxBytes == 1500 && options.maxPerSecond == 10 && options.burst == 20 && !options.kick,
			  "keyvalues_flood keeps the shipped 1500/10/20/kick 0");
	}
	const config::FixBlock voice = config::ReadFixBlock(*tree, "voice_flood");
	Check(voice.options != nullptr, "the packaged config has the voice_flood block");
	if (voice.options != nullptr) {
		const config::VoiceFloodOptions options = config::ReadVoiceFlood(*voice.options);
		Check(options.maxPackets == 64 && options.maxBytes == 4096 && options.maxPerSecond == 128 &&
				  options.cooldown == 5.0f && options.kick && options.kickStrikes == 3,
			  "voice_flood keeps the shipped 64/4096/128/5/kick 1/3");
	}
	const config::FixBlock teamSwitch = config::ReadFixBlock(*tree, "team_switch_spam");
	Check(teamSwitch.options != nullptr, "the packaged config has the team_switch_spam block");
	if (teamSwitch.options != nullptr) {
		const config::TeamSwitchSpamOptions options = config::ReadTeamSwitchSpam(*teamSwitch.options);
		Check(options.maxAttempts == 15 && options.windowSeconds == 10.0,
			  "team_switch_spam keeps the shipped 15 attempts / 10 s");
	}
	const config::FixBlock steamBan = config::ReadFixBlock(*tree, "steam_ban");
	Check(steamBan.options != nullptr, "the packaged config has the steam_ban block");
	if (steamBan.options != nullptr) {
		const config::SteamBanOptions options = config::ReadSteamBan(*steamBan.options);
		Check(options.clearAfterPass, "steam_ban ships clear_after_pass 1");
		Check(options.whitelist.empty(), "the shipped steam_ban whitelist is empty");
	}
}

void TestPackageScriptSource() {
	const std::filesystem::path source = std::string(GAME_FIXES_TEST_SOURCE_DIR) + "/package.sh";
	const auto text = ReadFile(source);
	Check(text.has_value(), "plugins/game_fixes/package.sh is readable");
	if (!text) {
		return;
	}
	Check(text->find("ref/src") == std::string::npos,
		  "package.sh does not prefer a fork work tree under ref/src (the 14-block config)");
	Check(text->find("FORK_CONFIG") == std::string::npos, "package.sh has no fork-config fallback left");
	Check(text->find("assets/game_fixes.ini") != std::string::npos,
		  "package.sh ships plugins/game_fixes/assets/game_fixes.ini");
	Check(text->find("cp -n") != std::string::npos || text->find("cp -rn") != std::string::npos,
		  "package.sh documents the no-clobber deploy rule for an existing server config");
	// The python fallback used to be os.path.abspath, which does not resolve symlinks: a link inside
	// the build tree could point at a server directory and pass the "inside BUILD_DIR" check.
	Check(text->find("os.path.realpath") != std::string::npos,
		  "the python fallback of absolute() uses os.path.realpath (symlinks resolved)");
	Check(text->find("os.path.abspath") == std::string::npos,
		  "package.sh does not use the symlink-blind os.path.abspath");
	Check(text->find("GAME_FIXES_SERVER_ROOTS") != std::string::npos,
		  "package.sh has the known-server-root denial list the tests extend");
	Check(text->find("BUILD_DIR must not be the filesystem root") != std::string::npos &&
			  text->find("OUT_DIR must not be the filesystem root") != std::string::npos,
		  "package.sh explicitly refuses / as BUILD_DIR and as OUT_DIR");
}

void TestPackageLayout(const std::filesystem::path& outDir) {
	for (const char* relative : {"game_fixes.pplugin", "bin/libgame_fixes.so", "bin/gamedata.jsonc", "gamedata.jsonc",
								 "assets/gamedata.jsonc", "configs/game_fixes/game_fixes.ini"}) {
		Check(std::filesystem::is_regular_file(outDir / relative),
			  std::string("the package contains ") + relative);
	}
	const auto rootGamedata = ReadFile(outDir / "gamedata.jsonc");
	const auto assetsGamedata = ReadFile(outDir / "assets/gamedata.jsonc");
	const auto binGamedata = ReadFile(outDir / "bin/gamedata.jsonc");
	Check(rootGamedata.has_value() && assetsGamedata.has_value() && binGamedata.has_value(),
		  "the package ships gamedata in the package root, assets/ and bin/");
	if (rootGamedata && assetsGamedata && binGamedata) {
		Check(*rootGamedata == *assetsGamedata && *assetsGamedata == *binGamedata,
			  "all three gamedata copies are the plugin's own file");
	}
}

} // namespace

int main() {
	std::printf("game_fixes package tests\n");

	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("game_fixes_package_tests_" + std::to_string(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(root, error);

	const std::filesystem::path buildDir = root / "build";
	const std::filesystem::path outDir = buildDir / "package" / "game_fixes";
	const std::filesystem::path logFile = root / "package.log";
	const std::filesystem::path assetsConfig =
		std::string(GAME_FIXES_TEST_SOURCE_DIR) + "/assets/game_fixes.ini";

	// Fake build artifacts: every artifact package.sh consumes is created here, so the test never
	// depends on a real ./build.sh run.
	WriteFile(buildDir / "plugins" / "game_fixes" / "libgame_fixes.so", "fake shared object\n");
	WriteFile(buildDir / "plugins" / "game_fixes" / "game_fixes.pplugin", "{\"name\":\"game_fixes\"}\n");

	// A pre-existing server config, as a deployment target would hold it (hand-edited enable flags,
	// whitelist, limits). package.sh must never touch it.
	const std::filesystem::path serverConfig = root / "server-configs" / "game_fixes" / "game_fixes.ini";
	const std::string serverConfigContent =
		"\"game_fixes\"\n{\n\t\"steam_ban\"\n\t{\n\t\t\"enable\"\t\"0\"\n\t\t\"whitelist\"\n\t\t{\n\t\t\t\"Admin\"\t"
		"\"76561198000000000\"\n\t\t}\n\t}\n}\n";
	WriteFile(serverConfig, serverConfigContent);

	// 1. The normal run with explicit temp dirs and fake artifacts.
	int exitCode = RunPackage(buildDir, outDir, logFile);
	PrintLogOnFailure(exitCode, logFile, "the temp build/output run");
	Check(exitCode == 0, "package.sh succeeds with a temp BUILD_DIR/OUT_DIR and fake artifacts");
	TestPackageLayout(outDir);
	TestPackagedConfig(outDir / "configs" / "game_fixes" / "game_fixes.ini", assetsConfig);
	TestPackageScriptSource();
	Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
		  "package.sh does not touch an existing server config");

	// 2. Idempotent: a second run rebuilds the same package.
	exitCode = RunPackage(buildDir, outDir, logFile);
	PrintLogOnFailure(exitCode, logFile, "the second run");
	Check(exitCode == 0, "package.sh can be run twice");
	Check(ReadFile(outDir / "configs" / "game_fixes" / "game_fixes.ini").value_or("") ==
			  ReadFile(assetsConfig).value_or("<unreadable>"),
		  "the rebuilt package ships the same config");

	// 3. Safety: OUT_DIR pointing at a server-configs directory is refused before anything is
	// deleted (the script refuses any OUT_DIR outside BUILD_DIR; the expected failure is not a
	// package error, so its log is not printed).
	exitCode = RunPackage(buildDir, root / "server-configs" / "game_fixes", logFile);
	Check(exitCode != 0, "package.sh refuses an OUT_DIR outside BUILD_DIR");
	Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
		  "the server config survives the refused OUT_DIR (nothing was deleted)");
	Check(!std::filesystem::exists(root / "server-configs" / "game_fixes" / "configs") &&
			  !std::filesystem::exists(root / "server-configs" / "game_fixes" / "bin"),
		  "the refused run does not create package directories inside the server configs");

	// 4. Symlink escape: a link inside BUILD_DIR points at a server-configs directory. The guard must
	// resolve it and refuse, or the rebuilt output would delete the real server config. This is the
	// hole the old os.path.abspath fallback had (it kept the link's lexical path).
	const std::filesystem::path escapeLink = buildDir / "escape";
	std::error_code linkError;
	std::filesystem::remove(escapeLink, linkError);
	std::filesystem::create_directory_symlink(root / "server-configs", escapeLink, linkError);
	Check(!linkError, "the symlink escape fixture is created");
	if (!linkError) {
		const std::filesystem::path escapedOut = escapeLink / "game_fixes";
		exitCode = RunPackage(buildDir, escapedOut, logFile);
		Check(exitCode != 0, "package.sh refuses an OUT_DIR that leaves BUILD_DIR through a symlink");
		Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
			  "the config behind the symlink is not deleted");
		Check(!std::filesystem::exists(root / "server-configs" / "game_fixes" / "bin") &&
				  !std::filesystem::exists(root / "server-configs" / "game_fixes" / "game_fixes.pplugin"),
			  "nothing is written through the symlink");

		// 5. The same escape through the python3 fallback: run package.sh with a PATH that has no
		// realpath, so absolute() must use os.path.realpath. A fallback that used os.path.abspath
		// would accept the lexical path and delete the target behind the link.
		std::string pathError;
		const std::filesystem::path noRealpath = root / "bin-no-realpath";
		if (MakePathWithoutRealpath(noRealpath, &pathError)) {
			exitCode = RunPackage(buildDir, escapedOut, logFile, {}, noRealpath);
			Check(exitCode != 0,
				  "package.sh refuses the symlink escape with the python os.path.realpath fallback "
				  "(PATH without realpath)");
			Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
				  "the config behind the symlink survives the python-fallback run");
		} else {
			Check(false, "the python-fallback PATH fixture could not be built: " + pathError);
		}
	}

	// 6. `/` must be refused both as BUILD_DIR (it would make every OUT_DIR "inside" it) and as
	// OUT_DIR; the guard runs before anything is created or deleted.
	exitCode = RunPackage("/", root / "root-build-out", logFile);
	Check(exitCode != 0, "package.sh refuses BUILD_DIR=/");
	Check(!std::filesystem::exists(root / "root-build-out"), "the refused BUILD_DIR=/ run creates nothing");
	exitCode = RunPackage(buildDir, "/", logFile);
	Check(exitCode != 0, "package.sh refuses OUT_DIR=/");

	// 7. A known server tree is refused even when OUT_DIR is inside BUILD_DIR. The test extends the
	// denial list with a temp "server" tree (GAME_FIXES_SERVER_ROOTS), so no real server path is
	// touched; a sentinel config proves nothing was deleted or written.
	const std::filesystem::path fakeServer = root / "serverlike";
	const std::filesystem::path fakeServerConfig = fakeServer / "configs" / "game_fixes" / "game_fixes.ini";
	WriteFile(fakeServerConfig, serverConfigContent);
	// Fake artifacts inside the fake server tree: without the denial list the script would succeed
	// here (and rebuild - i.e. delete - the server config directory), so the refusal is what keeps
	// the sentinel alive.
	WriteFile(fakeServer / "plugins" / "game_fixes" / "libgame_fixes.so", "fake shared object\n");
	WriteFile(fakeServer / "plugins" / "game_fixes" / "game_fixes.pplugin", "{\"name\":\"game_fixes\"}\n");
	exitCode = RunPackage(fakeServer, fakeServer / "configs" / "game_fixes", logFile,
						  "GAME_FIXES_SERVER_ROOTS=" + Quote(fakeServer.string()) + " ");
	Check(exitCode != 0, "package.sh refuses an OUT_DIR inside a known server tree, even inside BUILD_DIR");
	Check(ReadFile(fakeServerConfig).value_or("") == serverConfigContent,
		  "the fake server config is not deleted");
	Check(!std::filesystem::exists(fakeServer / "configs" / "game_fixes" / "bin") &&
			  !std::filesystem::exists(fakeServer / "configs" / "game_fixes" / "game_fixes.pplugin"),
		  "nothing is written into the fake server tree");

	// 8. Missing build artifacts fail cleanly and leave no half-built package.
	const std::filesystem::path emptyBuild = root / "empty-build";
	std::filesystem::create_directories(emptyBuild);
	const std::filesystem::path emptyOut = emptyBuild / "package" / "game_fixes";
	exitCode = RunPackage(emptyBuild, emptyOut, logFile);
	Check(exitCode != 0, "package.sh fails when the build artifacts are missing");
	Check(!std::filesystem::exists(emptyOut), "a failed package run leaves no output directory");

	std::filesystem::remove_all(root, error);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
