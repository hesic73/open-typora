#include "ImageStore.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace ot {

namespace {

constexpr int MaxSide = 4096;

QImage limited(QImage image)
{
    if (image.width() > MaxSide || image.height() > MaxSide)
        image = image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

bool isRemote(const QString &source)
{
    return source.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || source.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

} // namespace

ImageStore::ImageStore(QObject *parent)
    : QObject(parent)
{
}

QNetworkAccessManager *ImageStore::network()
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    return m_network;
}

QString ImageStore::localPath(const QString &source, const QString &baseDir)
{
    QString path = source.trimmed();
    if (path.isEmpty() || isRemote(path) || path.startsWith(QLatin1String("data:")))
        return {};
    if (path.startsWith(QLatin1String("file://")))
        return QUrl(path).toLocalFile();
    // Markdown writes spaces in paths as %20.
    if (path.contains(QLatin1Char('%')))
        path = QUrl::fromPercentEncoding(path.toUtf8());
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    if (QFileInfo(path).isRelative() && !baseDir.isEmpty())
        path = QDir(baseDir).filePath(path);
    return QDir::cleanPath(path);
}

QImage ImageStore::image(const QString &source, const QString &baseDir, State *state)
{
    const auto report = [&](State value) {
        if (state)
            *state = value;
    };
    const QString trimmed = source.trimmed();
    if (trimmed.startsWith(QLatin1String("data:"))) {
        const auto found = m_images.constFind(trimmed);
        if (found != m_images.constEnd()) {
            report(State::Ready);
            return *found;
        }
        const qsizetype comma = trimmed.indexOf(QLatin1Char(','));
        QImage image;
        if (comma > 0 && trimmed.left(comma).endsWith(QLatin1String(";base64")))
            image.loadFromData(QByteArray::fromBase64(trimmed.mid(comma + 1).toLatin1()));
        if (image.isNull()) {
            report(State::Failed);
            return {};
        }
        m_images.insert(trimmed, limited(image));
        report(State::Ready);
        return m_images.value(trimmed);
    }

    if (isRemote(trimmed)) {
        const auto found = m_images.constFind(trimmed);
        if (found != m_images.constEnd()) {
            report(State::Ready);
            return *found;
        }
        if (m_failed.contains(trimmed)) {
            report(State::Failed);
            return {};
        }
        report(State::Loading);
        if (!m_pending.contains(trimmed)) {
            m_pending.insert(trimmed);
            QNetworkRequest request{QUrl(trimmed)};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenTypora/0.1"));
            QNetworkReply *reply = network()->get(request);
            connect(reply, &QNetworkReply::finished, this, [this, reply, trimmed] {
                reply->deleteLater();
                m_pending.remove(trimmed);
                QImage image;
                if (reply->error() == QNetworkReply::NoError)
                    image.loadFromData(reply->readAll());
                if (image.isNull())
                    m_failed.insert(trimmed);
                else
                    m_images.insert(trimmed, limited(image));
                emit imageReady();
            });
        }
        return {};
    }

    const QString path = localPath(trimmed, baseDir);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile()) {
        report(State::Failed);
        return {};
    }
    const QString key = path + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    const auto found = m_images.constFind(key);
    if (found != m_images.constEnd()) {
        report(State::Ready);
        return *found;
    }
    if (m_failed.contains(key)) {
        report(State::Failed);
        return {};
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        m_failed.insert(key);
        report(State::Failed);
        return {};
    }
    m_images.insert(key, limited(image));
    report(State::Ready);
    return m_images.value(key);
}

} // namespace ot
