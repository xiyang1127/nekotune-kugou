#include "fake_context.h"
#include "fake_transport.h"
#include "kugou/plugin.h"
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <zlib.h>

using namespace nekotune;
using namespace kugou;

namespace {
const QString hash = "0123456789abcdef0123456789abcdef";
void configure(FakeContext &context) {
    context.extensionId = "nekotune.kugou";
    context.configuration = {{"enabled", true}, {"worker_url", "https://worker.example"}, {"legacy_imported", true}};
    context.storedSecrets.insert("account-key", "fixture-key");
}
QJsonObject encodedKrc(const QByteArray &text = "[0,2000]<0,1000,0>Hello<1000,1000,0>world") {
    uLongf size = compressBound(uLong(text.size()));
    QByteArray compressed(qsizetype(size), Qt::Uninitialized);
    compress(reinterpret_cast<Bytef *>(compressed.data()), &size, reinterpret_cast<const Bytef *>(text.constData()), uLong(text.size()));
    compressed.resize(qsizetype(size));
    static const uchar key[] = {0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47, 0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69};
    for (qsizetype i = 0; i < compressed.size(); ++i)
        compressed[i] = char(uchar(compressed[i]) ^ key[i % 16]);
    return {{"status", 200}, {"content", QString::fromLatin1(("krc1" + compressed).toBase64())}};
}
template <class T> Result<T> run(std::function<void(std::function<void(Result<T>)>)> call) {
    std::optional<Result<T>> result;
    call([&](Result<T> value) { result = std::move(value); });
    return result ? *result : Result<T>(failure("pending"));
}
} // namespace

class KugouTest final : public QObject {
    Q_OBJECT
  private slots:
    void routesSearchAndUrlBoundaries();
    void accountStateIsPersistedBeforeSuccess();
    void refusesRedirectsOversizeAndMissingEntitlement();
    void stagedLyricsDecodeKrcAndFallBack();
    void sidecarsPreserveUserAssets();
    void extensionRegistersSourcesAndDownloads();
};

