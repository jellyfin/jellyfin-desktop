#include "CertificateComponent.h"
#include "Pkcs12.h"
#include "core/ProfileManager.h"

#include <QCoreApplication>
#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>

#include <QtWebEngineCore/qwebenginecertificateerror.h>
#include <QtWebEngineCore/qwebengineclientcertificatestore.h>
#include <QtWebEngineQuick/qquickwebengineprofile.h>

static constexpr int VERIFY_TIMEOUT_MS = 10000;

// The files of one certificate directory (a server's or the pending one)
struct CertFolder
{
  QString dir;
  QString ca() const { return dir + "/CA.pem"; }
  QString cert() const { return dir + "/client.pem"; }
  QString key() const { return dir + "/client.key"; }
  QString names() const { return dir + "/names.json"; }
  bool hasCa() const { return QFile::exists(ca()); }
  bool hasClient() const { return QFile::exists(cert()) && QFile::exists(key()); }
};

/////////////////////////////////////////////////////////////////////////////////////////
static bool loadIdentity(const QString& certPath, const QString& keyPath, QSslCertificate& cert, QSslKey& key)
{
  cert = QSslCertificate::fromPath(certPath, QSsl::Pem).value(0);
  key = Pkcs12::loadPrivateKey(keyPath);
  return !cert.isNull() && !key.isNull();
}

/////////////////////////////////////////////////////////////////////////////////////////
static QVariantMap failure(const QString& reason)
{
  return {{"success", false}, {"reason", reason}};
}

