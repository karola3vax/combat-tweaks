#include "plugin.h"

#include "admin.h"
#include "barrier.h"
#include "chat.h"
#include "config.h"
#include "schema.h"
#include "update_check.h"
#ifdef COMBAT_TWEAKS_TRACE
#include "trace.h"
#endif

#include <engine2/igameresourceservice.h>
#include <engine2/inetworkserverservice.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{

// DFLAG_SUPPRESS_PHYSICS_FORCE. The game ORs the pawn's flags into each hit, and the tagging step
// exits on this bit before it slows the server's pawn or tells the client to slow down.
constexpr std::uint64_t kNoTagging = 2;

struct Setting
{
	const char *name;
	float wanted;
	std::uint64_t strip; // flags that block the value on a normal server
	bool *enabled;       // the config switch for it
	float original = 0.0f;
	std::uint64_t stripped = 0;
	bool active = false; // this plugin currently owns the value
};

config::Settings g_config;
std::vector<std::string> g_config_errors;
std::string g_directory;

Setting g_settings[] = {
	// Scales every damage aimpunch. Dev-only cheat convar; only the server computes this punch.
	{"mp_flinch_punch_scale", 0.0f, FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, &g_config.no_aimpunch},
	// Fade start of decals, in seconds (the misspelling is the game's). Clients add a fixed 3 s fade.
	// Must not go negative: anything below 0 other than -1 means the decal never fades.
	{"r_decals_overide_fadestarttime_params", 0.0f, 0, &g_config.short_decals},
};

std::ptrdiff_t g_damage_flags_offset = -1; // CBaseEntity::m_nTakeDamageFlags
std::ptrdiff_t g_pawn_offset = -1;         // CCSPlayerController::m_hPlayerPawn
std::ptrdiff_t g_life_state_offset = -1;   // CBaseEntity::m_lifeState
std::ptrdiff_t g_render_color_offset = -1; // CBaseModelEntity::m_clrRender
std::ptrdiff_t g_render_mode_offset = -1;  // CBaseModelEntity::m_nRenderMode
bool g_barrier_ready = false;
std::string g_barrier_error;
bool g_chat_ready = false;

constexpr std::uint8_t kLifeAlive = 0;
constexpr std::uint8_t kRenderNone = 2;
constexpr std::ptrdiff_t kColorAlpha = 3; // Color is r, g, b, a

// A pawn this plugin made invisible and how it looked before.
struct Body
{
	CEntityInstance *pawn = nullptr;
	std::uint8_t alpha = 0;
	std::uint8_t render_mode = 0;
};
std::array<Body, 64> g_bodies;

void RenderChanged(CEntityInstance *pawn, std::ptrdiff_t offset)
{
	NetworkStateChangedData data(static_cast<uint32>(offset));
	pawn->NetworkStateChanged(data);
}

// The client's ragdoll looks like its pawn, so an invisible dead pawn leaves no visible body.
void UpdateBody(Body &body, CEntityInstance *pawn, bool hide)
{
	if (body.pawn != pawn) body = {}; // a different pawn: the old one is gone, with nothing to restore
	if (hide == (body.pawn != nullptr)) return;
	auto &alpha = schema::Field<std::uint8_t>(pawn, g_render_color_offset + kColorAlpha);
	auto &render_mode = schema::Field<std::uint8_t>(pawn, g_render_mode_offset);
	if (hide)
	{
		body = {pawn, alpha, render_mode};
		alpha = 0;
		render_mode = kRenderNone;
	}
	else
	{
		alpha = body.alpha;
		render_mode = body.render_mode;
		body = {};
	}
	RenderChanged(pawn, g_render_color_offset);
	RenderChanged(pawn, g_render_mode_offset);
}

CEntityInstance *Pawn(int slot)
{
	CGameEntitySystem *entities = GameEntitySystem();
	CEntityInstance *controller = entities ? entities->GetPlayerController(CPlayerSlot(slot)) : nullptr;
	if (!controller || std::strcmp(controller->GetClassname(), "cs_player_controller") != 0) return nullptr;
	return entities->GetEntityInstance(schema::Field<CEntityHandle>(controller, g_pawn_offset));
}

