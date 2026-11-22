#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace chatterino::ffz::detail {
// Adapt the documented multi-room response to the existing single-room
// parser, never including another channel's emote set or badge metadata.
inline std::optional<QJsonObject> roomFromBatch(const QJsonObject &root, const QString &id)
{
    const auto sets = root.value("sets").toObject();
    for (const auto entry : root.value("rooms").toArray())
    {
        const auto room = entry.toObject();
        if (room.value("twitch_id").toVariant().toString() != id) continue;
        const auto setId = room.value("set").toVariant().toString();
        if (!sets.value(setId).isObject()) return std::nullopt;
        return QJsonObject{{"room", room}, {"sets", QJsonObject{{setId, sets.value(setId)}}}};
    }
    return std::nullopt;
}
}
