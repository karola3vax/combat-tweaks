// g++ -std=c++20 -I src tests/net_message_test.cpp -o net_message_test && ./net_message_test

#include "net_message.h"

#include <cassert>
#include <cstdio>

struct Type
{
	const char *name;
	const char *GetName() const { return name; }
};

struct Messages
{
	Type text {"CUserMessageTextMsg [124]"};
	Type other {"CMsgGCCStrike15_v2_GC2ClientTextMsg [124]"};
	Type nameless {nullptr};
	Type *FindNetworkMessageById(int id)
	{
		return id == 124 ? &text : id == 1 ? &other : id == 2 ? &nameless : nullptr;
	}
};

int main()
{
	Messages messages;
	assert(FindNetMessage(&messages, 124, "CUserMessageTextMsg") == &messages.text);
	assert(!FindNetMessage(&messages, 124, "CUserMessageText"));     // a prefix of the type is not the type
	assert(!FindNetMessage(&messages, 124, "TextMsg"));
	assert(!FindNetMessage(&messages, 1, "CUserMessageTextMsg"));    // the id now names another message
	assert(!FindNetMessage(&messages, 2, "CUserMessageTextMsg"));
	assert(!FindNetMessage(&messages, 3, "CUserMessageTextMsg"));
	assert(!FindNetMessage(static_cast<Messages *>(nullptr), 124, "CUserMessageTextMsg"));

	messages.text.name = "CUserMessageTextMsg";                       // the name from before 2000943
	assert(!FindNetMessage(&messages, 124, "CUserMessageTextMsg"));
	std::puts("net_message_test passed");
	return 0;
}
