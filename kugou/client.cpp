#include "client.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringDecoder>
#include <zlib.h>

using namespace nekotune;

namespace kugou {
namespace {
const QString prefix = "/api/music/kugou/v1";
QString clean(const QJsonValue &value) {
    static const QRegularExpression tags("<[^>]*>");
    QString text = value.isString() ? value.toString() : value.isDouble() ? QString::number(value.toDouble()) : QString();
    return text.remove(tags).trimmed().left(256);
}
QString text(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 17);
    return {};
}
QJsonValue first(const QJsonValue &a, const QJsonValue &b) { return a.isUndefined() || a.isNull() ? b : a; }
} // namespace

bool validHash(const QString &id) {
    static const QRegularExpression pattern("^[a-f\\d]{32}$", QRegularExpression::CaseInsensitiveOption);
    return pattern.match(id).hasMatch();
}
Result<QString> workerOrigin(const QString &value, bool enabled) {
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty() && !enabled)
        return QString();
    QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
        return failure("Set a valid Kugou Worker HTTPS origin");
    if (url.host() == "kugou-lyrics-api.lyuy.workers.dev")
        url.setHost("luy-music-api.lyuy.workers.dev");
    const auto host = url.host();
    const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "::1";
    const auto path = url.path();
    if ((url.scheme() != "https" && !(url.scheme() == "http" && loopback)) || !url.userName().isEmpty() ||
        !url.password().isEmpty() || url.hasQuery() || url.hasFragment() || (!path.isEmpty() && path != "/"))
        return failure("Invalid Worker URL: use an HTTPS origin without a path");
    return url.adjusted(QUrl::RemovePath).toString(QUrl::FullyEncoded);
}
Result<QString> audioUrl(const QString &value) {
    QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return failure("Kugou did not provide an audio URL");
    if (url.scheme() != "https" || !url.host().endsWith(".kugou.com") || !url.userName().isEmpty() ||
        !url.password().isEmpty() || (url.port() != -1 && url.port() != 443))
        return failure("Kugou did not provide a trusted audio URL");
    return url.toString(QUrl::FullyEncoded);
}
QString coverUrl(const QString &value) {
    QUrl url(QString(value).replace("{size}", "240"), QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host() != "imge.kugou.com" ||
        !url.userName().isEmpty() || !url.password().isEmpty() || (url.port() != -1 && url.port() != 80 && url.port() != 443))
        return {};
    url.setScheme("https");
    url.setPort(-1);
    url.setFragment({});
    return url.toString(QUrl::FullyEncoded);
}
bool businessOk(const QJsonObject &body, const QStringList &expected) {
    if (!expected.contains(text(body.value("status"))))
        return false;
    for (const auto *key : {"error_code", "errcode"})
        if (body.contains(key) && !body.value(key).isUndefined() && text(body.value(key)) != "0")
            return false;
    return true;
}
Result<QVariantList> parseTracks(const QJsonObject &body) {
    const auto lists = body.value("data").toObject().value("lists");
    if (!businessOk(body) || !lists.isArray())
        return failure("Invalid Kugou search response");
    QSet<QString> seen;
    QVariantList tracks;
    static const QRegularExpression audioPattern("^\\d{1,15}$");
    for (const auto &parentValue : lists.toArray()) {
        const auto parent = parentValue.toObject();
        QList<QJsonObject> rows{parent};
        for (const auto &child : parent.value("Grp").toArray())
            rows.append(child.toObject());
        for (const auto &row : rows) {
            const auto id = text(first(row.value("FileHash"), row.value("hash"))).toLower();
            if (!validHash(id) || seen.contains(id))
                continue;
            seen.insert(id);
            const auto audioId = text(first(row.value("MixSongID"), parent.value("MixSongID")));
            const auto title = clean(first(row.value("SongName"), parent.value("SongName")));
            const double seconds = first(row.value("Duration"), parent.value("Duration")).toVariant().toDouble();
            const auto image = row.value("Image").toString().isEmpty() ? parent.value("Image").toString() : row.value("Image").toString();
            tracks.append(QVariantMap{{"id", id},
                                      {"title", title.isEmpty() ? id : title},
                                      {"artist", clean(first(row.value("SingerName"), parent.value("SingerName")))},
                                      {"album", clean(first(row.value("AlbumName"), parent.value("AlbumName")))},
                                      {"duration_ms", qMax<qint64>(0, qRound64(seconds * 1000))},
                                      {"cover_url", coverUrl(image)},
                                      {"audio_id", audioPattern.match(audioId).hasMatch() ? audioId : QString()}});
            if (tracks.size() >= 100)
                return tracks;
        }
    }
    return tracks;
}
Result<Krc> decodeKrc(const QJsonObject &body) {
    static const QRegularExpression base64("^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$");
    const auto content = body.value("content").toString();
    if (body.value("status").toVariant().toInt() != 200 || !body.value("content").isString() ||
        content.size() > 4 * 1024 * 1024 || !base64.match(content).hasMatch())
        return failure("Invalid KRC response");
    const auto binary = QByteArray::fromBase64(content.toLatin1());
    if (!binary.startsWith("krc1"))
        return failure("Invalid KRC header");
    static const uchar key[] = {0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47, 0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69};
    QByteArray compressed = binary.mid(4);
    for (qsizetype i = 0; i < compressed.size(); ++i)
        compressed[i] = char(uchar(compressed[i]) ^ key[i % 16]);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        return failure("Invalid KRC data");
    QByteArray output;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    stream.next_in = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_in = uInt(compressed.size());
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = uInt(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        output.append(chunk.constData(), chunk.size() - stream.avail_out);
        if (output.size() > 16 * 1024 * 1024)
            status = Z_DATA_ERROR;
        if (status == Z_BUF_ERROR && stream.avail_in == 0)
            break;
    }
    const auto consumed = stream.total_in;
    inflateEnd(&stream);
    if (status != Z_STREAM_END)
        return failure("Invalid KRC data");
    if (qsizetype(consumed) != compressed.size())
        return failure("Trailing KRC data");
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(output);
    if (decoder.hasError())
        return failure("Invalid KRC encoding");
    static const QRegularExpression timed("^\\[\\d+,\\d+\\].*<\\d+,\\d+,\\d+>", QRegularExpression::MultilineOption);
    if (!timed.match(decoded).hasMatch())
        return failure("KRC contains no timed words");
    return Krc{binary, decoded};
}
Result<QString> validateKey(const QString &value) {
    static const QRegularExpression control("[\\x00-\\x1f\\x7f]");
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty() || value.toUtf8().size() > 4096 || control.match(trimmed).hasMatch())
        return failure("Kugou account key must be 1 to 4096 printable bytes");
    return trimmed;
}
Result<QString> externalKey() {
    const auto value = qEnvironmentVariable("KUGOU_ACCOUNT_API_KEY").trimmed();
    if (!value.isEmpty())
        return validateKey(value);
    const auto path = qEnvironmentVariable("KUGOU_ACCOUNT_API_KEY_FILE");
    if (path.isEmpty())
        return QString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failure("External key file is unavailable", ErrorCode::Io);
    if (file.size() > 4096)
        return failure("External key file is too large");
    return validateKey(QString::fromUtf8(file.readAll()));
}

