#include "module.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <link.h>
#endif

namespace module
{
namespace
{

// Every place where `needle` occurs.
std::vector<const std::uint8_t *> FindAll(const std::vector<Segment> &segments, const void *needle, std::size_t length,
	std::size_t alignment = 1)
{
	std::vector<const std::uint8_t *> hits;
	for (const Segment &segment : segments)
	{
		if (length > segment.size) continue;
		const std::uint8_t *end = segment.start + segment.size - length;
		for (const std::uint8_t *at = segment.start; at <= end; at += alignment)
			if (std::memcmp(at, needle, length) == 0) hits.push_back(at);
	}
	return hits;
}

template <typename T>
T Read(const std::uint8_t *at)
{
	T value;
	std::memcpy(&value, at, sizeof(T));
	return value;
}

} // namespace

std::vector<Segment> Segments(const void *address)
{
	std::vector<Segment> segments;
#ifdef _WIN32
	HMODULE handle = nullptr;
	MODULEINFO info {};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<const char *>(address), &handle)
		&& GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
		segments.push_back({static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage});
#else
	struct Search
	{
		std::uintptr_t address;
		std::vector<Segment> *result;
	} search {reinterpret_cast<std::uintptr_t>(address), &segments};
	dl_iterate_phdr(
		[](dl_phdr_info *info, size_t, void *data) -> int {
			auto *search = static_cast<Search *>(data);
			std::vector<Segment> found;
			bool contains = false;
			for (int i = 0; i < info->dlpi_phnum; ++i)
			{
				const ElfW(Phdr) &header = info->dlpi_phdr[i];
				if (header.p_type != PT_LOAD || header.p_memsz == 0) continue;
				const std::uintptr_t start = info->dlpi_addr + header.p_vaddr;
				found.push_back({reinterpret_cast<const std::uint8_t *>(start), static_cast<std::size_t>(header.p_memsz)});
				contains |= search->address >= start && search->address < start + header.p_memsz;
			}
			if (!contains) return 0;
			*search->result = std::move(found);
			return 1;
		},
		&search);
#endif
	return segments;
}

void *FindUnique(const std::vector<Segment> &segments, const char *pattern)
{
	std::vector<std::optional<std::uint8_t>> bytes;
	for (const char *at = pattern; *at;)
	{
		if (*at == ' ') ++at;
		else if (*at == '?') bytes.emplace_back(), ++at;
		else
		{
			bytes.emplace_back(static_cast<std::uint8_t>(std::strtoul(at, nullptr, 16)));
			at += 2;
		}
	}
	void *found = nullptr;
	int count = 0;
	for (const Segment &segment : segments)
	{
		if (bytes.size() > segment.size) continue;
		for (const std::uint8_t *at = segment.start; at + bytes.size() <= segment.start + segment.size && count < 2; ++at)
		{
			std::size_t i = 0;
			while (i < bytes.size() && (!bytes[i] || at[i] == *bytes[i])) ++i;
			if (i == bytes.size()) found = const_cast<std::uint8_t *>(at), ++count;
		}
	}
	return count == 1 ? found : nullptr;
}

void *FindVtable(const std::vector<Segment> &segments, const char *class_name)
{
	if (segments.empty()) return nullptr;
#ifdef _WIN32
	// MSVC: type descriptor (vtable pointer, spare, ".?AVName@@"), the complete object locator that
	// points at it by image offset, and the vtable, which starts right after a pointer to that locator.
	const std::uint8_t *base = segments.front().start;
	const std::string mangled = std::string(".?AV") + class_name + "@@";
	for (const std::uint8_t *name : FindAll(segments, mangled.c_str(), mangled.size() + 1))
	{
		const auto descriptor = static_cast<std::uint32_t>(name - 0x10 - base);
		for (const std::uint8_t *reference : FindAll(segments, &descriptor, sizeof(descriptor), 4))
		{
			const std::uint8_t *locator = reference - 12;
			if (locator < base || Read<std::uint32_t>(locator) != 1 || Read<std::uint32_t>(locator + 4) != 0) continue;
			if (Read<std::uint32_t>(locator + 20) != static_cast<std::uint32_t>(locator - base)) continue;
			const auto address = reinterpret_cast<std::uintptr_t>(locator);
			for (const std::uint8_t *slot : FindAll(segments, &address, sizeof(address), 8))
				return const_cast<std::uint8_t *>(slot + 8);
		}
	}
#else
	// Itanium: the type name ("17CGameEventManager"), the typeinfo whose second word points at it,
	// and the vtable, which follows an offset-to-top of 0 and a pointer to that typeinfo.
	const std::string mangled = std::to_string(std::strlen(class_name)) + class_name;
	for (const std::uint8_t *name : FindAll(segments, mangled.c_str(), mangled.size() + 1))
	{
		if (name > segments.front().start && name[-1] != '\0') continue; // a whole string, not the end of a longer one
		const auto name_address = reinterpret_cast<std::uintptr_t>(name);
		for (const std::uint8_t *reference : FindAll(segments, &name_address, sizeof(name_address), 8))
		{
			const auto typeinfo = reinterpret_cast<std::uintptr_t>(reference - 8);
			for (const std::uint8_t *slot : FindAll(segments, &typeinfo, sizeof(typeinfo), 8))
				if (Read<std::intptr_t>(slot - 8) == 0) return const_cast<std::uint8_t *>(slot + 8);
		}
	}
#endif
	return nullptr;
}

void *FindStaticObject(const std::vector<Segment> &segments, void *vtable)
{
	const auto address = reinterpret_cast<std::uintptr_t>(vtable);
	const std::uint8_t *found = nullptr;
	for (const std::uint8_t *at : FindAll(segments, &address, sizeof(address), 8))
	{
		if (found) return nullptr; // not unique
		found = at;
	}
	return const_cast<std::uint8_t *>(found);
}

} // namespace module
