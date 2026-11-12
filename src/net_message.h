#pragma once

// Finds a net message type by id and checks its name, so an id that the game moves to another
// message gives nothing instead of the wrong message. Since CS2 2000943 a type's name is
// "<proto type> [<id>]", for example "CUserMessageTextMsg [124]". A template, so the test can use a fake.

#include <cstring>

template <class Messages>
auto *FindNetMessage(Messages *messages, int id, const char *type)
{
	auto *found = messages ? messages->FindNetworkMessageById(id) : nullptr;
	const char *name = found ? found->GetUnscopedName() : nullptr;
	const size_t length = std::strlen(type);
	const bool match = name && std::strncmp(name, type, length) == 0 && name[length] == ' ' && name[length + 1] == '[';
	return match ? found : nullptr;
}
