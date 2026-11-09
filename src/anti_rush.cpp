#include "anti_rush.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace anti_rush
{

Box MakeBox(const Vec &a, const Vec &b)
{
	Box box;
	for (int axis = 0; axis < 3; ++axis)
	{
		box.min[axis] = std::min(a[axis], b[axis]);
		box.max[axis] = std::max(a[axis], b[axis]);
	}
	return box;
}

Zones ParseZones(std::string_view text, std::vector<std::string> &errors)
{
	using Json = nlohmann::json;
	Zones zones;
	const Json root = Json::parse(text, nullptr, false, true); // no exceptions, comments allowed
	if (root.is_discarded() || !root.is_object())
	{
		errors.push_back("not valid JSON; check commas and quotes. No zones loaded");
		return zones;
	}
	auto point = [](const Json &value, Vec &out) {
		if (!value.is_array() || value.size() != 3) return false;
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			if (!value[axis].is_number()) return false;
			out[axis] = value[axis].get<float>();
		}
		return true;
	};
	for (const auto &[map, list] : root.items())
	{
		if (!list.is_array())
		{
			errors.push_back(map + ": must be a list of zones [ ... ]");
			continue;
		}
		for (std::size_t i = 0; i < list.size(); ++i)
		{
			Vec from {}, to {};
			if (list[i].is_object() && list[i].contains("from") && list[i].contains("to") && point(list[i]["from"], from)
				&& point(list[i]["to"], to))
				zones[map].push_back(MakeBox(from, to));
			else
				errors.push_back(map + " zone " + std::to_string(i + 1) + ": needs \"from\": [x, y, z] and \"to\": [x, y, z]");
		}
	}
	return zones;
}

std::string FormatZones(const Zones &zones)
{
	std::vector<std::string> maps;
	for (const auto &[map, boxes] : zones)
		if (!boxes.empty()) maps.push_back(map);
	std::sort(maps.begin(), maps.end());
	auto number = [](float value) {
		char text[32];
		std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
		return std::string(text);
	};
	auto point = [&](const Vec &v) { return "[" + number(v[0]) + ", " + number(v[1]) + ", " + number(v[2]) + "]"; };
	std::string out = "// Combat Tweaks anti-rush barriers, by map name. Two opposite corners per box, in world units.\n"
					  "// Easiest to edit in game: \"!ct zone\" at each corner, then \"!ct zone save\".\n{\n";
	for (std::size_t m = 0; m < maps.size(); ++m)
	{
		out += "  \"" + maps[m] + "\": [\n";
		const auto &boxes = zones.at(maps[m]);
		for (std::size_t i = 0; i < boxes.size(); ++i)
			out += "    { \"from\": " + point(boxes[i].min) + ", \"to\": " + point(boxes[i].max) + " }"
				+ (i + 1 < boxes.size() ? ",\n" : "\n");
		out += std::string("  ]") + (m + 1 < maps.size() ? ",\n" : "\n");
	}
	return out + "}\n";
}

bool Inside(const Box &box, const Vec &point)
{
	for (int axis = 0; axis < 3; ++axis)
		if (point[axis] < box.min[axis] || point[axis] > box.max[axis]) return false;
	return true;
}

bool Locked(float seconds_since_round_start, int alive_t, int alive_ct, int unlock_seconds, int players_left)
{
	const bool timer_running = unlock_seconds == 0 || seconds_since_round_start < static_cast<float>(unlock_seconds);
	return timer_running && alive_t > players_left && alive_ct > players_left;
}

namespace
{

struct Wall
{
	int axis;    // 0 = x, 1 = y
	float sign;  // outward direction along the axis
	float depth; // how far inside the player is
};

// Nearest of the four side walls: that is the side the player came in from.
Wall NearestWall(const Box &box, const Vec &position)
{
	int axis = 0;
	float sign = -1.0f;
	float nearest = position[0] - box.min[0];
	const float distances[4] = {position[0] - box.min[0], box.max[0] - position[0], position[1] - box.min[1],
		box.max[1] - position[1]};
	for (int side = 1; side < 4; ++side)
		if (distances[side] < nearest)
		{
			nearest = distances[side];
			axis = side / 2;
			sign = side % 2 == 0 ? -1.0f : 1.0f;
		}
	return {axis, sign, nearest};
}

} // namespace

Vec Block(const Box &box, const Vec &position, const Vec &velocity, float eject_rate)
{
	const Wall wall = NearestWall(box, position);
	Vec result = velocity;
	result[wall.axis] = wall.sign * std::max(wall.depth * eject_rate, wall.sign * velocity[wall.axis]);
	return result;
}

bool MovingIn(const Box &box, const Vec &position, const Vec &velocity)
{
	const Wall wall = NearestWall(box, position);
	return wall.sign * velocity[wall.axis] < -1.0f; // slower than 1 unit per second is standing still
}

std::array<std::array<Vec, 2>, 12> Edges(const Box &box)
{
	auto corner = [&](int x, int y, int z) {
		return Vec {x ? box.max[0] : box.min[0], y ? box.max[1] : box.min[1], z ? box.max[2] : box.min[2]};
	};
	std::array<std::array<Vec, 2>, 12> edges;
	int i = 0;
	for (int a = 0; a < 2; ++a)
		for (int b = 0; b < 2; ++b)
		{
			edges[i++] = {corner(0, a, b), corner(1, a, b)};
			edges[i++] = {corner(a, 0, b), corner(a, 1, b)};
			edges[i++] = {corner(a, b, 0), corner(a, b, 1)};
		}
	return edges;
}

} // namespace anti_rush
