// game_fixes signature check — a test-time verifier for the plugin's gamedata.
//
// It scans the *current* game binaries (the test server's copy, mounted at
// ../../cs2test/upper/game/... next to the repository root) with every pattern in
// assets/gamedata.jsonc and fails when a pattern does not match exactly once. It also
// resolves file offsets to virtual addresses the way the loader does (through the ELF
// PT_LOAD segments), so the offsets can be compared with logs from the live server.
//
// It additionally reports xrefs for a few strings that identify the steam_ban pass
// (see the SIGSCAN report) — that is the only fix whose signature is not shared with
// another live plugin, so its patterns are re-derived against the shipped build.
//
// The detailed report goes to plugins/game_fixes/SIGSCAN.md next to this test; the
// console output stays short. When the game binaries are not present the test passes
// with a SKIP (a checkout without the game files must not fail the suite).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr const char* kServerSo = "/src/cs2test/upper/game/csgo/bin/linuxsteamrt64/libserver.so";
constexpr const char* kEngineSo = "/src/cs2test/upper/game/bin/linuxsteamrt64/libengine2.so";
constexpr const char* kGamedata = "/src/plugins/game_fixes/assets/gamedata.jsonc";
constexpr const char* kReport = "/src/plugins/game_fixes/SIGSCAN.md";

// ---------------------------------------------------------------- binary image --

struct Segment {
	std::size_t offset = 0;   // file offset
	std::uintptr_t vaddr = 0; // virtual address
	std::size_t filesz = 0;
	bool execute = false;
};

struct Binary {
	std::vector<std::uint8_t> data;
	std::vector<Segment> segments;

	// file offset -> VA, or nullopt when the offset is not inside a PT_LOAD segment.
	std::optional<std::uintptr_t> VaOf(std::size_t off) const {
		for (const Segment& s : segments) {
			if (off >= s.offset && off < s.offset + s.filesz) {
				return s.vaddr + (off - s.offset);
			}
		}
		return std::nullopt;
	}

	bool Load(const char* path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return false;
		}
		data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		if (data.size() < 64 || std::memcmp(data.data(), "\x7f""ELF", 4) != 0) {
			data.clear();
			return false;
		}
		auto rd16 = [&](std::size_t at) { return *reinterpret_cast<const std::uint16_t*>(data.data() + at); };
		auto rd32 = [&](std::size_t at) { return *reinterpret_cast<const std::uint32_t*>(data.data() + at); };
		auto rd64 = [&](std::size_t at) { return *reinterpret_cast<const std::uint64_t*>(data.data() + at); };
		const std::size_t phoff = rd64(0x20);
		const std::size_t phentsize = rd16(0x36);
		const std::size_t phnum = rd16(0x38);
		for (std::size_t i = 0; i < phnum; ++i) {
			const std::size_t at = phoff + i * phentsize;
			if (rd32(at) != 1) { // PT_LOAD
				continue;
			}
			Segment s;
			s.offset = rd64(at + 0x08);
			s.vaddr = rd64(at + 0x10);
			s.filesz = rd64(at + 0x20);
			s.execute = (rd32(at + 0x04) & 1u) != 0;
			segments.push_back(s);
		}
		return true;
	}
};

// ------------------------------------------------------------------- patterns ----

// "48 8D 35 ? ? ? ?" -> bytes with 256 meaning "any".
std::vector<int> ParsePattern(std::string_view pattern) {
	std::vector<int> bytes;
	std::size_t i = 0;
	while (i < pattern.size()) {
		while (i < pattern.size() && (pattern[i] == ' ' || pattern[i] == '\t')) {
			++i;
		}
		if (i >= pattern.size()) {
			break;
		}
		if (pattern[i] == '?') {
			bytes.push_back(-1);
			while (i < pattern.size() && pattern[i] == '?') {
				++i;
			}
			continue;
		}
		auto hex = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -2;
		};
		const int hi = hex(pattern[i]);
		const int lo = i + 1 < pattern.size() ? hex(pattern[i + 1]) : -2;
		if (hi < 0 || lo < 0) {
			bytes.push_back(-2); // invalid marker: never matches
			++i;
			continue;
		}
		bytes.push_back(hi * 16 + lo);
		i += 2;
	}
	return bytes;
}

