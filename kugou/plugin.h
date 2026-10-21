#pragma once
#include "lyrics.h"
#include <nekotune/sdk/extension_plugin.h>
#include <QObject>
#include <memory>
#include <vector>

namespace kugou {
class Music;
class Lyrics;

/// Plugin logic, separate from the Qt plugin shell so tests can drive it with a fake context.
class Extension {
  public:
    Extension(nekotune::sdk::IExtensionContext &context, nekotune::ext::HttpTransport &transport);
    ~Extension();
    void activate(std::function<void(Result<void>)> done);
    void close();
    Client &client() { return m_client; }

  private:
    friend class Music;
    friend class Lyrics;
    QVariantMap status() const;
    void notify();
    void emitEvent(const QString &name, const QVariantMap &data = {});
    void remember(const QVariantMap &result);
    Result<QVariantMap> track(const QString &id) const;
    Result<void> configureSources();
    void operation(const QString &name, std::function<void(Reply)> action, nekotune::sdk::Reply reply);
    void registerServices();
    void service(const QString &id, nekotune::sdk::Handler handler);

    nekotune::sdk::IExtensionContext &m_context;
    Client m_client;
    std::unique_ptr<Music> m_music;
    std::unique_ptr<Lyrics> m_lyrics;
    QStringList m_trackOrder;
    QHash<QString, QVariantMap> m_tracks;
    std::vector<nekotune::sdk::RegistrationPtr> m_registrations, m_sources;
    QString m_sourceSignature, m_activeTask;
    bool m_busy = false, m_closed = false, m_sourcesKnown = false;
};
} // namespace kugou
