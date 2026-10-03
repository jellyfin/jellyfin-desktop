#ifndef __CERTIFICATE_COMPONENT_H__
#define __CERTIFICATE_COMPONENT_H__

#include "ComponentManager.h"
#include <QNetworkReply>
#include <QPointer>
#include <QSslConfiguration>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <memory>

// Per-server CA and client (mTLS) certificates.
//
// Files are stored in <profile>/certs/<host>_<port>/ (CA.pem, client.pem, client.key).
// New uploads go to <profile>/certs/.pending/ first and are moved to the server
// directory by commit() once a connectivity check with them succeeded.
class CertificateComponent : public ComponentBase
{
  Q_OBJECT
  DEFINE_SINGLETON(CertificateComponent);

public:
  struct CertPaths
  {
    QString ca;
    QString cert;
    QString key;
    bool hasCa() const { return !ca.isEmpty(); }
    bool hasClient() const { return !cert.isEmpty() && !key.isEmpty(); }
  };

  bool componentExport() override { return true; }
  const char* componentName() override { return "certificates"; }
  bool componentInitialize() override;

  // Import a "ca" or "client" certificate (base64 encoded file contents) into the pending area.
  // Result: {success, fileName} or {success: false, reason: PASSWORD_REQUIRED | INVALID_PASSWORD |
  // INVALID_DATA | UNSUPPORTED | WRITE_ERROR | MISSING_COMMON_NAME}.
  Q_INVOKABLE QVariantMap stage(const QString& kind, const QString& fileName,
                                const QString& base64Data, const QString& password = QString());
  // {https, hasCa, caFileName, hasClient, clientFileName}; empty url = pending area
  Q_INVOKABLE QVariantMap info(const QString& url = QString()) const;
  // kind: "ca", "client" or "all"; empty url = pending area
  Q_INVOKABLE bool remove(const QString& url, const QString& kind);
  Q_INVOKABLE void clearPending();
  // Move pending certificates to the directory of url. Only allowed for https.
  Q_INVOKABLE bool commit(const QString& url);

  // QtWebEngine integration (called from QML)
  Q_INVOKABLE void setProfile(QObject* profile);
  Q_INVOKABLE int matchClientCert(const QVariantList& certificates, const QString& host) const;
  Q_INVOKABLE void handleCertificateError(const QVariant& certificateError);

  // Failed requests
  struct TlsFailure
  {
    QString message;
    bool certificateProblem = false; // retrying cannot fix it, the user has to load other certificates
  };
  // Keeps the SSL errors of reply, for describeFailure()
  static std::shared_ptr<QList<QSslError>> recordSslErrors(QNetworkReply* reply);
  // Client certificate without a CN on a TLS backend that crashes on it (QTBUG-142324)
  static bool isUnusableClientCert(const QSslCertificate& cert);
  static TlsFailure describeFailure(const QNetworkReply* reply, const QList<QSslError>& sslErrors);
  // True when a failed request is caused by certificates
  static bool isCertificateProblem(const QList<QSslError>& sslErrors, bool clientCertSent,
                                   QNetworkReply::NetworkError error, const QString& errorString);
  // User facing explanation: certificate problems are described, anything else returns fallback
  static QString describeTlsFailure(const QList<QSslError>& sslErrors, bool clientCertSent,
                                    QNetworkReply::NetworkError error, const QString& fallback);

  static QString dirName(const QUrl& url);
  static int effectivePort(const QUrl& url);
  static QUrl normalizeUrl(const QString& url);

  QString rootDir() const;
  QString pendingDir() const;
  QString serverDir(const QUrl& url) const;
  CertPaths certPaths(const QUrl& url, bool includePending = false) const;
  // Add the custom CAs of url to config and set its client certificate. Returns true if anything was applied.
  bool applyTo(QSslConfiguration& config, const QUrl& url, bool includePending = true) const;
  void refreshClientStore();
  // Calls done(true) if the server at url presents expected and it validates against its custom CA
  void verifyServer(const QUrl& url, const QSslCertificate& expected, std::function<void(bool)> done);

private:
  CertificateComponent(QObject* parent = nullptr) : ComponentBase(parent) {}

  QPointer<QObject> m_profile;
};

#endif // __CERTIFICATE_COMPONENT_H__
