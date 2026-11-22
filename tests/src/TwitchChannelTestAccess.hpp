#pragma once

#include "providers/twitch/TwitchChannel.hpp"

namespace chatterino {

class TwitchChannelTestAccess
{
public:
    static void setRoomId(TwitchChannel &channel, const QString &roomId)
    {
        channel.setRoomId(roomId);
    }
};

}  // namespace chatterino
