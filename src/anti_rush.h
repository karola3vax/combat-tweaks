#pragma once

// Anti-rush rules with no game types, so they can be tested on their own.

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anti_rush
{

using Vec = std::array<float, 3>;

struct Box
{
	Vec min {};
	Vec max {};
};

struct Config
{
	int unlock_seconds = 60;
	std::unordered_map<std::string, std::vector<Box>> boxes; // by map name
};

// Lines: `unlock_seconds <n>` or `<map> <x1> <y1> <z1> <x2> <y2> <z2>` (any two opposite corners).
// `#` starts a comment. Bad lines are skipped and reported in `errors`.
Config ParseConfig(std::string_view text, std::vector<std::string> *errors = nullptr);

bool Inside(const Box &box, const Vec &point);

// Locked until the timer ends or either team is down to one living player.
bool Locked(float seconds_since_round_start, int alive_t, int alive_ct, int unlock_seconds);

// Velocity that pushes a player inside the box out through the nearest side wall
// (x or y; height is left alone) at least at `push_speed`, keeping their other movement.
Vec PushOut(const Box &box, const Vec &position, const Vec &velocity, float push_speed);

// The 12 edges of the box, as start and end points, for drawing.
std::array<std::array<Vec, 2>, 12> Edges(const Box &box);

} // namespace anti_rush
