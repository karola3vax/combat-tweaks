// g++ -std=c++20 -I src -I third_party tests/update_check_test.cpp -o update_check_test && ./update_check_test

#include "version_compare.h"

#include <cassert>
#include <cstdio>

using update_check::Newer;

int main()
{
	assert(Newer("v1.2.10", "1.2.3"));
	assert(!Newer("v1.2.3", "1.2.10"));
	assert(Newer("1.10.0", "1.9.9"));
	assert(Newer("2", "1.99.99"));
	assert(!Newer("v1.2.3", "1.2.3"));
	assert(!Newer("V1.2", "1.2.0")); // missing parts count as 0
	assert(!Newer("1.2.0", "1.2"));
	assert(Newer("1.2.0.1", "1.2"));
	assert(Newer("0.1.1", "0.1.0"));

	// A pre-release never beats the same release; the release beats its own pre-release.
	assert(!Newer("1.2.3-rc1", "1.2.3"));
	assert(Newer("1.2.3", "1.2.3-rc1"));
	assert(!Newer("1.2.3-rc2", "1.2.3-rc1"));
	assert(Newer("1.3.0-beta", "1.2.9"));
	assert(!Newer("1.2.3+build.7", "1.2.3")); // build metadata is not a version change
	assert(!Newer("1.2.3+build-7", "1.2.3"));

	// Garbage from the network is never newer, and never makes the running version look old.
	assert(!Newer("", "1.0.0"));
	assert(!Newer("v", "1.0.0"));
	assert(!Newer("latest", "1.0.0"));
	assert(!Newer("1..2", "1.0.0"));
	assert(!Newer("1.2.", "1.0.0"));
	assert(!Newer("9.0.0 %s", "1.0.0"));
	assert(!Newer("99999999999999999999999.0", "1.0.0"));
	assert(!Newer("2.0.0", "dev"));
	assert(!Newer("-1.0.0", "0.0.1"));

	std::puts("update_check_test passed");
	return 0;
}
