#pragma once
#include "client.h"

namespace kugou {
void lyricCandidates(Client &client, const QVariantMap &track, std::function<void(Result<QVariantList>)> done);
/// Document for one candidate. binary carries the raw KRC (for the .krc sidecar) when available.
struct Document {
    QVariantMap value;
    QByteArray binary;
};
void lyricDocument(Client &client, const QVariantMap &candidate, bool includeLrc,
                   std::function<void(Result<Document>)> done);
/// The single candidate matching title and artist whose duration is clearly closest; empty if none.
QVariantMap chooseAutomatic(const QVariantList &candidates, const QVariantMap &track);
/// After a download: write .krc/.lrc and cover sidecars unless the user already has them.
void saveAssets(Client &client, const QVariantMap &track, const QString &audioPath, bool existing, Reply done);
} // namespace kugou
