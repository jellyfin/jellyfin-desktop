#ifndef ALBUMARTPROVIDER_H
#define ALBUMARTPROVIDER_H

#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class AlbumArtProvider : public QObject
{
  Q_OBJECT

public:
  explicit AlbumArtProvider(QObject* parent = nullptr);
  ~AlbumArtProvider() override;

  void requestArtwork(const QVariantMap& metadata, const QUrl& baseUrl);
  void setApiToken(const QString& token) { m_apiToken = token; }
  void cancelPending();

Q_SIGNALS:
  void artworkReady(const QByteArray& imageData, const QString& mimeType);
  void artworkUnavailable();

private Q_SLOTS:
  void onArtworkDownloaded();

private:
  QString extractArtworkUrl(const QVariantMap& metadata, const QUrl& baseUrl);
  void cleanup();

  QNetworkAccessManager* m_networkManager;
  QNetworkReply* m_pendingReply;
  QString m_pendingUrl;
  QString m_apiToken;
};

#endif // ALBUMARTPROVIDER_H
