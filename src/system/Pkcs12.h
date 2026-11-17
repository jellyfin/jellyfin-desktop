#ifndef PKCS12_H
#define PKCS12_H

#include <QByteArray>
#include <QList>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

namespace Pkcs12
{
  enum class Result
  {
    Success,
    PasswordRequired,
    InvalidPassword,
    InvalidData,
    // A PKCS#12 file that no reader on this system could open with the given password
    Unsupported
  };

  // True when data looks like a DER encoded PKCS#12 (PFX) container.
  bool isPkcs12Data(const QByteArray& data);

  // Reads CA certificates from PEM, DER or PKCS#12 data (all certificates in the container).
  Result readCertificates(const QByteArray& data, const QByteArray& passPhrase, QList<QSslCertificate>& out);

  // Reads the client certificate and private key from PKCS#12 data. CA bags are ignored.
  Result readClientIdentity(const QByteArray& data, const QByteArray& passPhrase,
                            QSslCertificate& cert, QSslKey& key);

  // Loads an RSA, EC or DSA private key from a PEM file.
  QSslKey loadPrivateKey(const QString& path);

  // QTBUG-142324: the Schannel backend in Qt < 6.10.2 crashes on client certificates without a CN.
  bool isSchannelMissingCnBugPresent();
}

#endif // PKCS12_H
