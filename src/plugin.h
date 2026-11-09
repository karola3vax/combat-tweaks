#pragma once

#include <ISmmPlugin.h>
#include <engine2/isource2server.h>

#include <chrono>
#include <string>

class CombatTweaks final : public ISmmPlugin, public IMetamodListener
{
public:
	CombatTweaks();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	bool Unload(char *error, size_t max_length) override;
	void OnLevelInit(const char *, const char *, const char *, const char *, bool, bool) override;

	const char *GetAuthor() override { return "karola3vax"; }
	const char *GetName() override { return "Combat Tweaks"; }
	const char *GetDescription() override
	{
		return "No hit aimpunch, slowdown or ragdolls, short decals, and anti-rush barriers";
	}
	const char *GetURL() override { return "https://github.com/karola3vax/combat-tweaks"; }
	const char *GetLicense() override { return "AGPL-3.0-only"; }
	const char *GetVersion() override { return COMBAT_TWEAKS_VERSION; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "COMBAT_TWEAKS"; }

private:
	using Clock = std::chrono::steady_clock;

	KHook::Return<void> OnGameFrame(IServerGameDLL *, bool simulating, bool first_tick, bool last_tick);

	KHook::Virtual<IServerGameDLL, void, bool, bool, bool> game_frame_;
	IServerGameDLL *server_ = nullptr;
	Clock::time_point next_settings_check_ {};
};

PLUGIN_GLOBALVARS();
