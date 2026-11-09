// g++ -std=c++20 -I src src/anti_rush.cpp tests/anti_rush_test.cpp -o anti_rush_test && ./anti_rush_test

#include "anti_rush.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace anti_rush;

int main()
{
	std::vector<std::string> errors;
	const Config config = ParseConfig("# comment\n"
									  "unlock_seconds 45\n"
									  "aim_map 100 -50 0  -100 50 128   # corners in any order\n"
									  "aim_map 1 2 3\n"
									  "\n",
		&errors);
	assert(config.unlock_seconds == 45);
	assert(config.boxes.at("aim_map").size() == 1);
	const Box box = config.boxes.at("aim_map")[0];
	assert(box.min[0] == -100 && box.max[0] == 100 && box.min[1] == -50 && box.max[2] == 128);
	assert(errors.size() == 1);
	assert(ParseConfig("unlock_seconds -5").unlock_seconds == 60);

	assert(Inside(box, {0, 0, 10}));
	assert(!Inside(box, {0, 0, 200}));
	assert(!Inside(box, {101, 0, 10}));

	assert(Locked(10, 5, 5, 60));
	assert(!Locked(60, 5, 5, 60));
	assert(!Locked(10, 1, 5, 60));
	assert(!Locked(10, 5, 1, 60));
	assert(!Locked(10, 5, 0, 60));

	// 10 units in from the -x side, walking +x: inward speed gone, eased out at 10 * 10, y and z kept.
	Vec v = Block(box, {-90, 0, 10}, {250, 30, 5}, 10);
	assert(v[0] == -100 && v[1] == 30 && v[2] == 5);
	// Barely touching while walking along the wall: sliding kept, no bounce.
	v = Block(box, {-99.5f, 0, 10}, {0, 250, 0}, 10);
	assert(v[0] == -5 && v[1] == 250);
	// Already leaving faster than the ease-out: kept.
	v = Block(box, {95, 0, 10}, {300, 0, 0}, 10);
	assert(v[0] == 300);
	// Nearest wall is +y.
	v = Block(box, {0, 45, 10}, {0, -100, 0}, 10);
	assert(v[1] == 50 && v[0] == 0);

	assert(MovingIn(box, {-95, 0, 10}, {250, 0, 0}));     // walking into the -x side
	assert(!MovingIn(box, {-95, 0, 10}, {-250, 0, 0}));   // walking away
	assert(!MovingIn(box, {-95, 0, 10}, {0, 250, 0}));    // sliding along it
	assert(MovingIn(box, {0, 45, 10}, {0, -100, 0}));     // into the +y side

	const auto edges = Edges(box);
	for (const auto &edge : edges)
	{
		int differing = 0;
		for (int axis = 0; axis < 3; ++axis) differing += edge[0][axis] != edge[1][axis];
		assert(differing == 1);
	}
	std::puts("anti_rush tests passed");
	return 0;
}
