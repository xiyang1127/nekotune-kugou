#include "lyrics.h"
#include "common/media.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <memory>

using namespace nekotune;

namespace kugou {
namespace {
QString normalized(const QString &value) {
    QString result;
    for (const char32_t code : value.normalized(QString::NormalizationForm_KC).toLower().toUcs4()) {
        const auto category = QChar::category(code);
        const bool drop = QChar::isSpace(code) ||
                          (category >= QChar::Punctuation_Connector && category <= QChar::Punctuation_Other) ||
                          (category >= QChar::Symbol_Math && category <= QChar::Symbol_Other);
        if (!drop)
            result += QString::fromUcs4(&code, 1);
    }
    return result;
}
} // namespace

void lyricCandidates(Client &client, const QVariantMap &track, std::function<void(Result<QVariantList>)> done) {
    const auto duration = track.value("duration_ms").toLongLong();
    client.json("/search/lyric",
                {{"keywords", track.value("title").toString()},
                 {"hash", track.value("id").toString()},
                 {"man", "yes"},
                 {"duration", duration ? QString::number(duration) : QString()},
                 {"album_audio_id", track.value("audio_id").toString()}},
                [track, done](Result<QJsonObject> result) {
                    if (!result)
                        return done(result.error());
                    const int status = result.value().value("status").toVariant().toInt();
                    if ((status != 200 && status != 404) || !result.value().value("candidates").isArray())
                        return done(failure("Invalid Kugou lyric candidates"));
                    QVariantList candidates;
                    for (const auto &value : result.value().value("candidates").toArray()) {
                        const auto item = value.toObject();
                        if (item.value("id").toVariant().toDouble() <= 0 || item.value("accesskey").toString().isEmpty() ||
                            !item.value("accesskey").isString())
                            continue;
                        const auto song = item.value("song").toString(), singer = item.value("singer").toString();
                        const auto itemDuration = item.value("duration").toVariant().toLongLong();
                        candidates.append(QVariantMap{
                            {"id", item.value("id").toVariant().toString()},
                            {"accesskey", item.value("accesskey").toString()},
                            {"title", song.isEmpty() ? track.value("title") : song},
                            {"artist", singer.isEmpty() ? track.value("artist") : singer},
                            {"album", track.value("album", QString()).toString()},
                            {"duration_ms", itemDuration ? itemDuration : track.value("duration_ms").toLongLong()},
                            {"cover_url", track.value("cover_url", QString()).toString()},
                            {"score", 0}});
                        if (candidates.size() >= 100)
                            break;
                    }
                    done(candidates);
                });
}
void lyricDocument(Client &client, const QVariantMap &candidate, bool includeLrc, std::function<void(Result<Document>)> done) {
    const auto id = candidate.value("id").toString(), accesskey = candidate.value("accesskey").toString();
    client.json("https://lyrics.kugou.com/download",
                {{"ver", "1"}, {"client", "pc"}, {"id", id}, {"accesskey", accesskey}, {"fmt", "krc"}, {"charset", "utf8"}},
                [&client, candidate, includeLrc, id, accesskey, done](Result<QJsonObject> response) {
                    // A broken KRC falls back to LRC for the same selected version.
                    auto krc = response ? decodeKrc(response.value()) : Result<Krc>(response.error());
                    auto finish = [candidate, krc, done](const QString &lrc) {
                        if (!krc && lrc.trimmed().isEmpty())
                            return done(failure("Selected lyrics are empty"));
                        static const QRegularExpression synced(R"(\[\d+:\d+(?:\.\d+)?\])");
                        Document document;
                        document.value = {{"krc_lyrics", krc ? krc.value().text : QString()},
                                          {"synced_lyrics", synced.match(lrc).hasMatch() ? lrc : QString()},
                                          {"plain_lyrics", lrc},
                                          {"cover_url", candidate.value("cover_url", QString()).toString()}};
                        if (krc)
                            document.binary = krc.value().binary;
                        done(document);
                    };
                    if (krc && !includeLrc)
                        return finish({});
                    client.json("/lyric", {{"id", id}, {"accesskey", accesskey}, {"fmt", "lrc"}, {"decode", "1"}},
                                [krc, finish, done](Result<QJsonObject> lrc) {
                                    if (!lrc) {
                                        if (!krc)
                                            return done(failure("Cannot download selected lyrics"));
                                        return finish({});
                                    }
                                    const auto content = lrc.value().value("decodeContent");
                                    finish(content.isString() ? content.toString() : content.toVariant().toString());
                                });
                });
}
QVariantMap chooseAutomatic(const QVariantList &candidates, const QVariantMap &track) {
    const auto title = normalized(track.value("title").toString()), artist = normalized(track.value("artist").toString());
    const auto trackDuration = track.value("duration_ms").toLongLong();
    QList<std::pair<qint64, QVariantMap>> choices;
    for (const auto &value : candidates) {
        const auto item = value.toMap();
        if (normalized(item.value("title").toString()) != title || normalized(item.value("artist").toString()) != artist)
            continue;
        const auto duration = item.value("duration_ms").toLongLong();
        const qint64 delta = duration > 0 && trackDuration > 0 ? qAbs(duration - trackDuration) : 0;
        if (delta <= 3000)
            choices.append({delta, item});
    }
    std::stable_sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (!choices.isEmpty() && (choices.size() == 1 || choices[0].first < choices[1].first))
        return choices[0].second;
    return {};
}
void saveAssets(Client &client, const QVariantMap &track, const QString &audioPath, bool existing, Reply done) {
    // Existing songs and user sidecars keep their previous assets and custom lyrics.
    if (existing)
        return done(QVariantMap{{"lyric_status", "existing"}, {"cover_status", "existing"}});
    const QFileInfo audio(audioPath);
    const auto base = audio.dir().filePath(audio.completeBaseName());
    auto statuses = std::make_shared<QVariantMap>(QVariantMap{{"lyric_status", "none"}, {"cover_status", "none"}});
    auto cover = [&client, track, base, statuses, done] {
        if (ext::pathExists(base + ".jpg") || ext::pathExists(base + ".png") || ext::pathExists(base + ".webp")) {
            statuses->insert("cover_status", "existing");
            return done(*statuses);
        }
        const auto url = coverUrl(track.value("cover_url").toString());
        if (url.isEmpty())
            return done(*statuses);
        client.bytes(QUrl(url), nullptr, 5 * 1024 * 1024, "image/jpeg,image/png,image/webp",
                     [base, statuses, done](Result<ext::HttpResponse> image) {
                         const auto suffix = image ? ext::imageSuffix(image.value().body, QString::fromLatin1(image.value().header("content-type")))
                                                   : QString();
                         const auto saved = suffix.isEmpty() ? Result<QString>(failure("Unsupported or oversized cover response"))
                                                             : ext::publishFile(base + "." + suffix, image.value().body);
                         statuses->insert("cover_status", saved ? saved.value() : QStringLiteral("error"));
                         done(*statuses);
                     });
    };
    if (ext::pathExists(base + ".krc") || ext::pathExists(base + ".lrc")) {
        statuses->insert("lyric_status", "existing");
        return cover();
    }
    lyricCandidates(client, track, [&client, track, base, statuses, cover](Result<QVariantList> candidates) {
        if (!candidates) {
            statuses->insert("lyric_status", "error");
            return cover();
        }
        const auto selected = chooseAutomatic(candidates.value(), track);
        if (selected.isEmpty()) {
            statuses->insert("lyric_status", candidates.value().isEmpty() ? "none" : "uncertain");
            return cover();
        }
        lyricDocument(client, selected, true, [base, statuses, cover](Result<Document> document) {
            if (!document) {
                statuses->insert("lyric_status", "error");
                return cover();
            }
            QString status = "none";
            if (!document.value().binary.isEmpty()) {
                const auto saved = ext::publishFile(base + ".krc", document.value().binary);
                status = saved ? saved.value() : QStringLiteral("error");
            }
            const auto synced = document.value().value.value("synced_lyrics").toString();
            if (!synced.isEmpty()) {
                const auto saved = ext::publishFile(base + ".lrc", synced.toUtf8());
                if (status != "saved" && status != "existing")
                    status = saved ? saved.value() : QStringLiteral("error");
            }
            statuses->insert("lyric_status", status);
            cover();
        });
    });
}
} // namespace kugou
