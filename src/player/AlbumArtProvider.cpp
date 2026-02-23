#include "AlbumArtProvider.h"
#include "system/SystemComponent.h"
#include "settings/SettingsComponent.h"
#include "core/ProfileManager.h"

#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QMimeDatabase>
#include <QDebug>

AlbumArtProvider::AlbumArtProvider(QObject* parent)
  : QObject(parent)
  , m_networkManager(new QNetworkAccessManager(this))
  , m_pendingReply(nullptr)
{
  // Set up disk cache for album art
  qint64 cacheSize = SettingsComponent::Get().value("albumart", "cacheSize").toLongLong();
  if (cacheSize <= 0)
    cacheSize = 10485760; // 10MB default

  auto* cache = new QNetworkDiskCache(this);
  QString cacheDir = ProfileManager::activeProfile().cacheDir("albumart");
  cache->setCacheDirectory(cacheDir);
  cache->setMaximumCacheSize(cacheSize);
  m_networkManager->setCache(cache);
  qDebug() << "AlbumArtProvider: Cache enabled, max size:" << cacheSize << "bytes, dir:" << cacheDir;
}

AlbumArtProvider::~AlbumArtProvider()
{
  cleanup();
}

void AlbumArtProvider::requestArtwork(const QVariantMap& metadata, const QUrl& baseUrl)
{
  QString artUrl = extractArtworkUrl(metadata, baseUrl);

  if (artUrl.isEmpty())
  {
    emit artworkUnavailable();
    return;
  }

  // If already downloading this URL, wait for it
  if (m_pendingReply && m_pendingUrl == artUrl)
    return;

  cleanup();
  m_pendingUrl = artUrl;

  QNetworkRequest request;
  request.setUrl(QUrl(artUrl));
  request.setRawHeader("User-Agent", SystemComponent::Get().getUserAgent().toUtf8());
  request.setSslConfiguration(SystemComponent::Get().getSSLConfiguration());

  m_pendingReply = m_networkManager->get(request);
  if (SettingsComponent::Get().ignoreSSLErrors()) {
    connect(m_pendingReply, QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),
            m_pendingReply, QOverload<>::of(&QNetworkReply::ignoreSslErrors));
  }
  connect(m_pendingReply, &QNetworkReply::finished, this, &AlbumArtProvider::onArtworkDownloaded);
}

void AlbumArtProvider::cancelPending()
{
  cleanup();
}

void AlbumArtProvider::onArtworkDownloaded()
{
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply)
    return;

  if (reply == m_pendingReply)
    m_pendingReply = nullptr;

  reply->deleteLater();

  bool fromCache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();

  if (reply->error() != QNetworkReply::NoError)
  {
    if (reply->error() != QNetworkReply::OperationCanceledError)
      qDebug() << "AlbumArtProvider: Download failed:" << reply->errorString();
    emit artworkUnavailable();
    return;
  }

  QByteArray imageData = reply->readAll();
  if (imageData.isEmpty())
  {
    qDebug() << "AlbumArtProvider: Download returned empty data";
    emit artworkUnavailable();
    return;
  }

  static QMimeDatabase mimeDb;
  QString mimeType = mimeDb.mimeTypeForData(imageData).name();
  qDebug() << "AlbumArtProvider:" << (fromCache ? "cache hit" : "cache miss")
           << "-" << reply->url().toString() << "-" << imageData.size() << "bytes";

  if (mimeType.startsWith("image/"))
  {
    emit artworkReady(imageData, mimeType);
  }
  else
  {
    qDebug() << "AlbumArtProvider: Not an image type:" << mimeType;
    emit artworkUnavailable();
  }
}

QString AlbumArtProvider::extractArtworkUrl(const QVariantMap& metadata, const QUrl& baseUrl)
{
  if (baseUrl.isEmpty())
    return QString();

  QString mediaType = metadata.value("MediaType").toString();
  QString itemType = metadata.value("Type").toString();

  QUrl artUrl = baseUrl;
  QUrlQuery query;

  if (mediaType == "Audio" || itemType == "Audio")
  {
    if (metadata.contains("AlbumId") && metadata.contains("AlbumPrimaryImageTag"))
    {
      QString albumId = metadata["AlbumId"].toString();
      QString imageTag = metadata["AlbumPrimaryImageTag"].toString();

      if (!albumId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(albumId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }

    auto imageTags = metadata["ImageTags"].toMap();
    if (imageTags.contains("Primary") && metadata.contains("Id"))
    {
      QString itemId = metadata["Id"].toString();
      QString imageTag = imageTags["Primary"].toString();

      if (!itemId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(itemId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }
  }
  else if (itemType == "Episode")
  {
    if (metadata.contains("SeriesId") && metadata.contains("SeriesPrimaryImageTag"))
    {
      QString seriesId = metadata["SeriesId"].toString();
      QString imageTag = metadata["SeriesPrimaryImageTag"].toString();

      if (!seriesId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(seriesId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }

    if (metadata.contains("SeasonId") && metadata.contains("SeasonPrimaryImageTag"))
    {
      QString seasonId = metadata["SeasonId"].toString();
      QString imageTag = metadata["SeasonPrimaryImageTag"].toString();

      if (!seasonId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(seasonId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }

    auto imageTags = metadata["ImageTags"].toMap();
    if (imageTags.contains("Primary") && metadata.contains("Id"))
    {
      QString itemId = metadata["Id"].toString();
      QString imageTag = imageTags["Primary"].toString();

      if (!itemId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(itemId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }
  }
  else if (itemType == "Movie")
  {
    auto imageTags = metadata["ImageTags"].toMap();
    if (imageTags.contains("Primary") && metadata.contains("Id"))
    {
      QString itemId = metadata["Id"].toString();
      QString imageTag = imageTags["Primary"].toString();

      if (!itemId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(itemId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }

    if (imageTags.contains("Backdrop") && metadata.contains("Id"))
    {
      QString itemId = metadata["Id"].toString();
      QString imageTag = imageTags["Backdrop"].toString();

      if (!itemId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Backdrop/0").arg(itemId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }
  }
  else
  {
    auto imageTags = metadata["ImageTags"].toMap();
    if (imageTags.contains("Primary") && metadata.contains("Id"))
    {
      QString itemId = metadata["Id"].toString();
      QString imageTag = imageTags["Primary"].toString();

      if (!itemId.isEmpty() && !imageTag.isEmpty())
      {
        artUrl.setPath(QString("/Items/%1/Images/Primary").arg(itemId));
        query.addQueryItem("tag", imageTag);
        query.addQueryItem("maxWidth", "512");
        artUrl.setQuery(query);
        return artUrl.toString();
      }
    }
  }

  return QString();
}

void AlbumArtProvider::cleanup()
{
  if (m_pendingReply)
  {
    m_pendingReply->abort();
    QMetaObject::invokeMethod(m_pendingReply, "deleteLater", Qt::QueuedConnection);
    m_pendingReply = nullptr;
    m_pendingUrl.clear();
  }
}
