#pragma once

// Chat texts from translations/*.json, with no game types, so they can be tested on their own.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace messages
{

// {name} -> value. The value is inserted as plain text, with bytes below 0x20 removed.
// The one exception is `team`: give it a color token, such as "{yellow}", to color the next words.
using Values = std::vector<std::pair<std::string, std::string>>;

// Language code (the file name without .json) -> key -> text.
using Table = std::map<std::string, std::map<std::string, std::string>>;

class Messages
{
public:
	// Reads every *.json in the folder and replaces what was loaded before. Never throws: each
	// problem comes back as one line of text, and the files without problems still load.
	std::vector<std::string> Load(const std::string &folder);

	// The finished chat line: the text in that language (English when the language or the key is
	// missing), with prefixes, values and colors filled in. It starts with a space, because CS2
	// drops a color code at the very start of a chat line. An unknown key gives the key itself.
	std::string Format(std::string_view language, std::string_view key, const Values &values = {}) const;

	const Table &Texts() const { return texts_; }

private:
	const std::string *Find(std::string_view language, std::string_view key) const;
	void Expand(std::string &out, std::string_view text, std::string_view language, const Values &values, bool prefixes) const;

	Table texts_;
};

// The file code ("tr", "pt", ...) for CS2's cl_language value ("turkish", "brazilian", ...).
// Anything unknown is "en".
std::string_view LanguageFile(std::string_view cl_language);

} // namespace messages
