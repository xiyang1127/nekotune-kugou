#include "plugin.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace nekotune;

namespace kugou {
class Music final : public sdk::IMusicProvider {
  public:
    explicit Music(Extension &extension) : m_extension(extension) {}
    void search(const QString &query, const QString &cursor, std::function<void(Result<sdk::SearchPage>)> done) override {
        m_extension.m_client.search(query, cursor.isEmpty() ? 1 : cursor.toInt(), [this, done](Result<QVariantMap> result) {
            if (!result)
                return done(result.error());
            m_extension.remember(result.value());
            sdk::SearchPage page;
            for (const auto &value : result.value().value("tracks").toList()) {
                const auto track = value.toMap();
                page.tracks.append({track.value("id").toString(), track.value("title").toString(), track.value("artist").toString(),
                                    track.value("album").toString(), track.value("cover_url").toString(),
                                    track.value("duration_ms").toLongLong()});
            }
            page.cursor = result.value().value("cursor").toString();
            done(page);
        });
    }
    void track(const QString &id, std::function<void(Result<sdk::Track>)> done) override {
        const auto found = m_extension.track(id);
        if (!found)
            return done(found.error());
        const auto &t = found.value();
        done(sdk::Track{t.value("id").toString(), t.value("title").toString(), t.value("artist").toString(),
                        t.value("album").toString(), t.value("cover_url").toString(), t.value("duration_ms").toLongLong()});
    }
    void resolve(const QString &id, const QString &, std::function<void(Result<sdk::ResolvedAudio>)> done) override {
        const auto found = m_extension.track(id);
        if (!found)
            return done(found.error());
        m_extension.m_client.resolve(found.value(), [done](Result<QVariantMap> result) {
            if (!result)
                return done(result.error());
            const auto &map = result.value();
            sdk::ResolvedAudio audio;
            audio.url = QUrl(map.value("url").toString());
            const auto headers = map.value("headers").toMap();
            for (auto it = headers.cbegin(); it != headers.cend(); ++it)
                audio.headers.insert(it.key(), it.value().toString());
            audio.expiresAt = map.value("expiresAt").toLongLong();
            audio.extension = map.value("extension").toString();
            audio.maxBytes = map.value("maxBytes").toLongLong();
            audio.allowedHosts = map.value("allowedHosts").toStringList();
            audio.contentTypes = map.value("contentTypes").toStringList();
            done(audio);
        });
    }
    void downloaded(const QString &id, const QString &path, int, bool existing, sdk::Reply done) override {
        const auto found = m_extension.track(id);
        if (!found)
            return done(found.error());
        saveAssets(m_extension.m_client, found.value(), path, existing, done);
    }

  private:
    Extension &m_extension;
};

class Lyrics final : public sdk::ILyricsProvider {
  public:
    explicit Lyrics(Extension &extension) : m_extension(extension) {}
    void search(const sdk::Params &query, sdk::Reply done) override {
        m_extension.m_client.search(query.value("title").toString(), 1, [this, done](Result<QVariantMap> result) {
            if (!result)
                return done(result.error());
            m_extension.remember(result.value());
            QVariantList candidates;
            for (const auto &value : result.value().value("tracks").toList()) {
                auto item = value.toMap();
                item.insert("song_result", true);
                item.insert("score", 0);
                candidates.append(item);
            }
            done(sdk::Params{{"candidates", candidates}});
        });
    }
    void resolve(const sdk::Params &candidate, sdk::Reply done) override {
        if (candidate.value("song_result").toBool()) {
            lyricCandidates(m_extension.m_client, candidate, [done](Result<QVariantList> candidates) {
                done(candidates ? Result<sdk::Params>(sdk::Params{{"candidates", candidates.value()}})
                                : Result<sdk::Params>(candidates.error()));
            });
            return;
        }
        lyricDocument(m_extension.m_client, candidate, false, [done](Result<Document> document) {
            done(document ? Result<sdk::Params>(document.value().value) : Result<sdk::Params>(document.error()));
        });
    }

