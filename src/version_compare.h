#pragma once

// Release version compare for the update check. Standard library only, so the test needs no SDK.

#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

namespace update_check
{

struct Version
{
	std::vector<std::uint64_t> parts; // 1.2 and 1.2.0 compare equal: missing parts count as 0
	bool prerelease = false;          // anything after '-'
	bool valid = false;
};

inline Version ParseVersion(std::string_view text)
{
	Version version;
	if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
	if (text.empty() || text.size() > 64) return version;
	for (const char c : text)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+'))
			return version; // the tag comes from the network and is printed: only plain version characters
	const std::string_view core = text.substr(0, text.find_first_of("-+"));
	version.prerelease = text.find('-') < text.find('+');
	for (std::size_t start = 0;;)
	{
		const std::size_t end = core.find('.', start);
		const std::string_view part = core.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
		std::uint64_t number = 0;
		const auto [ptr, error] = std::from_chars(part.data(), part.data() + part.size(), number);
		if (part.empty() || error != std::errc() || ptr != part.data() + part.size()) return version;
		version.parts.push_back(number);
		if (end == std::string_view::npos) break;
		start = end + 1;
	}
	while (!version.parts.empty() && version.parts.back() == 0) version.parts.pop_back();
	version.valid = true;
	return version;
}

// True when `latest` is a later release than `current`. Unreadable versions are never newer.
// shortcut: two pre-releases of the same version never count as newer; GitHub's releases/latest skips pre-releases anyway.
inline bool Newer(std::string_view latest, std::string_view current)
{
	const Version a = ParseVersion(latest);
	const Version b = ParseVersion(current);
	if (!a.valid || !b.valid) return false;
	if (a.parts != b.parts) return a.parts > b.parts; // trailing zeros are trimmed, so the vector order is numeric
	return !a.prerelease && b.prerelease;              // 1.2.0 is newer than 1.2.0-rc1, never the reverse
}

} // namespace update_check
