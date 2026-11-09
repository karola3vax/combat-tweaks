#pragma once

// A network message recipient list with one player.

#include <irecipientfilter.h>

class OneRecipient final : public IRecipientFilter
{
public:
	explicit OneRecipient(int slot) { recipients_.Set(slot); }
	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec &GetRecipients() const override { return recipients_; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return CPlayerSlot(-1); }

private:
	CPlayerBitVec recipients_; // starts empty
};