void KugouTest::routesSearchAndUrlBoundaries() {
    QCOMPARE(workerOrigin("https://kugou-lyrics-api.lyuy.workers.dev/").value(), "https://luy-music-api.lyuy.workers.dev");
    QCOMPARE(workerOrigin("http://127.0.0.1:1234").value(), "http://127.0.0.1:1234");
    for (const auto &url : {"http://public.example", "https://worker.example/api/music/kugou/v1", "https://key@worker.example",
                            "https://worker.example?key=secret"})
        QVERIFY2(!workerOrigin(url), url);
    QVERIFY(!audioUrl("https://kugou.com.evil.example/song"));
    QVERIFY(!audioUrl("http://fs.kugou.com/song"));
    QCOMPARE(coverUrl("http://imge.kugou.com/{size}/cover.png"), "https://imge.kugou.com/240/cover.png");
    const QJsonObject body{
        {"status", 1},
        {"error_code", 0},
        {"data", QJsonObject{{"lists", QJsonArray{QJsonObject{
                                          {"FileHash", hash.toUpper()},
                                          {"SongName", "<em>Title</em>"},
                                          {"SingerName", "Singer"},
                                          {"Duration", 3},
                                          {"Image", "http://imge.kugou.com/{size}/a.png"},
                                          {"Grp", QJsonArray{QJsonObject{{"FileHash", hash}},
                                                             QJsonObject{{"FileHash", QString(32, 'a')}, {"SongName", "Variant"}}}}}}}}}};
    const auto tracks = parseTracks(body).value();
    QCOMPARE(tracks.size(), 2);
    QCOMPARE(tracks[0].toMap().value("id").toString(), hash);
    QCOMPARE(tracks[0].toMap().value("duration_ms").toLongLong(), 3000);
    QCOMPARE(tracks[1].toMap().value("artist").toString(), "Singer");
    QCOMPARE(tracks[0].toMap().value("title").toString(), "Title");
}
void KugouTest::accountStateIsPersistedBeforeSuccess() {
    FakeContext context;
    configure(context);
    FakeTransport transport;
    transport.handler = [](const ext::HttpRequest &) -> std::optional<Result<ext::HttpResponse>> {
        return jsonResponse({{"status", 1}, {"cookies", QJsonObject{{"token", "private-token"}, {"userid", "7"}, {"dfid", "device"}}}});
    };
    Client client(context, transport);
    client.initialize([] {});
    const auto login = run<QJsonObject>([&](auto done) {
        client.account("/login/cellphone", {{"mobile", "13800138000"}, {"code", "123456"}}, done);
    });
    QVERIFY(login);
    QCOMPARE(transport.requests[0].url.toString(), "https://worker.example/api/music/kugou/v1/login/cellphone");
    bool keyHeader = false;
    for (const auto &[name, value] : transport.requests[0].headers)
        keyHeader = keyHeader || (name == "X-Account-Key" && value == "fixture-key");
    QVERIFY(keyHeader);
    QVERIFY(client.loggedIn());
    QVERIFY(context.storedSecrets.value("account-session").contains("private-token"));
    const auto status = QJsonDocument(QJsonObject::fromVariantMap(client.status())).toJson();
    QVERIFY(!status.contains("private-token") && !status.contains("fixture-key"));
    const auto previous = client.cookies();
    context.secretWriter = [](const QString &, const QString &) -> Result<void> { return failure("Keyring unavailable"); };
    const auto failed = run<QJsonObject>([&](auto done) { client.account("/song/url", {}, done); });
    QCOMPARE(failed.error().message, "Keyring unavailable");
    QCOMPARE(client.cookies(), previous);
    QVERIFY(!run<void>([&](auto done) { client.saveKey("new-key", done); }));
    QCOMPARE(client.key(), "fixture-key");
}
void KugouTest::refusesRedirectsOversizeAndMissingEntitlement() {
    FakeContext context;
    configure(context);
    FakeTransport transport;
    Client client(context, transport);
    client.initialize([] {});
    transport.handler = [](const ext::HttpRequest &) -> std::optional<Result<ext::HttpResponse>> {
        return rawResponse({}, 302, {{"location", "https://evil.example"}});
    };
    QVERIFY(run<QVariantMap>([&](auto done) { client.search("Song", 1, done); }).error().message.contains("HTTP 302"));
    transport.handler = [](const ext::HttpRequest &) -> std::optional<Result<ext::HttpResponse>> {
        return failure("response_too_large");
    };
    QVERIFY(run<QVariantMap>([&](auto done) { client.search("Song", 1, done); }).error().message.contains("too large"));
    transport.handler = [](const ext::HttpRequest &) -> std::optional<Result<ext::HttpResponse>> {
        return jsonResponse({{"status", 0}, {"fail_process", QJsonArray{"buy"}}});
    };
    client.setCookies({{"token", "token"}, {"userid", "1"}, {"dfid", "device"}});
    QVERIFY(run<QVariantMap>([&](auto done) { client.resolve({{"id", hash}}, done); })
                .error()
                .message.contains("no playback permission"));
}
void KugouTest::stagedLyricsDecodeKrcAndFallBack() {
    FakeContext context;
    configure(context);
    FakeTransport transport;
    bool brokenKrc = false;
    transport.handler = [&](const ext::HttpRequest &request) -> std::optional<Result<ext::HttpResponse>> {
        if (request.url.path().endsWith("/search/lyric"))
            return jsonResponse({{"status", 200},
                                 {"candidates", QJsonArray{QJsonObject{{"id", 12}, {"accesskey", "lyric-key"}, {"song", "Title"},
                                                                       {"singer", "Singer"}, {"duration", 3000}}}}});
        if (request.url.host() == "lyrics.kugou.com")
            return jsonResponse(brokenKrc ? QJsonObject{{"status", 500}} : encodedKrc());
        return jsonResponse({{"status", 200}, {"decodeContent", "[00:01.00]Fallback"}});
    };
    Client client(context, transport);
    const QVariantMap track{{"id", hash}, {"title", "Title"}, {"artist", "Singer"}, {"duration_ms", 3000}};
    const auto candidates = run<QVariantList>([&](auto done) { lyricCandidates(client, track, done); }).value();
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(chooseAutomatic(candidates, track).value("id").toString(), "12");
    QVERIFY(chooseAutomatic(candidates + candidates, track).isEmpty());
    const auto krc = run<Document>([&](auto done) { lyricDocument(client, candidates[0].toMap(), false, done); }).value();
    QVERIFY(krc.value.value("krc_lyrics").toString().contains("Hello"));
    QVERIFY(krc.binary.startsWith("krc1"));
    QCOMPARE(QUrlQuery(transport.requests[1].url).queryItemValue("fmt"), "krc");
    QCOMPARE(transport.requests[1].url.host(), "lyrics.kugou.com");
    brokenKrc = true;
    QCOMPARE(run<Document>([&](auto done) { lyricDocument(client, candidates[0].toMap(), false, done); })
                 .value()
                 .value.value("synced_lyrics")
                 .toString(),
             "[00:01.00]Fallback");
    QVERIFY(!decodeKrc({{"status", 200}, {"content", "%%%"}}));
    QVERIFY(!decodeKrc(encodedKrc("not lyrics")));
}
void KugouTest::sidecarsPreserveUserAssets() {
    QTemporaryDir folder;
    QFile user(folder.filePath("track.lrc"));
    QVERIFY(user.open(QIODevice::WriteOnly));
    user.write("user lyrics");
    user.close();
    FakeContext context;
    configure(context);
    FakeTransport transport;
    transport.handler = [](const ext::HttpRequest &) -> std::optional<Result<ext::HttpResponse>> { return failure("network_error"); };
    Client client(context, transport);
    const auto statuses = run<QVariantMap>([&](auto done) { saveAssets(client, {{"id", hash}}, folder.filePath("track.mp3"), false, done); });
    QCOMPARE(statuses.value().value("lyric_status").toString(), "existing");
    QCOMPARE(transport.requests.size(), 0);
    run<QVariantMap>([&](auto done) {
        saveAssets(client, {{"id", hash}, {"cover_url", "https://imge.kugou.com/a.png"}}, folder.filePath("other.mp3"), true, done);
    });
    QCOMPARE(transport.requests.size(), 0);
    QVERIFY(user.open(QIODevice::ReadOnly));
    QCOMPARE(user.readAll(), "user lyrics");
}
void KugouTest::extensionRegistersSourcesAndDownloads() {
    FakeContext context;
    configure(context);
    FakeTransport transport;
    Extension extension(context, transport);
    QVERIFY(run<void>([&](auto done) { extension.activate(done); }));
    QVERIFY(context.musicProviders.contains("music"));
    QVERIFY(context.lyricsProviders.contains("lyrics"));
    QVERIFY(context.call("status").value().value("key_saved").toBool());
    QVERIFY(!context.call("config.set", {{"enabled", true}, {"worker_url", "http://evil.example"}}));
    QCOMPARE(context.configuration.value("worker_url").toString(), "https://worker.example");
    const auto download = context.call("download", {{"hash", hash}});
    QCOMPARE(download.value().value("method").toString(), "music.download");
    QCOMPARE(download.value().value("params").toMap().value("source").toString(), "nekotune.kugou/music");
    context.deliver("music.download", {{"task", "fixture-task"}, {"state", "finished"}, {"song_id", 7}});
    bool finished = false;
    for (const auto &event : context.publishedEvents)
        finished = finished || (event.name == "download_finished" && event.data.value("song_id").toInt() == 7);
    QVERIFY(finished);
    QVERIFY(context.call("config.set", {{"enabled", false}, {"worker_url", "https://worker.example"}}));
    QVERIFY(context.musicProviders.isEmpty() && context.lyricsProviders.isEmpty());
    // The manager writes config directly instead of invoking the plugin's config.set service.
    auto config = context.configuration;
    config.insert("enabled", true);
    context.config().update(config, [](Result<void>) {});
    QVERIFY(context.musicProviders.contains("music") && context.lyricsProviders.contains("lyrics"));
    config.insert("worker_url", "http://evil.example");
    context.config().update(config, [](Result<void>) {});
    QVERIFY(context.musicProviders.isEmpty() && context.lyricsProviders.isEmpty());
    QVERIFY(context.published("operation_failed"));
    config.insert("worker_url", "https://worker.example");
    context.config().update(config, [](Result<void>) {});
    QVERIFY(context.musicProviders.contains("music") && context.lyricsProviders.contains("lyrics"));
    extension.close();
    QVERIFY(context.musicProviders.isEmpty() && context.serviceHandlers.isEmpty());
}

QTEST_GUILESS_MAIN(KugouTest)
#include "kugou_test.moc"
