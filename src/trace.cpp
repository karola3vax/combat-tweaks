#include "trace.h"

#include "schema.h"

#include <ISmmPlugin.h>
#include <entity2/entityinstance.h>

#include <array>
#include <cstdio>
#include <cstdint>

extern ISmmAPI *g_SMAPI;

namespace trace
{
namespace
{

// The tag lands about 2 ticks after the hit by default, so read the result a little later.
constexpr int kTicksToResult = 8;

std::ptrdiff_t health, armor, velocity, stack, movement, stamina, punch_services, punch_angle;

struct State
{
	int last_health = 0;
	int pending = 0;
	float velocity_at_hit = 0.0f;
	float stamina_at_hit = 0.0f;
};
std::array<State, 64> states;

float Stamina(CEntityInstance *pawn)
{
	void *services = schema::Field<void *>(pawn, movement);
	return services ? schema::Field<float>(services, stamina) : -1.0f;
}

} // namespace

bool Init(CSchemaSystemTypeScope *scope, char *error, std::size_t error_size)
{
	health = schema::FieldOffset(scope, "CBaseEntity", "m_iHealth");
	armor = schema::FieldOffset(scope, "CCSPlayerPawn", "m_ArmorValue");
	velocity = schema::FieldOffset(scope, "CCSPlayerPawn", "m_flVelocityModifier");
	stack = schema::FieldOffset(scope, "CCSPlayerPawn", "m_flFlinchStack");
	movement = schema::FieldOffset(scope, "CBasePlayerPawn", "m_pMovementServices");
	stamina = schema::FieldOffset(scope, "CCSPlayer_MovementServices", "m_flStamina");
	punch_services = schema::FieldOffset(scope, "CCSPlayerPawn", "m_pAimPunchServices");
	punch_angle = schema::FieldOffset(scope, "CCSPlayer_AimPunchServices", "m_unpredictableBaseAngle");
	if (health >= 0 && armor >= 0 && velocity >= 0 && stack >= 0 && movement >= 0 && stamina >= 0 && punch_services >= 0
		&& punch_angle >= 0)
		return true;
	std::snprintf(error, error_size, "Trace: CS2's schema is missing a field");
	return false;
}

void Frame(int slot, CEntityInstance *pawn)
{
	State &state = states[slot];
	const int hp = schema::Field<int>(pawn, health);
	if (hp < state.last_health && hp > 0)
	{
		void *services = schema::Field<void *>(pawn, punch_services);
		const float *angle = services ? &schema::Field<float>(services, punch_angle) : nullptr;
		state.velocity_at_hit = schema::Field<float>(pawn, velocity);
		state.stamina_at_hit = Stamina(pawn);
		state.pending = kTicksToResult;
		META_CONPRINTF("[NHE] hit slot=%d flag=%d hp=%d armor=%d punch=%.3f,%.3f,%.3f\n", slot, slot % 2 == 0, hp,
			schema::Field<int>(pawn, armor), angle ? angle[0] : 0.0f, angle ? angle[1] : 0.0f, angle ? angle[2] : 0.0f);
	}
	state.last_health = hp;
	if (state.pending > 0 && --state.pending == 0)
		META_CONPRINTF("[NHE] after slot=%d flag=%d velocity=%.3f->%.3f stamina=%.2f->%.2f stack=%.3f\n", slot, slot % 2 == 0,
			state.velocity_at_hit, schema::Field<float>(pawn, velocity), state.stamina_at_hit, Stamina(pawn),
			schema::Field<float>(pawn, stack));
}

} // namespace trace
