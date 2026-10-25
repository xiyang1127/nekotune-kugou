#include "plugin.h"

using namespace nekotune;

class KugouPlugin final : public QObject, public sdk::IExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NEKOTUNE_EXTENSION_IID)
    Q_INTERFACES(nekotune::sdk::IExtensionPlugin)
  public:
    void activate(sdk::ExtensionContext &context, sdk::Done done) override {
        m_transport = std::make_unique<ext::NetworkTransport>();
        m_extension = std::make_unique<kugou::Extension>(context, *m_transport);
        m_extension->activate(std::move(done));
    }
    void deactivate() override {
        if (m_extension)
            m_extension->close();
        m_extension.reset();
        m_transport.reset();
    }

  private:
    std::unique_ptr<ext::NetworkTransport> m_transport;
    std::unique_ptr<kugou::Extension> m_extension;
};

#include "plugin.moc"
