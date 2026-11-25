#include "providers/twitch/ModeratorModule.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <openssl/evp.h>
#include <cstdlib>

#ifdef CHATTERINO_HAVE_PLUGINS
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif

namespace chatterino::moderator {
namespace {
constexpr int MAX_ENVELOPE = 96 * 1024;
constexpr int MAX_SOURCE = 16 * 1024;
constexpr qint64 MAX_AGE = 91LL * 24 * 60 * 60;

bool verifySignature(const QByteArray &key, const QByteArray &data,
                     const QByteArray &signature)
{
    if (key.size() != 32 || signature.size() != 64)
        return false;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                 reinterpret_cast<const unsigned char *>(key.constData()), key.size()),
             EVP_PKEY_free);
    Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return pkey && ctx &&
        EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) == 1 &&
        EVP_DigestVerify(ctx.get(),
            reinterpret_cast<const unsigned char *>(signature.constData()), signature.size(),
            reinterpret_cast<const unsigned char *>(data.constData()), data.size()) == 1;
}
}

QByteArray ownerPublicKey()
{
    return QByteArray::fromBase64("3GB9OcyrH3zdcuR5xZAkeoyhQgO1AhhHpsJmWnU8owM=");
}

bool validUserId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("\\A[1-9][0-9]{0,19}\\z"));
    return re.match(id).hasMatch();
}

bool validChannel(const QString &login)
{
    static const QRegularExpression re(QStringLiteral("\\A[a-z0-9_]{1,25}\\z"));
    return re.match(login).hasMatch();
}

bool validReply(const QString &text)
{
    static const QRegularExpression controls(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    const auto trimmed = text.trimmed();
    return !trimmed.isEmpty() && trimmed.size() <= 500 &&
        !trimmed.startsWith(u'/') && !trimmed.startsWith(u'.') &&
        !trimmed.contains(controls);
}

std::optional<Policy> verifyPolicy(const QByteArray &envelope,
                                   const QByteArray &publicKey, const QDateTime &now)
{
    if (envelope.isEmpty() || envelope.size() > MAX_ENVELOPE)
        return {};
    const auto outer = QJsonDocument::fromJson(envelope).object();
    const auto data = QByteArray::fromBase64(outer.value("payload").toString().toLatin1(),
                                            QByteArray::AbortOnBase64DecodingErrors);
    const auto signature = QByteArray::fromBase64(outer.value("signature").toString().toLatin1(),
                                                 QByteArray::AbortOnBase64DecodingErrors);
    if (data.isEmpty() || !verifySignature(publicKey, data, signature))
        return {};
    const auto payload = QJsonDocument::fromJson(data).object();
    const auto issued = QDateTime::fromString(payload.value("issued_at").toString(), Qt::ISODate);
    const auto expires = QDateTime::fromString(payload.value("expires_at").toString(), Qt::ISODate);
    const auto revision = payload.value("revision").toInteger(0);
    if (payload.value("schema").toInt() != 1 || revision <= 0 ||
        !issued.isValid() || !expires.isValid() || issued > now.addSecs(300) ||
        expires <= now || issued.secsTo(expires) <= 0 ||
        issued.secsTo(expires) > MAX_AGE)
        return {};
    Policy p;
    p.revision = revision;
    p.expires = expires;
    p.version = payload.value("module_version").toString();
    p.minimumCooldown = payload.value("min_cooldown_seconds").toInt();
    p.maximumRules = payload.value("max_rules").toInt();
    p.source = payload.value("engine").toString().toUtf8();
    p.digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    static const QRegularExpression versionRe(QStringLiteral("\\A[0-9]{1,4}\\.[0-9]{1,4}\\.[0-9]{1,4}\\z"));
    if (!versionRe.match(p.version).hasMatch() ||
        p.minimumCooldown < 3 || p.minimumCooldown > 300 ||
        p.maximumRules < 1 || p.maximumRules > 200 ||
        p.source.isEmpty() || p.source.size() > MAX_SOURCE ||
        !payload.value("allowed_twitch_ids").isArray())
        return {};
    const auto ids = payload.value("allowed_twitch_ids").toArray();
    if (ids.size() > 500)
        return {};
    for (const auto &id : ids)
    {
        if (!id.isString() || !validUserId(id.toString()))
            return {};
        p.allowed.insert(id.toString());
    }
    return p;
}

bool permitted(const Policy *policy, const QString &account, const QString &roleAccount,
               const QSet<QString> &moderated, const QString &channel,
               bool fresh, bool currentModerator)
{
    return policy && fresh && policy->expires > QDateTime::currentDateTimeUtc() &&
        validUserId(account) && account == roleAccount &&
        policy->allowed.contains(account) && currentModerator &&
        validChannel(channel) && moderated.contains(channel);
}

struct Matcher::Impl {
#ifdef CHATTERINO_HAVE_PLUGINS
    lua_State *state = nullptr;
    size_t allocated = 0;
    int function = LUA_NOREF;
    int remaining = 0;
    struct Input {
        const QByteArray *text;
        const std::vector<std::pair<std::vector<QByteArray>, bool>> *rules;
        int function;
    };

    static void *allocator(void *ud, void *ptr, size_t oldSize, size_t newSize)
    {
        auto *self = static_cast<Impl *>(ud);
        const size_t previous = ptr ? oldSize : 0;
        if (!newSize)
        {
            std::free(ptr);
            self->allocated -= previous;
            return nullptr;
        }
        if (newSize > 2 * 1024 * 1024 ||
            self->allocated - previous > 2 * 1024 * 1024 - newSize)
            return nullptr;
        void *result = std::realloc(ptr, newSize);
        if (result)
            self->allocated = self->allocated - previous + newSize;
        return result;
    }
    static void hook(lua_State *L, lua_Debug *)
    {
        auto *self = *static_cast<Impl **>(lua_getextraspace(L));
        if (--self->remaining <= 0)
            luaL_error(L, "Moderator module budget exceeded");
    }
    static int call(lua_State *L)
    {
        const auto *input = static_cast<const Input *>(lua_touserdata(L, 1));
        lua_rawgeti(L, LUA_REGISTRYINDEX, input->function);
        lua_pushlstring(L, input->text->constData(), input->text->size());
        lua_createtable(L, static_cast<int>(input->rules->size()), 0);
        for (size_t i = 0; i < input->rules->size(); ++i)
        {
            const auto &rule = (*input->rules)[i];
            lua_createtable(L, 0, 2);
            lua_pushboolean(L, rule.second);
            lua_setfield(L, -2, "exact");
            lua_createtable(L, static_cast<int>(rule.first.size()), 0);
            for (size_t j = 0; j < rule.first.size(); ++j)
            {
                const auto &trigger = rule.first[j];
                lua_pushlstring(L, trigger.constData(), trigger.size());
                lua_rawseti(L, -2, j + 1);
            }
            lua_setfield(L, -2, "triggers");
            lua_rawseti(L, -2, i + 1);
        }
        lua_call(L, 2, 1);
        return 1;
    }
    ~Impl()
    {
        if (state)
            lua_close(state);
    }
#endif
};

Matcher::Matcher(const QByteArray &source)
    : impl_(std::make_unique<Impl>())
{
#ifdef CHATTERINO_HAVE_PLUGINS
    auto &i = *impl_;
    if (source.isEmpty() || source.size() > MAX_SOURCE)
        return;
    i.state = lua_newstate(Impl::allocator, &i);
    if (!i.state)
        return;
    auto *L = i.state;
    *static_cast<Impl **>(lua_getextraspace(L)) = &i;
    // No base, OS, IO, package, debug, coroutine, dynamic loading, host APIs.
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    lua_sethook(L, Impl::hook, LUA_MASKCOUNT, 1000);
    i.remaining = 100;
    if (luaL_loadbufferx(L, source.constData(), source.size(), "moderator-module", "t") == LUA_OK &&
        lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isfunction(L, -1))
        i.function = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, 0);
#else
    (void)source;
#endif
}

