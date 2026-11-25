#pragma once

#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"

namespace chatterino {

class TwitchChannelTestAccess
{
public:
    static void setRoomId(TwitchChannel &channel, const QString &roomId)
    {
        channel.setRoomId(roomId);
    }
    static void setMod(TwitchChannel &channel, bool value)
    {
        channel.setMod(value);
    }
    static void setLiveStream(TwitchChannel &channel, const HelixStream &stream)
    {
        // Fixture bypasses onLive notifications, but exercises the same tracking entry point.
        channel.setLive(true);
        channel.updateStreamStatus(stream, true);
    }
};

}  // namespace chatterino
