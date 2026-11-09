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

using Zones = std::unordered_map<std::string, std::vector<Box>>; // by map name

// The box between any two opposite corners.
Box MakeBox(const Vec &a, const Vec &b);

// zones.jsonc: { "<map>": [ { "from": [x, y, z], "to": [x, y, z] }, ... ], ... }. Comments are allowed.
// Bad entries are skipped and reported in `errors`. Never throws.
Zones ParseZones(std::string_view text, std::vector<std::string> &errors);

// zones.jsonc text for the zones, with a short header; ParseZones reads it back.
std::string FormatZones(const Zones &zones);

bool Inside(const Box &box, const Vec &point);

// Locked until the timer ends (0: no timer) or either team is down to `players_left` living players.
bool Locked(float seconds_since_round_start, int alive_t, int alive_ct, int unlock_seconds, int players_left);

// Velocity that makes the box act like a wall for a player overlapping it: movement into the
// nearest side (x or y; height is left alone) is cancelled, sliding along it is kept, and the
// player eases back out at `eject_rate` units per second for each unit of overlap.
Vec Block(const Box &box, const Vec &position, const Vec &velocity, float eject_rate);

// True when a player overlapping the box is moving into it through the nearest side wall.
bool MovingIn(const Box &box, const Vec &position, const Vec &velocity);

// The 12 edges of the box, as start and end points, for drawing.
std::array<std::array<Vec, 2>, 12> Edges(const Box &box);

} // namespace anti_rush
