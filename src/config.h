#pragma once

// combat_tweaks.jsonc: what server owners can change. Pure code, so it can be tested on its own.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace config
{

using Rgba = std::array<std::uint8_t, 4>;

struct Settings
{
	bool no_aimpunch = true;
	bool no_slowdown = true;
	bool hide_ragdolls = true;
	bool short_decals = true;

	struct AntiRush
	{
		bool enabled = true;
		int unlock_seconds = 60;
		int unlock_players_left = 1; // unlocks once a team has this many or fewer alive
		bool round_messages = true;
		Rgba line_color {120, 120, 120, 160}; // barrier walls; named for the old outline, kept so configs still load
		Rgba touched_color {255, 0, 0, 255};
		bool callout = true;
		float callout_seconds = 0.5f; // walking into a barrier this long counts as rushing
		bool glow = true;
		float glow_seconds = 3.0f;
		Rgba glow_color_t {240, 195, 105, 100};  // gold, as CS2's spectator x-ray draws T
		Rgba glow_color_ct {135, 180, 235, 100}; // sky blue, as the x-ray draws CT
		bool center_message = true;             // a private center-screen note to the rusher
		float center_seconds = 5.0f;
		int damage_per_second = 50; // while pushing into the barrier; 0 turns it off
		bool damage_can_kill = true;
		bool damage_sound = true; // the game's body-hit sound on each hit
		bool warning_sound = true; // a UI warning sound, only for the rusher, when caught
	} anti_rush;

	std::vector<std::uint64_t> admins; // SteamID64s allowed to use !ct commands
	bool update_check = true;
};

// Missing keys keep their defaults; wrong types, bad values and unknown keys are reported in `errors`
// and fall back to the default. Never throws.
Settings Parse(std::string_view text, std::vector<std::string> &errors);

} // namespace config
