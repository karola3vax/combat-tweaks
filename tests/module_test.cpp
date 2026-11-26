// g++ -std=c++20 -I src src/module.cpp tests/module_test.cpp -o module_test && ./module_test

#include "module.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>

using namespace module;

namespace
{

// The plain byte-by-byte scan the fast path must agree with.
void *Reference(const std::vector<Segment> &segments, const char *pattern)
{
	std::vector<std::optional<std::uint8_t>> bytes;
	for (const char *at = pattern; *at;)
	{
		if (*at == ' ') ++at;
		else if (*at == '?') bytes.emplace_back(), ++at;
		else bytes.emplace_back(static_cast<std::uint8_t>(std::strtoul(at, nullptr, 16))), at += 2;
	}
	void *found = nullptr;
	int count = 0;
	for (const Segment &segment : segments)
		for (std::size_t at = 0; at + bytes.size() <= segment.size; ++at)
		{
			std::size_t i = 0;
			while (i < bytes.size() && (!bytes[i] || segment.start[at + i] == *bytes[i])) ++i;
			if (i == bytes.size()) found = const_cast<std::uint8_t *>(segment.start + at), ++count;
		}
	return count == 1 ? found : nullptr;
}

} // namespace

int main()
{
	std::uint8_t a[64] = {}, b[32] = {};
	const std::vector<Segment> segments {{a, sizeof(a)}, {b, sizeof(b)}};
	auto put = [](std::uint8_t *at, std::initializer_list<int> bytes) {
		for (int byte : bytes) *at++ = static_cast<std::uint8_t>(byte);
	};

	put(a + 10, {0x48, 0x8B, 0x05, 0x11, 0x22});
	assert(FindUnique(segments, "? 48 8B 05 ? 22") == a + 9);   // wildcards at both ends
	assert(FindUnique(segments, "48 8B 05 11 22") == a + 10);
	assert(FindUnique(segments, "48 8B 05 ?") == a + 10);
	put(b + 3, {0x48, 0x8B, 0x05, 0x33});
	assert(FindUnique(segments, "48 8B 05 ?") == nullptr);       // two matches: ambiguous
	put(b + 28, {0x55, 0x66, 0x77, 0x88});
	assert(FindUnique(segments, "55 66 77 88") == b + 28);       // ends at the last byte of a segment
	assert(FindUnique(segments, "? 55 66 77 88") == b + 27);
	assert(FindUnique(segments, "55 66 77 88 ?") == nullptr);    // would run past the segment end
	put(a + 61, {0x55, 0x66, 0x77});
	put(b, {0x88});
	assert(FindUnique(segments, "55 66 77 88") == b + 28);       // never across segments
	assert(FindUnique(segments, "?") == nullptr);                // no fixed byte: old path
	const std::vector<Segment> one {{a + 10, 1}};
	assert(FindUnique(one, "? ") == a + 10);

	// Random buffers with few byte values, so partial and repeated matches are common.
	std::mt19937 random(12345);
	for (int round = 0; round < 3000; ++round)
	{
		for (auto &byte : a) byte = static_cast<std::uint8_t>(random() % 3);
		for (auto &byte : b) byte = static_cast<std::uint8_t>(random() % 3);
		std::string pattern;
		const int length = 1 + static_cast<int>(random() % 6);
		for (int i = 0; i < length; ++i)
			pattern += random() % 3 == 0 ? std::string("? ") : "0" + std::to_string(random() % 3) + " ";
		assert(FindUnique(segments, pattern.c_str()) == Reference(segments, pattern.c_str()));
	}

	// Pointer searches stay 8-aligned from the segment start.
	alignas(8) std::uint8_t c[64] = {};
	const std::vector<Segment> pointers {{c, sizeof(c)}};
	void *vtable = reinterpret_cast<void *>(std::uintptr_t {0x1122334455667788});
	const auto value = reinterpret_cast<std::uintptr_t>(vtable);
	std::memcpy(c + 3, &value, sizeof(value));  // unaligned copy is ignored
	assert(FindStaticObject(pointers, vtable) == nullptr);
	std::memcpy(c + 24, &value, sizeof(value));
	assert(FindStaticObject(pointers, vtable) == c + 24);
	std::memcpy(c + 56, &value, sizeof(value)); // last aligned slot
	assert(FindStaticObject(pointers, vtable) == nullptr);

	std::puts("module tests passed");
}