/////////////////////////////////////////////////////////////////////////////////////////
static QString reasonFor(Pkcs12::Result result)
{
  switch (result)
  {
    case Pkcs12::Result::PasswordRequired: return "PASSWORD_REQUIRED";
    case Pkcs12::Result::InvalidPassword: return "INVALID_PASSWORD";
    case Pkcs12::Result::Unsupported: return "UNSUPPORTED";
    default: return "INVALID_DATA";
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
static bool writeFile(const QString& path, const QByteArray& content, QFileDevice::Permissions perms = {})
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(content) != content.size())
  {
    qWarning() << "CertificateComponent: failed to write" << path;
    return false;
  }
  file.close();
  if (perms)
    file.setPermissions(perms);
  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
static QJsonObject readNames(const CertFolder& folder)
{
  QFile file(folder.names());
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}

/////////////////////////////////////////////////////////////////////////////////////////
static void setName(const CertFolder& folder, const QString& kind, const QString& name)
{
  QJsonObject names = readNames(folder);
  if (name.isEmpty())
    names.remove(kind);
  else
    names.insert(kind, name);
  writeFile(folder.names(), QJsonDocument(names).toJson(QJsonDocument::Compact));
}

/////////////////////////////////////////////////////////////////////////////////////////
static bool replaceFile(const QString& from, const QString& to)
{
  QFile::remove(to);
  return QFile::copy(from, to);
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::componentInitialize()
{
  clearPending();
  connect(qApp, &QCoreApplication::aboutToQuit, this, &CertificateComponent::clearPending);
  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
int CertificateComponent::effectivePort(const QUrl& url)
{
  if (url.port() > 0)
    return url.port();
  return url.scheme().compare("https", Qt::CaseInsensitive) == 0 ? 443 : 80;
}

/////////////////////////////////////////////////////////////////////////////////////////
QUrl CertificateComponent::normalizeUrl(const QString& url)
{
  QString str = url.trimmed();
  if (str.isEmpty())
    return QUrl();
  if (str.startsWith("//"))
    str = "https:" + str;
  else if (!str.contains("://"))
    str = "https://" + str;
  return QUrl(str);
}

/////////////////////////////////////////////////////////////////////////////////////////
QString CertificateComponent::dirName(const QUrl& url)
{
  static const QRegularExpression invalidChars("[^a-z0-9.-]");
  QString host = url.host().toLower();
  host.replace(invalidChars, "-");
  if (host.isEmpty())
    host = "server";
  return QString("%1_%2").arg(host).arg(effectivePort(url));
}

/////////////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<QList<QSslError>> CertificateComponent::recordSslErrors(QNetworkReply* reply)
{
  auto errors = std::make_shared<QList<QSslError>>();
  connect(reply, &QNetworkReply::sslErrors, reply, [errors](const QList<QSslError>& sslErrors) {
    *errors = sslErrors;
  });
  return errors;
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::isUnusableClientCert(const QSslCertificate& cert)
{
  return !cert.isNull() && cert.subjectInfo(QSslCertificate::CommonName).isEmpty() &&
         Pkcs12::isSchannelMissingCnBugPresent();
}

/////////////////////////////////////////////////////////////////////////////////////////
CertificateComponent::TlsFailure CertificateComponent::describeFailure(const QNetworkReply* reply,
                                                                      const QList<QSslError>& sslErrors)
{
  // applyTo() left out the client certificate, the server will keep refusing the connection
  CertPaths paths = Get().certPaths(reply->request().url(), true);
  if (paths.hasClient() && isUnusableClientCert(QSslCertificate::fromPath(paths.cert, QSsl::Pem).value(0)))
    return {tr("The client certificate has no Common Name (CN) and cannot be used on this system. "
               "Load a client certificate that has a CN."), true};

  bool clientCertSent = !reply->request().sslConfiguration().localCertificate().isNull();
  return {describeTlsFailure(sslErrors, clientCertSent, reply->error(), reply->errorString()),
          isCertificateProblem(sslErrors, clientCertSent, reply->error(), reply->errorString())};
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::isCertificateProblem(const QList<QSslError>& sslErrors, bool clientCertSent,
                                                QNetworkReply::NetworkError error, const QString& errorString)
{
  if (!sslErrors.isEmpty())
    return true;

  // The server's chain was fine but the handshake failed: it refused our client certificate
  return clientCertSent &&
         (error == QNetworkReply::SslHandshakeFailedError || error == QNetworkReply::RemoteHostClosedError ||
          errorString.contains("alert", Qt::CaseInsensitive));
}

/////////////////////////////////////////////////////////////////////////////////////////
QString CertificateComponent::describeTlsFailure(const QList<QSslError>& sslErrors, bool clientCertSent,
                                                 QNetworkReply::NetworkError error, const QString& fallback)
{
  auto has = [&sslErrors](std::initializer_list<QSslError::SslError> types) -> const QSslError* {
    for (const QSslError& e : sslErrors)
    {
      if (std::find(types.begin(), types.end(), e.error()) != types.end())
        return &e;
    }
    return nullptr;
  };

  if (const QSslError* e = has({QSslError::SelfSignedCertificateInChain, QSslError::SelfSignedCertificate,
                                QSslError::UnableToGetLocalIssuerCertificate, QSslError::UnableToGetIssuerCertificate,
                                QSslError::UnableToVerifyFirstCertificate, QSslError::CertificateUntrusted}))
  {
    QString issuer = e->certificate().issuerDisplayName();
    if (issuer.isEmpty())
      return tr("The server certificate is not trusted. Load the CA certificate that issued it.");
    return tr("The server certificate is issued by \"%1\", which is not trusted. "
              "Load the CA certificate that issued it.").arg(issuer);
  }

  if (const QSslError* e = has({QSslError::HostNameMismatch}))
  {
    QStringList names = e->certificate().subjectAlternativeNames().values(QSsl::DnsEntry);
    names += e->certificate().subjectAlternativeNames().values(QSsl::IpAddressEntry);
    if (names.isEmpty())
      names = e->certificate().subjectInfo(QSslCertificate::CommonName);
    return tr("The server certificate is not valid for this address (it is for: %1).").arg(names.join(", "));
  }

  if (has({QSslError::CertificateExpired}))
    return tr("The server certificate has expired.");
  if (has({QSslError::CertificateNotYetValid}))
    return tr("The server certificate is not valid yet.");

  if (isCertificateProblem(sslErrors, clientCertSent, error, fallback))
  {
    return tr("The server did not accept the client certificate. "
              "Load the client certificate issued for this server. (%1)").arg(fallback);
  }

  return fallback;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString CertificateComponent::rootDir() const
{
  return ProfileManager::activeProfile().dataDir("certs");
}

/////////////////////////////////////////////////////////////////////////////////////////
QString CertificateComponent::pendingDir() const
{
  return rootDir() + "/.pending";
}

/////////////////////////////////////////////////////////////////////////////////////////
QString CertificateComponent::serverDir(const QUrl& url) const
{
  return rootDir() + "/" + dirName(url);
}

/////////////////////////////////////////////////////////////////////////////////////////
CertificateComponent::CertPaths CertificateComponent::certPaths(const QUrl& url, bool includePending) const
{
  QList<CertFolder> folders;
  if (includePending)
    folders << CertFolder{pendingDir()};
  if (url.isValid() && !url.host().isEmpty())
    folders << CertFolder{serverDir(url)};

  CertPaths paths;
  for (const CertFolder& folder : folders)
  {
    if (!paths.hasCa() && folder.hasCa())
      paths.ca = folder.ca();
    if (!paths.hasClient() && folder.hasClient())
    {
      paths.cert = folder.cert();
      paths.key = folder.key();
    }
  }
  return paths;
}

/////////////////////////////////////////////////////////////////////////////////////////
QVariantMap CertificateComponent::stage(const QString& kind, const QString& fileName,
                                        const QString& base64Data, const QString& password)
{
  const CertFolder pending{pendingDir()};
  const QByteArray data = QByteArray::fromBase64(base64Data.toLatin1());
  const QByteArray pass = password.toUtf8();

  if (!QDir().mkpath(pending.dir))
    return failure("WRITE_ERROR");

  QVariantMap res = {{"success", true}, {"fileName", fileName}};

  if (kind == "ca")
  {
    QList<QSslCertificate> certs;
    Pkcs12::Result result = Pkcs12::readCertificates(data, pass, certs);
    if (result != Pkcs12::Result::Success)
      return failure(reasonFor(result));

    QByteArray pem;
    for (const QSslCertificate& cert : certs)
      pem += cert.toPem();
    if (!writeFile(pending.ca(), pem))
      return failure("WRITE_ERROR");
    setName(pending, "ca", fileName);
    qInfo() << "CertificateComponent: staged" << certs.size() << "CA certificate(s) from" << fileName;
  }
  else if (kind == "client")
  {
    QSslCertificate cert;
    QSslKey key;
    Pkcs12::Result result = Pkcs12::readClientIdentity(data, pass, cert, key);
    if (result != Pkcs12::Result::Success)
      return failure(reasonFor(result));
    // It could never be used: sending it crashes, and without it the server refuses the connection
    if (isUnusableClientCert(cert))
    {
      qWarning() << "CertificateComponent: rejected client certificate without CN from" << fileName
                 << "(TLS backend" << QSslSocket::activeBackend() << ", Qt" << qVersion() << ")";
      return failure("MISSING_COMMON_NAME");
    }

    if (!writeFile(pending.cert(), cert.toPem()) ||
        !writeFile(pending.key(), key.toPem(), QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    {
      QFile::remove(pending.cert());
      QFile::remove(pending.key());
      return failure("WRITE_ERROR");
    }
    setName(pending, "client", fileName);
    qInfo() << "CertificateComponent: staged client certificate from" << fileName;
  }
  else
  {
    return failure("INVALID_DATA");
  }

  refreshClientStore();
  return res;
}

/////////////////////////////////////////////////////////////////////////////////////////
QVariantMap CertificateComponent::info(const QString& url) const
{
  QUrl target = normalizeUrl(url);
  CertFolder folder{url.isEmpty() ? pendingDir() : serverDir(target)};
  QJsonObject names = readNames(folder);
  bool hasCa = folder.hasCa();
  bool hasClient = folder.hasClient();

  return {
    {"https", url.isEmpty() || target.scheme() == "https"},
    {"hasCa", hasCa},
    {"caFileName", hasCa ? names.value("ca").toString() : QString()},
    {"hasClient", hasClient},
    {"clientFileName", hasClient ? names.value("client").toString() : QString()}
  };
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::remove(const QString& url, const QString& kind)
{
  CertFolder folder{url.isEmpty() ? pendingDir() : serverDir(normalizeUrl(url))};
  bool ok = true;

  if (kind == "all")
  {
    ok = QDir(folder.dir).removeRecursively();
  }
  else if (kind == "ca")
  {
    QFile::remove(folder.ca());
    setName(folder, "ca", QString());
  }
  else if (kind == "client")
  {
    QFile::remove(folder.cert());
    QFile::remove(folder.key());
    setName(folder, "client", QString());
  }
  else
  {
    return false;
  }

  refreshClientStore();
  return ok;
}

/////////////////////////////////////////////////////////////////////////////////////////
void CertificateComponent::clearPending()
{
  QDir(pendingDir()).removeRecursively();
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::commit(const QString& url)
{
  const CertFolder from{pendingDir()};
  const bool hasCa = from.hasCa();
  const bool hasClient = from.hasClient();
  if (!hasCa && !hasClient)
    return true;

  QUrl target = normalizeUrl(url);
  if (target.scheme() != "https" || target.host().isEmpty())
  {
    qWarning() << "CertificateComponent: certificates require https, discarding pending for" << url;
    clearPending();
    refreshClientStore();
    return false;
  }

  const CertFolder to{serverDir(target)};
  QDir().mkpath(to.dir);
  QJsonObject names = readNames(from);
  bool ok = true;

  if (hasCa)
  {
    ok &= replaceFile(from.ca(), to.ca());
    setName(to, "ca", names.value("ca").toString());
  }
  if (hasClient)
  {
    ok &= replaceFile(from.cert(), to.cert());
    ok &= replaceFile(from.key(), to.key());
    QFile::setPermissions(to.key(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    setName(to, "client", names.value("client").toString());
  }

  clearPending();
  refreshClientStore();
  qInfo() << "CertificateComponent: committed certificates to" << to.dir << (ok ? "" : "(with errors)");
  return ok;
}

/////////////////////////////////////////////////////////////////////////////////////////
void CertificateComponent::openOverlay(const QString& mode, const QString& serverUrl, const QString& serverName)
{
  clearPending();

  QUrlQuery query;
  query.addQueryItem("mode", mode);
  if (!serverUrl.isEmpty())
    query.addQueryItem("server", serverUrl);
  if (!serverName.isEmpty())
    query.addQueryItem("name", serverName);

  QUrl url("qrc:///web-client/extension/find-webclient.html");
  url.setQuery(query);
  emit overlayRequested(url.toString());
}

/////////////////////////////////////////////////////////////////////////////////////////
void CertificateComponent::closeOverlay(const QString& resultUrl)
{
  clearPending();
  refreshClientStore();
  emit overlayClosed(resultUrl);
}

/////////////////////////////////////////////////////////////////////////////////////////
bool CertificateComponent::applyTo(QSslConfiguration& config, const QUrl& url, bool includePending) const
{
  CertPaths paths = certPaths(url, includePending);
  bool applied = false;

  QList<QSslCertificate> customCas = paths.hasCa() ? QSslCertificate::fromPath(paths.ca, QSsl::Pem)
                                                  : QList<QSslCertificate>();
  if (!customCas.isEmpty())
  {
    config.setCaCertificates(config.caCertificates() + customCas);
    applied = true;
  }

  QSslCertificate cert;
  QSslKey key;
  if (paths.hasClient() && loadIdentity(paths.cert, paths.key, cert, key) && !isUnusableClientCert(cert))
  {
    config.setLocalCertificate(cert);
    config.setPrivateKey(key);
    applied = true;
  }
  return applied;
}

/////////////////////////////////////////////////////////////////////////////////////////
void CertificateComponent::setProfile(QObject* profile)
{
  m_profile = profile;
  refreshClientStore();
}

/////////////////////////////////////////////////////////////////////////////////////////
void CertificateComponent::refreshClientStore()
{
  auto* profile = qobject_cast<QQuickWebEngineProfile*>(m_profile.data());
  QWebEngineClientCertificateStore* store = profile ? profile->clientCertificateStore() : nullptr;
  if (!store)
    return;

  store->clear();

  QList<CertFolder> folders = {CertFolder{pendingDir()}};
  QDir root(rootDir());
  for (const QString& sub : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    folders << CertFolder{root.filePath(sub)};

  for (const CertFolder& folder : folders)
  {
    QSslCertificate cert;
    QSslKey key;
    if (loadIdentity(folder.cert(), folder.key(), cert, key))
      store->add(cert, key);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
int CertificateComponent::matchClientCert(const QVariantList& certificates, const QString& host) const
{
  CertPaths paths = certPaths(normalizeUrl(host), true);
  const QSslCertificate cert = QSslCertificate::fromPath(paths.cert, QSsl::Pem).value(0);
  if (!paths.hasClient() || cert.isNull())
    return -1;

  for (int i = 0; i < certificates.size(); ++i)
  {
    // QML hands over ClientCertificateOption objects, which only expose these properties
    if (QObject* option = certificates[i].value<QObject*>())
    {
      if (option->property("subject").toString() == cert.subjectDisplayName() &&
          option->property("issuer").toString() == cert.issuerDisplayName() &&
          option->property("effectiveDate").toDateTime() == cert.effectiveDate() &&
          option->property("expiryDate").toDateTime() == cert.expiryDate())
        return i;
    }
    else if (certificates[i].value<QSslCertificate>() == cert)
    {
      return i;
    }
  }
  return -1;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Chromium does not know our custom CAs. When it reports an untrusted authority for a
// server that has a custom CA, verify the server ourselves and accept only if it passes.
void CertificateComponent::handleCertificateError(const QVariant& certificateError)
{
  if (!certificateError.canConvert<QWebEngineCertificateError>())
    return;

  QWebEngineCertificateError error = certificateError.value<QWebEngineCertificateError>();
  if (error.type() != QWebEngineCertificateError::CertificateAuthorityInvalid ||
      error.certificateChain().isEmpty() || !certPaths(error.url(), true).hasCa())
  {
    error.rejectCertificate();
    return;
  }

  error.defer();
  verifyServer(error.url(), error.certificateChain().first(), [error](bool trusted) mutable {
    if (trusted)
      error.acceptCertificate();
    else
      error.rejectCertificate();
  });
}

/////////////////////////////////////////////////////////////////////////////////////////
// Connect to the server with Qt's TLS stack using the custom CA of url, so the chain,
// expiry and host name are checked, and make sure it presents the expected certificate.
void CertificateComponent::verifyServer(const QUrl& url, const QSslCertificate& expected,
                                        std::function<void(bool)> done)
{
  QSslConfiguration config = QSslConfiguration::defaultConfiguration();
  if (!applyTo(config, url, true))
  {
    done(false);
    return;
  }

  auto* socket = new QSslSocket(this);
  socket->setSslConfiguration(config);

  auto finish = [socket, done, url](bool trusted) {
    if (socket->property("done").toBool())
      return;
    socket->setProperty("done", true);
    qInfo() << "CertificateComponent: custom CA verification for" << url.host()
            << (trusted ? "passed" : "failed");
    done(trusted);
    socket->abort();
    socket->deleteLater();
  };

  connect(socket, &QSslSocket::encrypted, this, [socket, expected, finish]() {
    finish(socket->peerCertificate() == expected);
  });
  connect(socket, &QSslSocket::sslErrors, this, [finish](const QList<QSslError>& errors) {
    qWarning() << "CertificateComponent: custom CA verification errors:" << errors;
    finish(false);
  });
  connect(socket, &QSslSocket::errorOccurred, this, [finish]() { finish(false); });
  QTimer::singleShot(VERIFY_TIMEOUT_MS, socket, [finish]() { finish(false); });

  socket->connectToHostEncrypted(url.host(), effectivePort(url));
}