// Takes over the convars of switched-on features and hands back those of switched-off ones.
void ApplySettings()
{
	for (Setting &setting : g_settings)
	{
		ConVarRefAbstract convar(setting.name, true);
		if (*setting.enabled && !setting.active)
		{
			setting.original = convar.GetFloat();
			setting.stripped = convar.GetFlags() & setting.strip;
			convar.RemoveFlags(setting.stripped);
			setting.active = true;
		}
		else if (!*setting.enabled && setting.active)
		{
			convar.SetFloat(setting.original);
			convar.AddFlags(setting.stripped);
			setting.active = false;
		}
		if (setting.active && convar.GetFloat() != setting.wanted) convar.SetFloat(setting.wanted);
	}
}

std::string ReadFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void LoadConfig()
{
	const std::string path = g_directory + "/combat_tweaks.jsonc";
	const std::string text = ReadFile(path);
	g_config_errors.clear();
	g_config = text.empty() ? config::Settings {} : config::Parse(text, g_config_errors);
	if (text.empty()) g_config_errors.push_back("could not read " + path + "; using the defaults");
	for (const std::string &line : g_config_errors) META_CONPRINTF("[COMBAT_TWEAKS] combat_tweaks.jsonc: %s\n", line.c_str());
}

void Reload()
{
	LoadConfig();
	ApplySettings();
	if (g_barrier_ready)
	{
		barrier::Configure(g_config.anti_rush);
		barrier::ReloadZones();
	}
	admin::Configure(g_config.admins);
	chat::Reload();
	update_check::Enabled(g_config.update_check);
}

void Status(std::vector<std::string> &lines)
{
	auto on = [](bool value) { return value ? "on" : "off"; };
	char line[256];
	lines.push_back(std::string("Combat Tweaks ") + COMBAT_TWEAKS_VERSION);
	std::snprintf(line, sizeof(line), "No aimpunch: %s; no slowdown: %s; hide ragdolls: %s; short decals: %s",
		on(g_config.no_aimpunch), on(g_config.no_slowdown), on(g_config.hide_ragdolls), on(g_config.short_decals));
	lines.push_back(line);
	if (g_barrier_ready) barrier::Status(lines);
	else lines.push_back("Anti-rush: unavailable, " + g_barrier_error);
	chat::Status(lines);
	std::snprintf(line, sizeof(line), "Admins: %d; update check: %s", static_cast<int>(g_config.admins.size()),
		on(g_config.update_check));
	lines.push_back(line);
	if (g_config_errors.empty()) lines.push_back("combat_tweaks.jsonc: no problems");
	for (const std::string &error : g_config_errors) lines.push_back("combat_tweaks.jsonc: " + error);
}

} // namespace

CombatTweaks g_CombatTweaks;
PLUGIN_EXPOSE(CombatTweaks, g_CombatTweaks);

CombatTweaks::CombatTweaks()
	: game_frame_(&IServerGameDLL::GameFrame, this, nullptr, &CombatTweaks::OnGameFrame)
{
}

