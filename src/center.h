#pragma once

// HTML in the middle of one player's screen (the "show_survival_respawn_status" panel).
// The game's event manager is caught the first time the game loads its event list, so after a
// late plugin load this works from the next map on.

#include <string>

namespace center
{

bool Init(const void *server_module_address);
bool Ready();
// Shows the HTML for about a second; call it every tick to keep it up. End the HTML with <br>,
// or the panel hides its last line during live rounds.
void Send(int slot, const std::string &html);
// A sound only that player hears, by sound event name (for example a UI sound).
void PlaySound(int slot, const char *sound_event);
void Shutdown();

} // namespace center