  private:
    Extension &m_extension;
};

Extension::Extension(sdk::IExtensionContext &context, ext::HttpTransport &transport)
    : m_context(context), m_client(context, transport), m_music(std::make_unique<Music>(*this)),
      m_lyrics(std::make_unique<Lyrics>(*this)) {}
Extension::~Extension() { close(); }

void Extension::close() {
    if (m_closed)
        return;
    m_closed = true;
    m_client.cancel();
    m_sources.clear();
    m_registrations.clear();
}
QVariantMap Extension::status() const {
    auto result = m_client.status();
    result.insert("busy", m_busy);
    result.insert("download_active", !m_activeTask.isEmpty());
    return result;
}
void Extension::emitEvent(const QString &name, const QVariantMap &data) {
    if (!m_closed)
        m_context.publish(name, data);
}
void Extension::notify() { emitEvent("status", status()); }
void Extension::remember(const QVariantMap &result) {
    for (const auto &value : result.value("tracks").toList()) {
        const auto track = value.toMap();
        const auto id = track.value("id").toString();
        if (!m_tracks.contains(id))
            m_trackOrder.append(id);
        m_tracks.insert(id, track);
    }
    while (m_trackOrder.size() > 1000)
        m_tracks.remove(m_trackOrder.takeFirst());
    QVariantMap stored;
    for (const auto &id : std::as_const(m_trackOrder))
        stored.insert(id, m_tracks.value(id));
    m_context.storageSet("tracks", stored, [](Result<void>) {});
}
Result<QVariantMap> Extension::track(const QString &id) const {
    if (!validHash(id))
        return failure("Invalid Kugou track ID");
    return m_tracks.value(id, QVariantMap{{"id", id}, {"title", id}});
}
Result<void> Extension::configureSources() {
    if (m_closed)
        return {};
    const auto config = m_context.config();
    const auto signature = QJsonDocument::fromVariant(QVariantList{config.value("enabled"), config.value("worker_url")})
                               .toJson(QJsonDocument::Compact);
    if (m_sourcesKnown && signature == m_sourceSignature)
        return {};
    m_sources.clear();
    m_sourcesKnown = false;
    if (!m_client.configured()) {
        m_sourceSignature = signature;
        m_sourcesKnown = true;
        return {};
    }
    if (auto origin = workerOrigin(config.value("worker_url").toString()); !origin)
        return origin.error();
    m_sources.push_back(m_context.registerMusicProvider("music", m_music.get(),
                                                        {{"name", "酷狗 / Kugou"}, {"download", true}, {"page", "music"}}));
    m_sources.push_back(m_context.registerLyricsProvider("lyrics", m_lyrics.get(), {{"name", "酷狗 / Kugou"}}));
    m_sourceSignature = signature;
    m_sourcesKnown = true;
    return {};
}
void Extension::operation(const QString &name, std::function<void(Reply)> action, sdk::Reply reply) {
    if (m_busy)
        return reply(failure("Kugou operation already in progress"));
    m_busy = true;
    notify();
    reply(sdk::Params{{"started", true}});
    action([this, name](Result<QVariantMap> result) {
        if (result)
            emitEvent(name, result.value());
        else
            emitEvent("operation_failed", {{"message", result.error().message}});
        m_busy = false;
        notify();
    });
}
void Extension::service(const QString &id, sdk::Handler handler) {
    m_registrations.push_back(m_context.registerService(id, std::move(handler)));
}

void Extension::activate(std::function<void(Result<void>)> done) {
    auto start = [this, done] {
        m_client.initialize([this, done] {
            m_context.storageGet("tracks", [this, done](Result<QVariant> stored) {
                if (stored) {
                    const auto map = stored.value().toMap();
                    for (auto it = map.cbegin(); it != map.cend(); ++it) {
                        m_trackOrder.append(it.key());
                        m_tracks.insert(it.key(), it.value().toMap());
                    }
                }
                registerServices();
                m_registrations.push_back(m_context.on("extension.config_changed", [this](const sdk::Params &) {
                    if (auto configured = configureSources(); !configured)
                        emitEvent("operation_failed", {{"message", configured.error().message}});
                    emitEvent("config_changed");
                    notify();
                }));
                m_registrations.push_back(m_context.on("music.download", [this](const sdk::Params &event) {
                    if (event.value("task").toString() != m_activeTask || m_activeTask.isEmpty())
                        return;
                    const auto state = event.value("state").toString();
                    if (state == "running")
                        return emitEvent("download_progress", event);
                    m_activeTask.clear();
                    emitEvent(state == "finished" ? "download_finished" : state == "cancelled" ? "download_cancelled" : "operation_failed",
                              event);
                    notify();
                }));
                done(configureSources());
            });
        });
    };
    if (m_context.config().value("legacy_imported").toBool())
        return start();
    m_context.hostCall("player.status", {}, [this, start, done](Result<sdk::Params> host) {
        if (!host)
            return done(host.error());
        QVariantMap legacy;
        QFile file(QDir(host.value().value("config_directory").toString()).filePath("settings.json"));
        if (file.open(QIODevice::ReadOnly))
            legacy = QJsonDocument::fromJson(file.readAll()).object().value("kugou").toObject().toVariantMap();
        else if (file.exists())
            m_context.log(QtWarningMsg, "Legacy configuration unavailable; configure Kugou in plugin settings");
        const auto origin = workerOrigin(legacy.value("worker_url").toString(), false);
        const auto url = origin ? origin.value() : QString();
        QVariantMap config{{"enabled", legacy.value("enabled").toBool() && !url.isEmpty()}, {"worker_url", url}};
        const auto current = m_context.config();
        for (auto it = current.cbegin(); it != current.cend(); ++it)
            config.insert(it.key(), it.value());
        config.insert("legacy_imported", true);
        m_context.updateConfig(config, [start, done](Result<void> saved) {
            if (!saved)
                return done(saved);
            start();
        });
    });
}

void Extension::registerServices() {
    service("status", [this](const sdk::Params &, sdk::Reply reply) { reply(status()); });
    service("config.set", [this](const sdk::Params &params, sdk::Reply reply) {
        const auto enabled = params.value("enabled"), worker = params.value("worker_url");
        if (enabled.typeId() != QMetaType::Bool || worker.typeId() != QMetaType::QString)
            return reply(failure("Invalid Kugou configuration"));
        if (m_busy || !m_activeTask.isEmpty())
            return reply(failure("Kugou operation already in progress"));
        const auto origin = workerOrigin(worker.toString(), enabled.toBool());
        if (!origin)
            return reply(origin.error());
        auto config = m_context.config();
        config.insert("enabled", enabled.toBool());
        config.insert("worker_url", origin.value());
        m_context.updateConfig(config, [this, reply](Result<void> saved) {
            if (!saved)
                return reply(saved.error());
            if (auto configured = configureSources(); !configured)
                return reply(configured.error());
            emitEvent("config_changed");
            notify();
            reply(status());
        });
    });
    service("save_key", [this](const sdk::Params &params, sdk::Reply reply) {
        if (m_busy)
            return reply(failure("Kugou operation already in progress"));
        m_client.saveKey(params.value("key").toString(), [this, reply](Result<void> saved) {
            if (!saved)
                return reply(saved.error());
            notify();
            reply(status());
        });
    });
    service("clear_key", [this](const sdk::Params &, sdk::Reply reply) {
        if (m_busy)
            return reply(failure("Kugou operation already in progress"));
        m_client.clearKey([this, reply](Result<void> cleared) {
            if (!cleared)
                return reply(cleared.error());
            notify();
            reply(status());
        });
    });
    static const QRegularExpression mobilePattern("^1[3-9]\\d{9}$");
    static const QRegularExpression codePattern("^\\d{4,8}$");
    service("send_code", [this](const sdk::Params &params, sdk::Reply reply) {
        const auto mobile = params.value("mobile").toString();
        if (!mobilePattern.match(mobile).hasMatch())
            return reply(failure("Invalid mainland China mobile number"));
        operation("code_sent", [this, mobile](Reply done) {
            auto sendCode = [this, mobile, done] {
                m_client.account("/captcha/sent", {{"mobile", mobile}}, [done](Result<QJsonObject> sent) {
                    if (!sent)
                        return done(sent.error());
                    if (!businessOk(sent.value()))
                        return done(failure("Kugou rejected SMS request"));
                    done(QVariantMap{});
                });
            };
            const auto dfid = m_client.cookies().value("dfid").toString();
            if (!dfid.isEmpty() && dfid != "0" && dfid != "-" && dfid != "undefined")
                return sendCode();
            m_client.account("/register/dev", {}, [this, sendCode, done](Result<QJsonObject> registered) {
                if (!registered)
                    return done(registered.error());
                if (!businessOk(registered.value()) || m_client.cookies().value("dfid").toString().isEmpty())
                    return done(failure("Kugou device registration failed"));
                sendCode();
            });
        }, reply);
    });
    service("login", [this](const sdk::Params &params, sdk::Reply reply) {
        const auto mobile = params.value("mobile").toString(), code = params.value("code").toString();
        if (!mobilePattern.match(mobile).hasMatch() || !codePattern.match(code).hasMatch())
            return reply(failure("Invalid mobile number or verification code"));
        operation("logged_in", [this, mobile, code](Reply done) {
            m_client.account("/login/cellphone", {{"mobile", mobile}, {"code", code}}, [this, done](Result<QJsonObject> result) {
                if (!result)
                    return done(result.error());
                const auto data = result.value().value("data").toObject();
                const auto cookies = m_client.cookies();
                if (!businessOk(result.value()) || !m_client.loggedIn() ||
                    cookies.value("token").toString() != data.value("token").toString() ||
                    cookies.value("userid").toString() != data.value("userid").toVariant().toString())
                    return done(failure("Kugou login failed or needs extra verification"));
                done(QVariantMap{});
            });
        }, reply);
    });
    service("search", [this](const sdk::Params &params, sdk::Reply reply) {
        const auto keywords = params.value("keywords").toString();
        const int page = params.value("page", 1).toInt();
        operation("search_results", [this, keywords, page](Reply done) {
            m_client.search(keywords, page, [this, page, done](Result<QVariantMap> result) {
                if (!result)
                    return done(result.error());
                remember(result.value());
                QVariantList songs;
                for (const auto &value : result.value().value("tracks").toList()) {
                    auto item = value.toMap();
                    item.insert("hash", item.value("id"));
                    songs.append(item);
                }
                done(QVariantMap{{"songs", songs}, {"page", page}});
            });
        }, reply);
    });
    service("download", [this](const sdk::Params &params, sdk::Reply reply) {
        if (!m_activeTask.isEmpty())
            return reply(failure("A Kugou download is already in progress"));
        const auto item = track(params.value("hash").toString());
        if (!item)
            return reply(item.error());
        m_context.hostCall("music.download", {{"source", m_context.id() + "/music"}, {"track", item.value()}},
                           [this, reply](Result<sdk::Params> result) {
                               if (!result)
                                   return reply(result);
                               m_activeTask = result.value().value("task").toString();
                               notify();
                               reply(result);
                           });
    });
    service("cancel", [this](const sdk::Params &, sdk::Reply reply) {
        if (m_activeTask.isEmpty())
            return reply(failure("No active Kugou download"));
        m_context.hostCall("music.cancel", {{"task", m_activeTask}}, reply);
    });
    service("play", [this](const sdk::Params &params, sdk::Reply reply) {
        const auto item = track(params.value("hash").toString());
        if (!item)
            return reply(item.error());
        m_context.hostCall("music.enqueue", {{"source", m_context.id() + "/music"}, {"track", item.value()}, {"play", true}}, reply);
    });
}
} // namespace kugou
