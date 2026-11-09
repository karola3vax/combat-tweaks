// g++ -std=c++20 -I src -I third_party src/messages.cpp tests/messages_test.cpp -o messages_test && ./messages_test
// Run from the project folder: it reads the real translations folder.

#include "messages.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace messages;

namespace
{

// Every {token} in a text, sorted, so a translation may move them but not lose or add one.
std::vector<std::string> Tokens(const std::string &text)
{
	std::vector<std::string> tokens;
	for (std::size_t open = text.find('{'); open != std::string::npos; open = text.find('{', open + 1))
	{
		const std::size_t close = text.find('}', open);
		assert(close != std::string::npos);
		tokens.push_back(text.substr(open, close - open + 1));
	}
	std::sort(tokens.begin(), tokens.end());
	return tokens;
}

} // namespace

int main()
{
	Messages texts;
	const std::vector<std::string> errors = texts.Load("translations");
	for (const std::string &error : errors) std::printf("%s\n", error.c_str());
	assert(errors.empty());

	// Every language has exactly English's keys, with the same placeholders and colors.
	const auto &all = texts.Texts();
	const auto &en = all.at("en");
	for (const char *code : {"en", "tr", "ru", "pt", "es", "de", "zh", "pl", "fr"}) assert(all.contains(code));
	for (const auto &[code, language] : all)
	{
		assert(language.size() == en.size());
		for (const auto &[key, text] : en)
		{
			assert(language.contains(key));
			if (Tokens(language.at(key)) != Tokens(text)) std::printf("%s.json \"%s\" has other tokens\n", code.c_str(), key.c_str());
			assert(Tokens(language.at(key)) == Tokens(text));
		}
	}

	// Prefixes, colors and values; a leading space; outside text cannot carry color codes.
	assert(texts.Format("en", "rush_attempt", {{"team", "{yellow}"}, {"name", "Bad\x07Guy {red}\n"}})
		== " \x07[ANTI-RUSH]\x01 \x09" "BadGuy {red}\x01 tried to rush into a blocked zone.");
	assert(texts.Format("en", "barrier_unlocked") == " \x07[ANTI-RUSH]\x01 The barrier is open, rushing is allowed.");
	// A team value that is not a color adds nothing.
	assert(texts.Format("en", "rush_attempt", {{"team", "\x07"}, {"name", "A"}}).find(" A\x01 tried") != std::string::npos);
	// Unknown placeholders stay visible.
	assert(texts.Format("en", "zone_saved") == " \x06[Combat Tweaks]\x01 Zone {number} is saved for this map.");

	// Fallbacks: an unknown language reads English, an unknown key gives the key.
	assert(texts.Format("xx", "barrier_unlocked") == texts.Format("en", "barrier_unlocked"));
	assert(texts.Format("tr", "barrier_unlocked") != texts.Format("en", "barrier_unlocked"));
	assert(texts.Format("tr", "no_such_key") == " no_such_key");

	// A key only English has is read from English.
	const std::filesystem::path folder = std::filesystem::temp_directory_path() / "combat_tweaks_messages_test";
	std::filesystem::create_directories(folder);
	std::ofstream(folder / "en.json") << "{ \"a\": \"{grey}A {lightblue}{x}\", /* comment */ \"b\": \"B\" }";
	std::ofstream(folder / "tr.json") << "{ \"a\": \"Ä\" }";
	std::ofstream(folder / "bad.json") << "{ \"a\": ";
	std::ofstream(folder / "num.json") << "{ \"a\": 5 }";
	Messages small;
	assert(small.Load(folder.string()).size() == 2); // bad.json and num.json's "a"
	assert(small.Format("tr", "a") == " Ä");
	assert(small.Format("tr", "b") == " B");
	assert(small.Format("en", "a", {{"x", "1"}}) == " \x08" "A \x0B" "1");
	std::filesystem::remove_all(folder);
	assert(Messages().Load("no_such_folder").size() == 2); // the folder, and no en.json

	assert(LanguageFile("english") == "en" && LanguageFile("turkish") == "tr" && LanguageFile("russian") == "ru");
	assert(LanguageFile("brazilian") == "pt" && LanguageFile("portuguese") == "pt");
	assert(LanguageFile("spanish") == "es" && LanguageFile("latam") == "es" && LanguageFile("german") == "de");
	assert(LanguageFile("schinese") == "zh" && LanguageFile("tchinese") == "zh");
	assert(LanguageFile("polish") == "pl" && LanguageFile("french") == "fr");
	assert(LanguageFile("koreana") == "en" && LanguageFile("") == "en");

	std::printf("messages_test passed\n");
	return 0;
}