Matcher::~Matcher() = default;
bool Matcher::valid() const
{
#ifdef CHATTERINO_HAVE_PLUGINS
    return impl_->state && impl_->function != LUA_NOREF;
#else
    return false;
#endif
}

std::optional<size_t> Matcher::match(const QString &text, const std::vector<MatchRule> &rules)
{
#ifdef CHATTERINO_HAVE_PLUGINS
    if (!valid() || text.size() > 500 || rules.empty() || rules.size() > 200)
        return {};
    auto &i = *impl_;
    const QByteArray inputText = text.simplified().toCaseFolded().toUtf8();
    std::vector<std::pair<std::vector<QByteArray>, bool>> normalized;
    normalized.reserve(rules.size());
    for (const auto &rule : rules)
    {
        auto &entry = normalized.emplace_back();
        entry.second = rule.exact;
        for (const auto &phrase : rule.triggers)
            entry.first.push_back(phrase.simplified().toCaseFolded().toUtf8());
    }
    const Impl::Input input{&inputText, &normalized, i.function};
    auto *L = i.state;
    i.remaining = 100;
    lua_pushcfunction(L, Impl::call);
    lua_pushlightuserdata(L, const_cast<Impl::Input *>(&input));
    std::optional<size_t> result;
    if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isinteger(L, -1))
    {
        const auto index = lua_tointeger(L, -1);
        if (index > 0 && static_cast<size_t>(index) <= rules.size())
            result = static_cast<size_t>(index - 1);
    }
    lua_settop(L, 0);
    return result;
#else
    (void)text;
    (void)rules;
    return {};
#endif
}
}  // namespace chatterino::moderator
