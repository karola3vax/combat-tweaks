#include "anti_rush.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace anti_rush
{

Config ParseConfig(std::string_view text, std::vector<std::string> *errors)
{
	Config config;
	std::istringstream lines {std::string(text)};
	std::string line;
	for (int number = 1; std::getline(lines, line); ++number)
	{
		line = line.substr(0, line.find('#'));
		std::istringstream words(line);
		std::string first;
		if (!(words >> first)) continue;
		std::string rest;
		if (first == "unlock_seconds")
		{
			int seconds = 0;
			if (words >> seconds && seconds >= 0 && !(words >> rest))
			{
				config.unlock_seconds = seconds;
				continue;
			}
		}
		else
		{
			Vec a {}, b {};
			if (words >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2] && !(words >> rest))
			{
				Box box;
				for (int axis = 0; axis < 3; ++axis)
				{
					box.min[axis] = std::min(a[axis], b[axis]);
					box.max[axis] = std::max(a[axis], b[axis]);
				}
				config.boxes[first].push_back(box);
				continue;
			}
		}
		if (errors) errors->push_back("line " + std::to_string(number) + ": " + line);
	}
	return config;
}

bool Inside(const Box &box, const Vec &point)
{
	for (int axis = 0; axis < 3; ++axis)
		if (point[axis] < box.min[axis] || point[axis] > box.max[axis]) return false;
	return true;
}

bool Locked(float seconds_since_round_start, int alive_t, int alive_ct, int unlock_seconds)
{
	return seconds_since_round_start < static_cast<float>(unlock_seconds) && alive_t > 1 && alive_ct > 1;
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