std::vector<std::size_t> FindAll(const Binary& bin, const std::vector<int>& pattern) {
	std::vector<std::size_t> hits;
	if (pattern.empty() || pattern.size() > bin.data.size()) {
		return hits;
	}
	// Anchor on the first fixed byte to keep the scan linear.
	std::size_t first = 0;
	while (first < pattern.size() && pattern[first] < 0) {
		++first;
	}
	if (first == pattern.size()) {
		return hits;
	}
	const std::uint8_t needle = static_cast<std::uint8_t>(pattern[first]);
	const std::size_t last = bin.data.size() - pattern.size();
	for (std::size_t i = 0; i <= last; ++i) {
		if (bin.data[i + first] != needle) {
			continue;
		}
		bool ok = true;
		for (std::size_t j = 0; j < pattern.size() && ok; ++j) {
			ok = pattern[j] < 0 || bin.data[i + j] == static_cast<std::uint8_t>(pattern[j]);
		}
		if (ok) {
			hits.push_back(i);
		}
	}
	return hits;
}

// -------------------------------------------------------------------- xrefs ------

// Finds code that loads the address of `va` through a rip-relative addressing form.
// Covers the shapes the compiler emits for string/global references:
//   [REX] opcode modrm(mod=00, rm=101) disp32
// which is lea/mov/call/jmp with the target in the next instruction.
std::vector<std::uintptr_t> FindXrefs(const Binary& bin, std::uintptr_t target) {
	std::vector<std::uintptr_t> hits;
	static const std::uint8_t kOpcodes[] = {0x8D, 0x8B, 0x89, 0x8A, 0x03, 0x0B, 0x2B, 0x33,
											0x39, 0xC7, 0xFF, 0x8F, 0x0F, 0xB6, 0xB7, 0xBE};
	for (const Segment& s : bin.segments) {
		if (!s.execute) {
			continue;
		}
		const std::size_t begin = s.offset;
		const std::size_t end = s.offset + s.filesz;
		for (std::size_t i = begin; i + 7 <= end; ++i) {
			std::size_t at = i;
			if (bin.data[at] >= 0x40 && bin.data[at] <= 0x4F) {
				++at; // REX prefix
			}
			if (std::find(std::begin(kOpcodes), std::end(kOpcodes), bin.data[at]) == std::end(kOpcodes)) {
				continue;
			}
			const std::uint8_t modrm = bin.data[at + 1];
			if ((modrm & 0xC7) != 0x05) { // mod=00, rm=101: rip-relative
				continue;
			}
			const std::int32_t disp = *reinterpret_cast<const std::int32_t*>(bin.data.data() + at + 2);
			const std::uintptr_t instrEnd = s.vaddr + (at + 6 - s.offset);
			if (instrEnd + static_cast<std::int64_t>(disp) == target) {
				hits.push_back(s.vaddr + (i - s.offset));
			}
		}
	}
	std::sort(hits.begin(), hits.end());
	hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
	return hits;
}

// ----------------------------------------------------------------------- misc ----

std::string StripComments(std::string text) {
	std::string out;
	bool inString = false;
	bool escape = false;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		if (inString) {
			out.push_back(c);
			if (escape) {
				escape = false;
			} else if (c == '\\') {
				escape = true;
			} else if (c == '"') {
				inString = false;
			}
			continue;
		}
		if (c == '"') {
			inString = true;
			out.push_back(c);
			continue;
		}
		if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
			while (i < text.size() && text[i] != '\n') {
				++i;
			}
			out.push_back('\n');
			continue;
		}
		out.push_back(c);
	}
	return out;
}

std::string Hex(std::uintptr_t value) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%lX", static_cast<unsigned long>(value));
	return buf;
}

struct Result {
	std::string name;
	std::string library;
	std::size_t matches = 0;
	std::size_t firstOffset = 0;
	std::optional<std::uintptr_t> va;
};

