# No Hit Effects

Metamod plugin for CS2 servers. It applies to all players and has no commands. The only config is `anti_rush.ini`.

| Effect | How |
| --- | --- |
| No aimpunch when hit | Removes the dev-only and cheat flags from `mp_flinch_punch_scale` and sets it to 0 |
| No slowdown when hit | Sets `DFLAG_SUPPRESS_PHYSICS_FORCE` (0x2) in each player pawn's `m_nTakeDamageFlags`, so the game's tagging step exits before it slows the pawn or tells the client to |
| Shorter bullet decals | Sets `r_decals_overide_fadestarttime_params` to 0. Decals still appear, then fade over about 3 seconds |
| No ragdolls | Sets a dead pawn's `m_clrRender` alpha to 0 and `m_nRenderMode` to `kRenderNone`, and restores both on respawn |
| Anti-rush barrier | Draws a faint grey `beam` outline around each box in `addons/no_hit_effects/anti_rush.ini`, and players stop at it like a wall, until either team is down to one living player or `unlock_seconds` (default 60) have passed since freeze time ended. Off during warmup |

The fade setting also shortens blood and scorch marks; blood pops out at 3 seconds instead of fading. A server cannot stop bullet holes from appearing, because each player's game paints its own.

These ragdoll methods failed in live tests with `sv_cheats` on or off: `DFLAG_REMOVE_NO_RAGDOLL`, clearing `m_bClientSideRagdoll`, and `EF_NODRAW`.

Settings are checked once a second, because map changes and game mode configs write them back. `meta unload` restores the original values and flags.

Field offsets come from CS2's schema at load, and the plugin refuses to load if a field or convar is gone. The anti-rush barrier also needs three signatures (`CreateEntityByName`, `DispatchSpawn`, `UTIL_Remove`, checked on build 2000942). If a CS2 update moves one, only the barrier turns off, with a console message.

The wall works by cancelling movement into the box through `m_vecAbsVelocity`, and it also stops players in noclip.

The evidence behind each method is in [the research report](../../06-Reports-and-Media/reports/CS2%20hit%20slowdown%20punch%20and%20decals.md).

## Build

```bash
powershell.exe -ExecutionPolicy Bypass -File scripts/build-steamrt3.ps1
```

```bash
powershell.exe -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

Linux builds run in Docker with Valve's SteamRT3 image. The result lands in `dist/addons`; copy that folder into `game/csgo/addons`.

## Test build

`build-steamrt3.ps1 --trace` builds a version for testing only:

- It logs each hit's punch, speed, stamina and flinch stack.
- Odd player slots keep the normal slowdown, as a control group.
- It leaves the settings alone after load, so a test can change them from the console.

Never ship this build.
