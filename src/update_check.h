#pragma once

// Asks GitHub for the latest Combat Tweaks release at load and every 6 hours, and prints one console
// line per newer version. Silent on every failure. Game thread only; nothing here blocks.

namespace update_check
{

void Enabled(bool enabled); // false cancels a running request and stops further checks
void Start(const char *version); // the running version, for example COMBAT_TWEAKS_VERSION
void Frame();                    // call about once a second from the game thread
void Stop();                     // call on unload: cancels and releases the request

} // namespace update_check
