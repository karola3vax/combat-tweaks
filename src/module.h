#pragma once

// Finding things inside a loaded game module (server.dll / libserver.so).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace module
{

struct Segment
{
	const std::uint8_t *start;
	std::size_t size;
};

// The mapped parts of the module that contains `address`.
std::vector<Segment> Segments(const void *address);

// The one place matching the pattern ("48 8B ? ..."; "?" is any byte), or nullptr when it is missing
// or not unique.
void *FindUnique(const std::vector<Segment> &segments, const char *pattern);

// The virtual function table of a class, found through its run-time type information, or nullptr.
void *FindVtable(const std::vector<Segment> &segments, const char *class_name);

// The one object inside the module whose first word is `vtable` (a global instance of that class),
// or nullptr when there is none or more than one.
void *FindStaticObject(const std::vector<Segment> &segments, void *vtable);

} // namespace module
