#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>

class QNetworkAccessManager;

namespace ot {

/// Pictures the document shows. Files are read when first asked for; pictures
/// on the web are fetched in the background and announced with imageReady().
class ImageStore : public QObject
{
    Q_OBJECT

public:
    explicit ImageStore(QObject *parent = nullptr);

    enum class State { Ready, Loading, Failed };

    /// The picture `source` (as written in the Markdown) names, relative paths
    /// being taken from `baseDir`.
    QImage image(const QString &source, const QString &baseDir, State *state = nullptr);

    /// The file a source names, empty for pictures on the web.
    static QString localPath(const QString &source, const QString &baseDir);

signals:
    void imageReady();

private:
    QNetworkAccessManager *network();

    QHash<QString, QImage> m_images;
    QSet<QString> m_failed;
    QSet<QString> m_pending;
    QNetworkAccessManager *m_network = nullptr;
};

} // namespace ot
