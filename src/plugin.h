#pragma once

#include <ISmmPlugin.h>
#include <engine2/isource2server.h>

#include <chrono>

class NoHitEffects final : public ISmmPlugin
{
public:
	NoHitEffects();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	bool Unload(char *error, size_t max_length) override;

	const char *GetAuthor() override { return "karola3vax"; }
	const char *GetName() override { return "No Hit Effects"; }
	const char *GetDescription() override { return "Removes hit aimpunch, hit slowdown and ragdolls, and shortens bullet decals"; }
	const char *GetURL() override { return ""; }
	const char *GetLicense() override { return "Private"; }
	const char *GetVersion() override { return NO_HIT_EFFECTS_VERSION; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "NO_HIT_EFFECTS"; }

private:
	using Clock = std::chrono::steady_clock;

	KHook::Return<void> OnGameFrame(IServerGameDLL *, bool simulating, bool first_tick, bool last_tick);

	KHook::Virtual<IServerGameDLL, void, bool, bool, bool> game_frame_;
	IServerGameDLL *server_ = nullptr;
	Clock::time_point next_settings_check_ {};
};

PLUGIN_GLOBALVARS();
