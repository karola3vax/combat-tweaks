// g++ -std=c++20 -I src -I third_party src/config.cpp tests/config_test.cpp -o config_test && ./config_test

#include "config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

int main()
{
	std::vector<std::string> errors;

	// The shipped file parses with no complaints and matches the built-in defaults.
	std::ifstream file("configs/combat_tweaks.jsonc", std::ios::binary);
	const std::string shipped {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	assert(!shipped.empty());
	config::Settings settings = config::Parse(shipped, errors);
	for (const auto &e : errors) std::printf("shipped config: %s\n", e.c_str());
	assert(errors.empty());
	assert(settings.no_aimpunch && settings.anti_rush.unlock_seconds == 60 && settings.anti_rush.glow_color_ct[3] == 255);

	// Bad values are reported and fall back; good ones are taken.
	errors.clear();
	settings = config::Parse(R"({
		"no_aimpunch": "yes",
		"hide_ragdolls": false,
		"typo": 1,
		"admins": ["76561198353131845", "123", 76561198000000000],
		"anti_rush": { "unlock_seconds": 900, "callout_seconds": 1.5, "line_color": [10, 20, 30], "glow_color_t": [1, 2] }
	})",
		errors);
	assert(settings.no_aimpunch);                 // wrong type: default kept
	assert(!settings.hide_ragdolls);              // taken
	assert(settings.anti_rush.unlock_seconds == 60); // out of range: default kept
	assert(settings.anti_rush.callout_seconds == 1.5f);
	assert((settings.anti_rush.line_color == config::Rgba {10, 20, 30, 255}));
	assert(settings.anti_rush.glow_color_t[0] == 255); // bad color: default kept
	assert(settings.admins.size() == 2 && settings.admins[0] == 76561198353131845ull);
	assert(errors.size() == 5); // "yes", typo, "123", 900, glow_color_t

	errors.clear();
	settings = config::Parse("{ not json", errors);
	assert(errors.size() == 1 && settings.short_decals);

	std::puts("config tests passed");
	return 0;
}
