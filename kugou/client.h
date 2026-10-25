#pragma once
#include "common/http.h"
#include <nekotune/sdk/extension_context.h>
#include <QJsonObject>
#include <QVariantMap>

namespace kugou {
using nekotune::Result;
using Reply = std::function<void(Result<QVariantMap>)>;
using Done = std::function<void(Result<void>)>;

bool validHash(const QString &id);
/// Worker origin without path; empty when blank and Kugou is disabled.
Result<QString> workerOrigin(const QString &value, bool enabled = true);
Result<QString> audioUrl(const QString &value);
/// HTTPS artwork on imge.kugou.com with {size} filled in; empty otherwise.
QString coverUrl(const QString &value);
bool businessOk(const QJsonObject &body, const QStringList &expected = {"1"});
Result<QVariantList> parseTracks(const QJsonObject &body);
struct Krc {
    QByteArray binary;
    QString text;
};
Result<Krc> decodeKrc(const QJsonObject &body);
Result<QString> validateKey(const QString &value);
/// KUGOU_ACCOUNT_API_KEY or the file named by KUGOU_ACCOUNT_API_KEY_FILE.
Result<QString> externalKey();

/// Worker client. Account calls send the key and the stored cookie session; updated cookies are
/// written to the keyring and verified before the call succeeds.
class Client {
  public:
    using Query = QList<QPair<QString, QString>>;
    Client(nekotune::sdk::ExtensionContext &context, nekotune::ext::HttpTransport &transport);
    void initialize(std::function<void()> done);
    bool configured() const;
    bool loggedIn() const;
    QVariantMap status() const;
    void saveKey(const QString &value, Done done);
    void clearKey(Done done);
    void cancel() { m_transport.cancelAll(); }
    void bytes(const QUrl &url, const QJsonObject *body, qint64 limit, const QByteArray &accept,
               std::function<void(Result<nekotune::ext::HttpResponse>)> done);
    /// route is a Worker route ("/search") or an absolute https:// URL.
    void json(const QString &route, const Query &query, std::function<void(Result<QJsonObject>)> done,
              const QJsonObject *body = nullptr);
    void account(const QString &route, QJsonObject body, std::function<void(Result<QJsonObject>)> done);
    void search(const QString &query, int page, Reply done);
    void resolve(const QVariantMap &track, Reply done);

    QVariantMap cookies() const { return m_cookies; }
    void setCookies(const QVariantMap &cookies) { m_cookies = cookies; }
    QString key() const { return m_key; }

  private:
    nekotune::sdk::ExtensionContext &m_context;
    nekotune::ext::HttpTransport &m_transport;
    QString m_key, m_credentialError;
    bool m_keySaved = false;
    QVariantMap m_cookies;
};
} // namespace kugou
