#include "plugin.h"

#include "barrier.h"
#include "schema.h"
#ifdef COMBAT_TWEAKS_TRACE
#include "trace.h"
#endif

#include <engine2/igameresourceservice.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>
#include <engine2/inetworkserverservice.h>
#include <schemasystem/schemasystem.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
	float original = 0.0f;
	std::uint64_t stripped = 0;
};

Setting g_settings[] = {
	// Scales every damage aimpunch. Dev-only cheat convar; only the server computes this punch.
	{"mp_flinch_punch_scale", 0.0f, FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT},
	// Fade start of decals, in seconds (the misspelling is the game's). Clients add a fixed 3 s fade.
	// Must not go negative: anything below 0 other than -1 means the decal never fades.
	{"r_decals_overide_fadestarttime_params", 0.0f, 0},
};

std::ptrdiff_t g_damage_flags_offset = -1; // CBaseEntity::m_nTakeDamageFlags
std::ptrdiff_t g_pawn_offset = -1;         // CCSPlayerController::m_hPlayerPawn
std::ptrdiff_t g_life_state_offset = -1;   // CBaseEntity::m_lifeState
std::ptrdiff_t g_render_color_offset = -1; // CBaseModelEntity::m_clrRender
std::ptrdiff_t g_render_mode_offset = -1;  // CBaseModelEntity::m_nRenderMode

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

void ApplySettings()
{
	for (Setting &setting : g_settings)
	{
		ConVarRefAbstract convar(setting.name, true);
		if (convar.GetFloat() != setting.wanted) convar.SetFloat(setting.wanted);
	}
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
	GET_V_IFACE_ANY(GetServerFactory, server_, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, schema_system, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService,
		GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

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
	for (Setting &setting : g_settings)
	{
		ConVarRefAbstract convar(setting.name, true);
		setting.original = convar.GetFloat();
		setting.stripped = convar.GetFlags() & setting.strip;
		convar.RemoveFlags(setting.stripped);
	}
	ApplySettings();
	std::string barrier_error;
	INetworkServerService *network_service = nullptr;
	GET_V_IFACE_ANY(GetEngineFactory, network_service, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	if (!barrier::Init(scope, server_, network_service, std::string(ismm->GetBaseDir()) + "/addons/combat_tweaks/anti_rush.ini", barrier_error))
		META_CONPRINTF("[COMBAT_TWEAKS] Anti-rush is off: %s.\n", barrier_error.c_str());
	game_frame_.Add(server_);
	META_CONPRINTF("[COMBAT_TWEAKS] Combat Tweaks %s loaded%s.\n", COMBAT_TWEAKS_VERSION, late ? " late" : "");
	return true;
}

bool CombatTweaks::Unload(char *, size_t)
{
	game_frame_.Remove(server_);
	barrier::Shutdown();
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *pawn = Pawn(slot);
		if (!pawn) continue;
		schema::Field<std::uint64_t>(pawn, g_damage_flags_offset) &= ~kNoTagging;
		UpdateBody(g_bodies[slot], pawn, false);
	}
	for (const Setting &setting : g_settings)
	{
		ConVarRefAbstract convar(setting.name, true);
		convar.SetFloat(setting.original);
		convar.AddFlags(setting.stripped);
	}
	return true;
}

KHook::Return<void> CombatTweaks::OnGameFrame(IServerGameDLL *, bool simulating, bool, bool)
{
	if (!simulating) return {KHook::Action::Ignore};
	// Each frame rather than on spawn: a new pawn starts without the flag, and this also covers a late load.
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *pawn = Pawn(slot);
		if (!pawn) continue;
		UpdateBody(g_bodies[slot], pawn, schema::Field<std::uint8_t>(pawn, g_life_state_offset) != kLifeAlive);
#ifdef COMBAT_TWEAKS_TRACE
		trace::Frame(slot, pawn);
		if (slot % 2 != 0) continue; // odd slots keep the normal slowdown, as the control group
#endif
		auto &flags = schema::Field<std::uint64_t>(pawn, g_damage_flags_offset);
		if ((flags & kNoTagging) == 0) flags |= kNoTagging;
	}
	barrier::Frame();
#ifndef COMBAT_TWEAKS_TRACE // the trace build leaves the settings alone so a test can compare values
	// Map changes and game mode configs write these back (Valve's deathmatch config sets the decal one).
	const auto now = Clock::now();
	if (now >= next_settings_check_)
	{
		next_settings_check_ = now + std::chrono::seconds(1);
		ApplySettings();
	}
#endif
	return {KHook::Action::Ignore};
}
