#include "kv1.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace game_fixes {

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
		const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
		if (left != right) {
			return false;
		}
	}
	return true;
}

namespace {

std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The tokenizer and grammar of the KeyValues reader the port used through pisexcompat, building a
// KvNode tree instead:
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: there is no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, true);
				parent.children[index] = std::move(child);
			}
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace

const KvNode* KvNode::Find(std::string_view key) const {
	return const_cast<KvNode*>(this)->Find(key);
}

KvNode* KvNode::Find(std::string_view key) {
	if (key.empty()) {
		return nullptr;
	}
	for (KvNode& child : children) {
		if (EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	if (key.empty()) {
		return hasValue ? std::string_view(value) : def;
	}
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = Trim(GetString(key));
	if (text.empty()) {
		return def;
	}
	int number = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
	if (result.ec != std::errc{}) {
		return std::atoi(std::string(text).c_str()); // Valve's atoi fallback
	}
	return number;
}

float KvNode::GetFloat(std::string_view key, float def) const {
	const std::string_view text = Trim(GetString(key));
	if (text.empty()) {
		return def;
	}
	// strtof needs a NUL-terminated buffer; a string_view does not guarantee one.
	const std::string copy(text);
	char* end = nullptr;
	const float number = std::strtof(copy.c_str(), &end);
	if (end == copy.c_str()) {
		return def;
	}
	return number;
}

bool KvNode::GetBool(std::string_view key, bool def) const {
	const std::string_view text = Trim(GetString(key));
	if (text.empty()) {
		return def;
	}
	if (EqualNoCase(text, "true") || EqualNoCase(text, "yes") || EqualNoCase(text, "on")) {
		return true;
	}
	if (EqualNoCase(text, "false") || EqualNoCase(text, "no") || EqualNoCase(text, "off")) {
		return false;
	}
	return GetInt(key, def ? 1 : 0) != 0;
}

std::optional<KvNode> ParseKv1(std::string_view text) {
	// A file saved by a Windows editor starts with a UTF-8 BOM: it is not part of the first key.
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	KvNode root;
	Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// A single top-level block is the file's root ("game_fixes" { ... }): Valve's loader merges the
	// block into the object it parses into, which is what the shim's LoadFromFile did as well.
	if (root.children.size() == 1 && !root.children.front().children.empty()) {
		KvNode top = std::move(root.children.front());
		return top;
	}
	return root;
}

std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return ParseKv1(content);
}

} // namespace game_fixes
