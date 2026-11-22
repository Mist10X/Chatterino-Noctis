#pragma once

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <pajlada/signals/signal.hpp>
#include <QTimer>
#include <array>

namespace chatterino::servicehealth {

enum Service { SevenTV, FFZ, History, Count };

struct Entry {
    QUrl url;
    quint64 generation = 0;
    bool unavailable = false;
    bool checking = false;
};

inline std::array<Entry, Count> entries;
inline pajlada::Signals::Signal<> changed;

inline bool enabled(Service service)
{
    if (service == SevenTV) return getSettings()->enableSevenTVChannelEmotes;
    if (service == FFZ) return getSettings()->enableFFZChannelEmotes;
    return getSettings()->loadTwitchMessageHistoryOnConnect;
}

inline void clear(Service service)
{
    auto &entry = entries[service];
    if (entry.url.isEmpty()) return;
    const bool notify = entry.unavailable;
    entry = Entry{{}, entry.generation + 1};
    if (notify) changed.invoke();
}

inline void success(Service service)
{
    postToThread([service] { clear(service); });
}

inline bool relevant(const NetworkResult &result)
{
    const int status = result.status().value_or(0);
    return status == 408 || status >= 500 ||
           (status == 0 && result.error() != QNetworkReply::NoError &&
            result.error() != QNetworkReply::OperationCanceledError);
}

inline void probe(Service service);

inline void schedule(Service service, int delay)
{
    const auto generation = ++entries[service].generation;
    QTimer::singleShot(delay, QCoreApplication::instance(), [service, generation] {
        if (generation == entries[service].generation && !isAppAboutToQuit())
            probe(service);
    });
}

inline void probe(Service service)
{
    auto &entry = entries[service];
    if (!enabled(service)) { clear(service); return; }
    if (entry.url.isEmpty() || entry.checking || isAppAboutToQuit()) return;
    entry.checking = true;
    const auto generation = ++entry.generation;
    const auto finish = [service, generation](bool failed) {
        auto &state = entries[service];
        if (generation != state.generation || isAppAboutToQuit()) return;
        state.checking = false;
        if (!failed || !enabled(service)) { clear(service); return; }
        if (!state.unavailable)
        {
            state.unavailable = true;
            changed.invoke();
        }
        schedule(service, 60000);
    };
    NetworkRequest(entry.url).caller(QCoreApplication::instance()).timeout(10000)
        .onSuccess([service, finish](const NetworkResult &result) {
            const auto json = result.parseJson();
            const bool valid = service == SevenTV ? json.contains("id") || json.contains("emote_set")
                             : service == FFZ ? json.value("rooms").isArray()
                                              : json.value("messages").isArray() || json.contains("error_code");
            finish(!valid);
        })
        .onError([finish](const NetworkResult &result) { finish(relevant(result)); })
        .execute();
}

// Parallel startup failures share a single confirmation request per service.
inline void failed(Service service, QUrl url, const NetworkResult &result)
{
    if (!relevant(result)) return;
    postToThread([service, url = std::move(url)] {
        auto &entry = entries[service];
        if (!entry.url.isEmpty() || !enabled(service) || isAppAboutToQuit()) return;
        entry.url = url;
        schedule(service, 4000);
    });
}

}  // namespace chatterino::servicehealth
