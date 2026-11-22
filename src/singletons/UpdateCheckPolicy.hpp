#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <chrono>
#include <optional>
#include <semver/semver.hpp>

namespace chatterino::updatecheck {

constexpr auto firstDelay = std::chrono::seconds(4);
constexpr auto regularDelay = std::chrono::minutes(10);
constexpr auto requestSpacing = std::chrono::seconds(15);
constexpr auto rateLimitDelay = std::chrono::hours(1);

inline std::chrono::milliseconds retryDelay(unsigned failures, bool rateLimited)
{
    if (rateLimited)
        return rateLimitDelay;
    if (failures <= 1)
        return std::chrono::minutes(1);
    if (failures == 2)
        return std::chrono::minutes(2);
    if (failures == 3)
        return std::chrono::minutes(5);
    return regularDelay;
}

inline QString versionOfTag(QString tag)
{
    tag = tag.trimmed();
    if (tag.startsWith(u'v') || tag.startsWith(u'V'))
        tag.remove(0, 1);
    return tag;
}

struct Release {
    QJsonObject metadata;
    QString version;
    bool prerelease;
};

// GitHub's ordering is not semantic-version ordering, and /latest can be
// manually designated. Choose the highest eligible valid published version.
inline std::optional<Release> bestRelease(const QJsonValue &value, bool withTests)
{
    const auto releases = value.isArray() ? value.toArray()
        : value.isObject() ? QJsonArray{value} : QJsonArray{};
    std::optional<Release> best;
    semver::version bestVersion;
    for (const auto &item : releases)
    {
        const auto obj = item.toObject();
        if (obj.value("draft").toBool())
            continue;
        const auto version = versionOfTag(obj.value("tag_name").toString());
        semver::version parsed;
        if (!parsed.from_string_noexcept(version.toStdString()))
            continue;
        const bool prerelease = obj.value("prerelease").toBool() || version.contains(u'-');
        if (prerelease && !withTests)
            continue;
        if (!best || parsed > bestVersion)
        {
            bestVersion = parsed;
            best = Release{obj, version, prerelease};
        }
    }
    return best;
}

}  // namespace chatterino::updatecheck
