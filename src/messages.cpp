#include "messages.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace messages
{

namespace
{

constexpr std::string_view kFallback = "en";

struct Color
{
	std::string_view token;
	char byte;
};

// CS2 chat color bytes.
constexpr Color kColors[] = {
	{"default", '\x01'},
	{"red", '\x07'},
	{"lime", '\x06'},
	{"grey", '\x08'},
	{"lightblue", '\x0B'},
	{"yellow", '\x09'},
};

const Color *FindColor(std::string_view token)
{
	for (const Color &color : kColors)
		if (color.token == token) return &color;
	return nullptr;
}

void AppendPlain(std::string &out, std::string_view value)
{
	// Player names and other outside text must not carry color codes or line breaks.
	for (char c : value)
		if (static_cast<unsigned char>(c) >= 0x20) out += c;
}

} // namespace

std::vector<std::string> Messages::Load(const std::string &folder)
{
	std::vector<std::string> errors;
	Table texts;
	std::error_code error;
	std::filesystem::directory_iterator it(folder, error), end;
	for (; !error && it != end; it.increment(error))
	{
		const std::filesystem::path &path = it->path();
		if (path.extension() != ".json") continue;
		std::string file = "a translation file";
		try
		{
			file = path.filename().string();
			std::ifstream stream(path, std::ios::binary);
			if (!stream.is_open())
			{
				errors.push_back("cannot read " + file);
				continue;
			}
			std::stringstream content;
			content << stream.rdbuf();
			const nlohmann::json json = nlohmann::json::parse(content.str(), nullptr, true, true);
			if (!json.is_object())
			{
				errors.push_back(file + ": not a JSON object");
				continue;
			}
			auto &language = texts[path.stem().string()];
			for (const auto &[key, value] : json.items())
			{
				if (key.starts_with('_')) continue; // comments
				if (value.is_string())
					language[key] = value.get<std::string>();
				else
					errors.push_back(file + ": \"" + key + "\" is not a string");
			}
		}
		catch (const std::exception &exception)
		{
			errors.push_back(file + ": " + exception.what());
		}
	}
	if (error) errors.push_back("cannot list " + folder + ": " + error.message());
	if (!texts.contains(std::string(kFallback))) errors.push_back("en.json is missing; it is the fallback for every language");
	texts_ = std::move(texts);
	return errors;
}

const std::string *Messages::Find(std::string_view language, std::string_view key) const
{
	for (std::string_view code : {language, kFallback})
	{
		const auto file = texts_.find(std::string(code));
		if (file == texts_.end()) continue;
		const auto text = file->second.find(std::string(key));
		if (text != file->second.end()) return &text->second;
	}
	return nullptr;
}

// One pass, so values are never read for tokens: a player named "{red}" stays "{red}".
void Messages::Expand(std::string &out, std::string_view text, std::string_view language, const Values &values, bool prefixes) const
{
	std::size_t at = 0;
	while (at < text.size())
	{
		const std::size_t open = text.find('{', at);
		const std::size_t close = open == std::string_view::npos ? open : text.find('}', open);
		if (close == std::string_view::npos)
		{
			out += text.substr(at);
			return;
		}
		out += text.substr(at, open - at);
		at = close + 1;
		const std::string_view token = text.substr(open + 1, close - open - 1);
		if (const Color *color = FindColor(token))
		{
			out += color->byte;
			continue;
		}
		if (prefixes && (token == "rush_prefix" || token == "admin_prefix"))
		{
			if (const std::string *prefix = Find(language, token)) Expand(out, *prefix, language, values, false);
			continue;
		}
		bool found = false;
		for (const auto &[name, value] : values)
		{
			if (name != token) continue;
			found = true;
			if (name != "team")
				AppendPlain(out, value);
			else if (const Color *color = value.size() > 2 ? FindColor(std::string_view(value).substr(1, value.size() - 2)) : nullptr)
				out += color->byte;
			break;
		}
		if (!found) out += text.substr(open, at - open); // unknown tokens stay visible
	}
}

std::string Messages::Format(std::string_view language, std::string_view key, const Values &values) const
{
	std::string out = " ";
	const std::string *text = Find(language, key);
	if (!text)
		out += key;
	else
		Expand(out, *text, language, values, true);
	return out;
}

std::string_view LanguageFile(std::string_view cl_language)
{
	static constexpr std::pair<std::string_view, std::string_view> kFiles[] = {
		{"english", "en"},
		{"turkish", "tr"},
		{"russian", "ru"},
		{"brazilian", "pt"},
		{"portuguese", "pt"},
		{"spanish", "es"},
		{"latam", "es"},
		{"german", "de"},
		{"schinese", "zh"},
		{"tchinese", "zh"},
		{"polish", "pl"},
		{"french", "fr"},
	};
	for (const auto &[game, file] : kFiles)
		if (game == cl_language) return file;
	return kFallback;
}

} // namespace messages