bool CombatTweaks::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	ISchemaSystem *schema_system = nullptr;
	INetworkServerService *network_service = nullptr;
	GET_V_IFACE_ANY(GetServerFactory, server_, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, schema_system, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService,
		GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, network_service, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

	CSchemaSystemTypeScope *scope = schema::ServerScope(schema_system);
	g_damage_flags_offset = schema::FieldOffset(scope, "CBaseEntity", "m_nTakeDamageFlags");
	g_pawn_offset = schema::FieldOffset(scope, "CCSPlayerController", "m_hPlayerPawn");
	g_life_state_offset = schema::FieldOffset(scope, "CBaseEntity", "m_lifeState");
	g_render_color_offset = schema::FieldOffset(scope, "CBaseModelEntity", "m_clrRender");
	g_render_mode_offset = schema::FieldOffset(scope, "CBaseModelEntity", "m_nRenderMode");
	if (g_damage_flags_offset < 0 || g_pawn_offset < 0 || g_life_state_offset < 0 || g_render_color_offset < 0
		|| g_render_mode_offset < 0)
	{
		std::snprintf(error, maxlen,
			"CS2's schema is missing a field (m_nTakeDamageFlags %d, m_hPlayerPawn %d, m_lifeState %d, m_clrRender %d, m_nRenderMode %d)",
			static_cast<int>(g_damage_flags_offset), static_cast<int>(g_pawn_offset), static_cast<int>(g_life_state_offset),
			static_cast<int>(g_render_color_offset), static_cast<int>(g_render_mode_offset));
		return false;
	}
#ifdef COMBAT_TWEAKS_TRACE
	if (!trace::Init(scope, error, maxlen)) return false;
#endif
	for (const Setting &setting : g_settings)
	{
		ConVarRefAbstract convar(setting.name, true);
		if (convar.IsValidRef() && convar.IsConVarDataAvailable()) continue;
		std::snprintf(error, maxlen, "CS2 no longer has the convar %s", setting.name);
		return false;
	}

	g_directory = std::string(ismm->GetBaseDir()) + "/addons/combat_tweaks";
	LoadConfig();
	ApplySettings();
	g_chat_ready = chat::Init(g_directory + "/translations");
	if (!g_chat_ready) META_CONPRINTF("[COMBAT_TWEAKS] Chat messages are off: this CS2 version changed how they are sent.\n");
	g_barrier_ready = barrier::Init(scope, server_, network_service, g_directory + "/zones.jsonc", g_barrier_error);
	if (g_barrier_ready) barrier::Configure(g_config.anti_rush);
	else META_CONPRINTF("[COMBAT_TWEAKS] Anti-rush is off: %s.\n", g_barrier_error.c_str());
	if (!admin::Init(scope, {Reload, Status})) META_CONPRINTF("[COMBAT_TWEAKS] The ct command is off: CS2's schema changed.\n");
	admin::Configure(g_config.admins);
	update_check::Enabled(g_config.update_check);
	update_check::Start(COMBAT_TWEAKS_VERSION);
	g_SMAPI->AddListener(this, this);
	game_frame_.Add(server_);
	META_CONPRINTF("[COMBAT_TWEAKS] Combat Tweaks %s loaded%s. Type \"ct status\" for details.\n", COMBAT_TWEAKS_VERSION,
		late ? " late" : "");
	return true;
}

bool CombatTweaks::Unload(char *, size_t)
{
	game_frame_.Remove(server_);
	update_check::Stop();
	admin::Shutdown();
	barrier::Shutdown();
	chat::Shutdown();
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *pawn = Pawn(slot);
		if (!pawn) continue;
		schema::Field<std::uint64_t>(pawn, g_damage_flags_offset) &= ~kNoTagging;
		UpdateBody(g_bodies[slot], pawn, false);
	}
	for (Setting &setting : g_settings)
	{
		if (!setting.active) continue;
		ConVarRefAbstract convar(setting.name, true);
		convar.SetFloat(setting.original);
		convar.AddFlags(setting.stripped);
		setting.active = false;
	}
	return true;
}

void CombatTweaks::OnLevelInit(const char *, const char *, const char *, const char *, bool, bool)
{
	Reload(); // config edits take effect on the next map
}

KHook::Return<void> CombatTweaks::OnGameFrame(IServerGameDLL *, bool simulating, bool, bool)
{
	if (!simulating) return {KHook::Action::Ignore};
	// Each frame rather than on spawn: a new pawn starts without the flag, and this also covers a late load.
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *pawn = Pawn(slot);
		if (!pawn) continue;
		const bool dead = schema::Field<std::uint8_t>(pawn, g_life_state_offset) != kLifeAlive;
		UpdateBody(g_bodies[slot], pawn, g_config.hide_ragdolls && dead);
#ifdef COMBAT_TWEAKS_TRACE
		trace::Frame(slot, pawn);
		if (slot % 2 != 0) continue; // odd slots keep the normal slowdown, as the control group
#endif
		auto &flags = schema::Field<std::uint64_t>(pawn, g_damage_flags_offset);
		if (g_config.no_slowdown) flags |= kNoTagging;
		else flags &= ~kNoTagging;
	}
	if (g_barrier_ready) barrier::Frame();
	if (g_chat_ready) chat::Frame();
	const auto now = Clock::now();
	if (now >= next_settings_check_)
	{
		next_settings_check_ = now + std::chrono::seconds(1);
#ifndef COMBAT_TWEAKS_TRACE // the trace build leaves the settings alone so a test can compare values
		// Map changes and game mode configs write these back (Valve's deathmatch config sets the decal one).
		ApplySettings();
#endif
		update_check::Frame();
	}
	return {KHook::Action::Ignore};
}