int Run() {
	Binary server;
	Binary engine;
	const bool haveServer = server.Load(kServerSo);
	const bool haveEngine = engine.Load(kEngineSo);
	std::ofstream report(kReport);
	const bool haveReport = report.is_open();

	if (!haveServer && !haveEngine) {
		std::printf("game_fixes signature check: SKIP (no game binaries at %s)\n", kServerSo);
		return 0;
	}

	std::ifstream file(kGamedata);
	if (!file) {
		std::printf("game_fixes signature check: FAIL (gamedata not found at %s)\n", kGamedata);
		return 1;
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	const nlohmann::json gamedata = nlohmann::json::parse(StripComments(buffer.str()));

	report << "# Signature scan report\n\n";
	report << "Generated by `plugins/game_fixes/tests/signature_check_tests.cpp` against:\n\n";
	report << "    " << kServerSo << (haveServer ? " (loaded)\n" : " (missing)\n");
	report << "    " << kEngineSo << (haveEngine ? " (loaded)\n" : " (missing)\n");
	report << "\n`va` is the address the loader would use, derived from the file offset through the\n"
			  "ELF PT_LOAD segments; on the live server add the module base shown in `maps`.\n\n";
	report << "## Signatures\n\n| name | library | matches | file offset | va |\n|---|---|---:|---|---|\n";

	bool ok = true;
	const auto& csgo = gamedata.at("csgo");
	for (const auto& [name, entry] : csgo.at("Signatures").items()) {
		const std::string library = entry.value("library", std::string("server"));
		const std::string pattern = entry.value("linuxsteamrt64", std::string());
		if (pattern.empty()) {
			report << "| " << name << " | " << library << " | no linuxsteamrt64 pattern | |\n";
			ok = false;
			continue;
		}
		const Binary& bin = library == "server" ? server : engine;
		if (bin.data.empty()) {
			report << "| " << name << " | " << library << " | library not loaded | |\n";
			continue;
		}
		const auto hits = FindAll(bin, ParsePattern(pattern));
		report << "| " << name << " | " << library << " | " << hits.size() << " | ";
		if (hits.empty()) {
			report << "| |\n";
			ok = false;
		} else {
			const auto va = bin.VaOf(hits.front());
			report << Hex(hits.front()) << " | " << (va ? Hex(*va) : "-") << " |\n";
			if (hits.size() != 1) {
				ok = false; // not unique
			}
		}
	}

	// xrefs for the steam_ban derivation
	report << "\n## xrefs (signature derivation aids)\n\n";
	for (const char* needle : {"Kicking user %s (sv_kick_players_with_cooldown=%d)",
							   "sv_kick_players_with_cooldown",
							   "Notification about user penalty: %u/%u (%u sec)"}) {
		const std::size_t pos = haveServer ? [&] {
			const std::string hay(reinterpret_cast<const char*>(server.data.data()), server.data.size());
			return hay.find(needle);
		}()
										  : std::string::npos;
		report << "* `" << needle << "`: ";
		if (pos == std::string::npos) {
			report << "not found\n";
			continue;
		}
		const auto va = server.VaOf(pos);
		report << "file offset " << Hex(pos) << " va " << (va ? Hex(*va) : "-") << "; xrefs:";
		if (va) {
			const auto xrefs = FindXrefs(server, *va);
			if (xrefs.empty()) {
				report << " none";
			}
			for (std::uintptr_t x : xrefs) {
				report << " " << Hex(x);
			}
			// A short dump around every xref, so a pattern can be derived from the report
			// without another tool run: 48 bytes before and 64 after, as file offsets.
			for (std::uintptr_t x : xrefs) {
				std::optional<std::size_t> fileOff;
				for (const Segment& s : server.segments) {
					if (x >= s.vaddr && x < s.vaddr + s.filesz) {
						fileOff = s.offset + (x - s.vaddr);
					}
				}
				if (!fileOff) {
					continue;
				}
				report << "\n  ```\n  xref " << Hex(x) << " (file " << Hex(*fileOff) << ")\n";
				const std::size_t begin = *fileOff > 48 ? *fileOff - 48 : 0;
				const std::size_t end = std::min(server.data.size(), *fileOff + 64);
				for (std::size_t i = begin; i < end; ++i) {
					if (i % 16 == 0) {
						char buf[32];
						std::snprintf(buf, sizeof(buf), "  %08lX:", static_cast<unsigned long>(i));
						report << (i == begin ? "" : "\n") << buf;
					}
					char buf[8];
					std::snprintf(buf, sizeof(buf), " %02X", server.data[i]);
					report << buf;
				}
				report << "\n  ```\n";
			}
		}
		report << "\n";
	}

	report.close();
	if (haveReport) {
		std::printf("game_fixes signature check: report written to %s\n", kReport);
	}
	if (!ok) {
		std::printf("game_fixes signature check: FAIL (see %s)\n", kReport);
		return 1;
	}
	std::printf("game_fixes signature check: OK\n");
	return 0;
}

} // namespace

int main() {
	return Run();
}