Client::Client(sdk::IExtensionContext &context, ext::HttpTransport &transport) : m_context(context), m_transport(transport) {}

void Client::initialize(std::function<void()> done) {
    m_context.secretGet("account-key", [this, done](Result<QString> saved) {
        if (!saved)
            m_credentialError = "System keyring unavailable or legacy credential migration failed";
        else {
            m_keySaved = !saved.value().isEmpty();
            if (m_keySaved)
                m_key = saved.value();
            else if (const auto external = externalKey())
                m_key = external.value();
            else
                m_credentialError = "System keyring unavailable or legacy credential migration failed";
        }
        m_context.secretGet("account-session", [this, done](Result<QString> session) {
            if (!session)
                m_credentialError = "System keyring unavailable or legacy session migration failed";
            else if (!session.value().isEmpty()) {
                const auto object = QJsonDocument::fromJson(session.value().toUtf8()).object();
                bool valid = object.value("version").toInt() == 1 && object.value("cookies").isObject();
                for (const auto &value : object.value("cookies").toObject())
                    valid = valid && value.isString();
                if (valid)
                    m_cookies = object.value("cookies").toObject().toVariantMap();
                else
                    m_credentialError = "System keyring unavailable or legacy session migration failed";
            }
            done();
        });
    });
}
bool Client::configured() const {
    const auto config = m_context.config();
    return config.value("enabled").toBool() && !config.value("worker_url").toString().isEmpty();
}
bool Client::loggedIn() const {
    const auto dfid = m_cookies.value("dfid").toString();
    return !m_cookies.value("token").toString().isEmpty() && !m_cookies.value("userid").toString().isEmpty() &&
           !dfid.isEmpty() && dfid != "0" && dfid != "-" && dfid != "undefined";
}
QVariantMap Client::status() const {
    const auto config = m_context.config();
    return {{"enabled", config.value("enabled").toBool()},
            {"worker_url", config.value("worker_url").toString()},
            {"configured", configured() && !m_key.isEmpty()},
            {"key_saved", m_keySaved},
            {"logged_in", configured() && loggedIn()},
            {"credential_error", m_credentialError}};
}
void Client::saveKey(const QString &value, Done done) {
    const auto next = validateKey(value);
    if (!next)
        return done(next.error());
    m_context.secretSet("account-key", next.value(), [this, next = next.value(), done](Result<void> saved) {
        if (!saved)
            return done(saved);
        m_context.secretGet("account-key", [this, next, done](Result<QString> read) {
            if (!read)
                return done(read.error());
            if (read.value() != next)
                return done(failure("Cannot verify system keyring write"));
            m_key = next;
            m_keySaved = true;
            m_credentialError.clear();
            done({});
        });
    });
}
void Client::clearKey(Done done) {
    m_context.secretDelete("account-key", [this, done](Result<void> removed) {
        if (!removed)
            return done(removed);
        const auto external = externalKey();
        m_key = external ? external.value() : QString();
        m_keySaved = false;
        done({});
    });
}
void Client::bytes(const QUrl &url, const QJsonObject *body, qint64 limit, const QByteArray &accept,
                   std::function<void(Result<ext::HttpResponse>)> done) {
    ext::HttpRequest request;
    request.url = url;
    request.maxBytes = limit;
    request.headers = {{"Accept", accept}, {"User-Agent", "NekoTune-Kugou/1.0"}};
    if (body) {
        request.method = "POST";
        request.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
        request.headers.append(qMakePair(QByteArray("X-Account-Key"), m_key.toUtf8()));
        request.body = QJsonDocument(*body).toJson(QJsonDocument::Compact);
    }
    m_transport.send(request, [done](Result<ext::HttpResponse> result) {
        if (!result) {
            const auto &message = result.error().message;
            done(failure(message == "cancelled"            ? QStringLiteral("Cancelled")
                         : message == "response_too_large" ? QStringLiteral("Kugou response is too large")
                                                           : QStringLiteral("Cannot reach Kugou service or request timed out")));
            return;
        }
        if (result.value().status < 200 || result.value().status >= 300)
            return done(failure(QStringLiteral("Kugou request failed: HTTP %1").arg(result.value().status)));
        done(result);
    });
}
void Client::json(const QString &route, const Query &query, std::function<void(Result<QJsonObject>)> done,
                  const QJsonObject *body) {
    if (!configured())
        return done(failure("Enable Kugou and configure its Worker in plugin settings"));
    QString base = route;
    if (!route.startsWith("https:")) {
        const auto origin = workerOrigin(m_context.config().value("worker_url").toString());
        if (!origin)
            return done(origin.error());
        base = origin.value() + prefix + route;
    }
    QUrl url(base);
    QStringList search;
    for (const auto &[key, value] : query)
        if (!value.isEmpty())
            search.append(QString::fromLatin1(QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value)));
    url.setQuery(search.join('&'), QUrl::StrictMode);
    bytes(url, body, 4 * 1024 * 1024, "application/json", [done](Result<ext::HttpResponse> response) {
        if (!response)
            return done(response.error());
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(response.value().body, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return done(failure("Invalid Kugou response"));
        done(document.object());
    });
}
void Client::account(const QString &route, QJsonObject body, std::function<void(Result<QJsonObject>)> done) {
    if (m_key.isEmpty())
        return done(failure("Set the Kugou account key in plugin settings"));
    body.insert("cookies", QJsonObject::fromVariantMap(m_cookies));
    json(route, {}, [this, done](Result<QJsonObject> result) {
        if (!result)
            return done(result);
        auto value = result.value();
        const auto updated = value.take("cookies");
        if (!updated.isObject())
            return done(value);
        auto cookies = m_cookies;
        const auto object = updated.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.value().isString())
                cookies.insert(it.key(), it.value().toString());
        const auto serialized = QString::fromUtf8(
            QJsonDocument(QJsonObject{{"version", 1}, {"cookies", QJsonObject::fromVariantMap(cookies)}}).toJson(QJsonDocument::Compact));
        if (serialized.toUtf8().size() > 16384)
            return done(failure("Kugou session exceeds storage limit"));
        m_context.secretSet("account-session", serialized, [this, cookies, value, done](Result<void> saved) {
            if (!saved)
                return done(saved.error());
            m_context.secretGet("account-session", [this, cookies, value, done](Result<QString> read) {
                const auto stored = read ? QJsonDocument::fromJson(read.value().toUtf8()).object().value("cookies").toObject().toVariantMap()
                                         : QVariantMap{};
                if (!read || stored != cookies)
                    return done(failure("Cannot verify system keyring session write"));
                m_cookies = cookies;
                done(value);
            });
        });
    }, &body);
}
void Client::search(const QString &query, int page, Reply done) {
    if (query.trimmed().isEmpty() || query.size() > 200 || page < 1 || page > 1000)
        return done(failure("Invalid search keywords or page"));
    json("/search",
         {{"keywords", query.trimmed()}, {"type", "song"}, {"page", QString::number(page)}, {"pagesize", "30"}},
         [page, done](Result<QJsonObject> body) {
             if (!body)
                 return done(body.error());
             const auto tracks = parseTracks(body.value());
             if (!tracks)
                 return done(tracks.error());
             QVariantMap result{{"tracks", tracks.value()}};
             if (body.value().value("data").toObject().value("lists").toArray().size() == 30 && page < 1000)
                 result.insert("cursor", QString::number(page + 1));
             done(result);
         });
}
void Client::resolve(const QVariantMap &track, Reply done) {
    const auto id = track.value("id").toString();
    if (!validHash(id))
        return done(failure("Invalid Kugou track ID"));
    if (!loggedIn())
        return done(failure("Log in to Kugou first"));
    QJsonObject body{{"hash", id}, {"quality", "128"}};
    if (const auto audio = track.value("audio_id").toString(); !audio.isEmpty())
        body.insert("album_audio_id", audio);
    account("/song/url", body, [done](Result<QJsonObject> result) {
        if (!result)
            return done(result.error());
        const auto &value = result.value();
        if (!businessOk(value, {"1", "200"})) {
            if (text(value.value("error_code")) == "20028")
                return done(failure("Kugou requires additional account verification"));
            for (const auto &item : value.value("fail_process").toArray())
                if (item.toString() == "pkg" || item.toString() == "buy")
                    return done(failure("Kugou account has no playback permission for this song"));
            return done(failure("Kugou did not provide playback permission"));
        }
        auto urls = value.contains("url") ? value.value("url") : value.value("data").toObject().value("url");
        const auto url = audioUrl(urls.isArray() ? urls.toArray().first().toString() : urls.toString());
        if (!url)
            return done(url.error());
        done(QVariantMap{{"url", url.value()},
                         {"headers", QVariantMap{{"User-Agent", "Mozilla/5.0 NekoTune/1.0"}}},
                         {"expiresAt", QDateTime::currentMSecsSinceEpoch() + 5 * 60'000},
                         {"extension", "mp3"},
                         {"maxBytes", 100LL * 1024 * 1024},
                         {"allowedHosts", QStringList{"*.kugou.com"}},
                         {"contentTypes", QStringList{"audio/mpeg", "audio/mp3", "audio/flac", "audio/x-flac", "audio/aac",
                                                      "audio/mp4", "audio/x-m4a", "audio/ogg", "application/ogg", "audio/wav",
                                                      "audio/x-wav", "audio/webm"}}});
    });
}
} // namespace kugou
