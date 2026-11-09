#include "config.h"

#include <nlohmann/json.hpp>

#include <set>

namespace config
{
namespace
{

using Json = nlohmann::json;

// Reads one known key from an object and reports anything it can't use.
class Reader
{
public:
	Reader(const Json &object, std::string path, std::vector<std::string> &errors)
		: object_(object), path_(std::move(path)), errors_(errors)
	{
	}

	~Reader()
	{
		if (!object_.is_object()) return;
		for (const auto &[key, value] : object_.items())
			if (!seen_.count(key)) errors_.push_back(path_ + key + ": unknown setting, ignored");
	}

	void Bool(const char *key, bool &out) { Read(key, out, [](const Json &v) { return v.is_boolean(); }, "true or false"); }

	void Int(const char *key, int &out, int min, int max)
	{
		int value = out;
		if (Read(key, value, [](const Json &v) { return v.is_number_integer(); }, "a whole number")) Range(key, out, value, min, max);
	}

	void Float(const char *key, float &out, float min, float max)
	{
		float value = out;
		if (Read(key, value, [](const Json &v) { return v.is_number(); }, "a number")) Range(key, out, value, min, max);
	}

	void Color(const char *key, Rgba &out)
	{
		const Json *value = Find(key);
		if (!value) return;
		bool ok = value->is_array() && (value->size() == 3 || value->size() == 4);
		for (std::size_t i = 0; ok && i < value->size(); ++i)
			ok = (*value)[i].is_number_integer() && (*value)[i].get<int>() >= 0 && (*value)[i].get<int>() <= 255;
		if (!ok)
		{
			errors_.push_back(path_ + key + ": must be [red, green, blue] or [red, green, blue, alpha], each 0-255");
			return;
		}
		for (std::size_t i = 0; i < value->size(); ++i) out[i] = static_cast<std::uint8_t>((*value)[i].get<int>());
		if (value->size() == 3) out[3] = 255;
	}

	void SteamIds(const char *key, std::vector<std::uint64_t> &out)
	{
		const Json *value = Find(key);
		if (!value) return;
		if (!value->is_array())
		{
			errors_.push_back(path_ + key + ": must be a list of SteamID64s in quotes");
			return;
		}
		for (const Json &item : *value)
		{
			// 17-digit SteamID64s starting with 7656119; quoted so JSON tools keep every digit.
			const std::string text = item.is_string() ? item.get<std::string>() : item.dump();
			std::uint64_t id = 0;
			bool ok = text.size() == 17 && text.rfind("7656119", 0) == 0;
			for (char c : text) ok = ok && c >= '0' && c <= '9';
			if (ok)
			{
				for (char c : text) id = id * 10 + static_cast<std::uint64_t>(c - '0');
				out.push_back(id);
			}
			else
				errors_.push_back(path_ + key + ": \"" + text + "\" is not a SteamID64 (17 digits, starts with 7656119)");
		}
	}

	const Json *Object(const char *key)
	{
		const Json *value = Find(key);
		if (value && !value->is_object())
		{
			errors_.push_back(path_ + key + ": must be a { ... } section");
			return nullptr;
		}
		return value;
	}

private:
	const Json *Find(const char *key)
	{
		seen_.insert(key);
		if (!object_.is_object() || !object_.contains(key)) return nullptr;
		return &object_.at(key);
	}

	template <typename T, typename Check>
	bool Read(const char *key, T &out, Check check, const char *expected)
	{
		const Json *value = Find(key);
		if (!value) return false;
		if (!check(*value))
		{
			errors_.push_back(path_ + key + ": must be " + expected);
			return false;
		}
		out = value->get<T>();
		return true;
	}

	template <typename T>
	void Range(const char *key, T &out, T value, T min, T max)
	{
		if (value < min || value > max)
			errors_.push_back(path_ + key + ": must be between " + std::to_string(min) + " and " + std::to_string(max));
		else
			out = value;
	}

	const Json &object_;
	std::string path_;
	std::vector<std::string> &errors_;
	std::set<std::string> seen_;
};

} // namespace

Settings Parse(std::string_view text, std::vector<std::string> &errors)
{
	Settings settings;
	const Json root = Json::parse(text, nullptr, false, true); // no exceptions, comments allowed
	if (root.is_discarded() || !root.is_object())
	{
		errors.push_back("not valid JSON; check commas and quotes. Using the defaults");
		return settings;
	}
	Reader top(root, "", errors);
	top.Bool("no_aimpunch", settings.no_aimpunch);
	top.Bool("no_slowdown", settings.no_slowdown);
	top.Bool("hide_ragdolls", settings.hide_ragdolls);
	top.Bool("short_decals", settings.short_decals);
	top.SteamIds("admins", settings.admins);
	top.Bool("update_check", settings.update_check);
	if (const Json *section = top.Object("anti_rush"))
	{
		auto &a = settings.anti_rush;
		Reader rush(*section, "anti_rush.", errors);
		rush.Bool("enabled", a.enabled);
		rush.Int("unlock_seconds", a.unlock_seconds, 0, 600);
		rush.Int("unlock_players_left", a.unlock_players_left, 0, 32);
		rush.Bool("round_messages", a.round_messages);
		rush.Color("line_color", a.line_color);
		rush.Color("touched_color", a.touched_color);
		rush.Bool("callout", a.callout);
		rush.Float("callout_seconds", a.callout_seconds, 0.0f, 10.0f);
		rush.Bool("glow", a.glow);
		rush.Float("glow_seconds", a.glow_seconds, 0.0f, 60.0f);
		rush.Color("glow_color_t", a.glow_color_t);
		rush.Color("glow_color_ct", a.glow_color_ct);
		rush.Bool("center_message", a.center_message);
		rush.Float("center_seconds", a.center_seconds, 0.0f, 10.0f);
		rush.Int("damage_per_second", a.damage_per_second, 0, 100);
		rush.Bool("damage_can_kill", a.damage_can_kill);
		rush.Bool("damage_sound", a.damage_sound);
		rush.Bool("warning_sound", a.warning_sound);
	}
	return settings;
}

} // namespace config
