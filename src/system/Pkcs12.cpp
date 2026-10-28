// Reads certificates and private keys from PEM, DER and PKCS#12 data.
//
// PKCS#12 files are read with Qt's importPkcs12 first. It cannot read CA-only containers
// or every cipher, so two fallbacks follow:
//  - the platform API (Windows CryptoAPI, macOS Security framework) for CA bundles
//  - libcrypto, loaded at runtime, for PBES2/AES, legacy ciphers and CA-only files
//
// OpenSSL is not linked. When the first libcrypto found cannot read a file, the other
// installed ones are tried.
#include "Pkcs12.h"

#include <QBuffer>
#include <QDebug>
#include <QFile>
#include <QLibrary>
#include <QMutex>
#include <QSslSocket>
#include <QVersionNumber>

#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

#if defined(Q_OS_DARWIN)
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

using Pkcs12::Result;

namespace
{

// Opaque OpenSSL types
struct OsslBio;
struct OsslPkcs12;
struct OsslX509;
struct OsslPkey;
struct OsslStack;

constexpr int BIO_CTRL_INFO = 3;

/////////////////////////////////////////////////////////////////////////////////////////
// Loads the first loadable libcrypto from position next on, and moves next past it
QLibrary* loadCryptoLibrary(int& next)
{
  static const QStringList names = {
#if defined(Q_OS_DARWIN)
    // Never probe an unversioned "libcrypto": /usr/lib/libcrypto.dylib aborts the process
    "/usr/lib/libcrypto.46.dylib", "/usr/lib/libcrypto.44.dylib", "/usr/lib/libcrypto.42.dylib",
    "/usr/lib/libcrypto.41.dylib", "/usr/lib/libcrypto.35.dylib", "libcrypto.3", "libcrypto.1.1",
    "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib", "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib",
    "/opt/local/lib/libcrypto.3.dylib"
#elif defined(Q_OS_WIN)
    "libcrypto-3-x64", "libcrypto-1_1-x64", "libcrypto-3", "libcrypto"
#else
    "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"
#endif
  };

  while (next < names.size())
  {
    auto* lib = new QLibrary(names[next++]);
    if (lib->load())
    {
      // OpenSSL 3 needs the legacy provider for RC2/3DES files. This also lets Qt's own
      // importPkcs12 read them, as Qt's OpenSSL backend uses the same library.
      using LoadProvider = void* (*)(void*, const char*);
      if (auto load = reinterpret_cast<LoadProvider>(lib->resolve("OSSL_PROVIDER_load")))
      {
        load(nullptr, "default");
        load(nullptr, "legacy");
      }
      return lib;
    }
    delete lib;
  }
  return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////
struct Crypto
{
  OsslBio* (*BIO_new_mem_buf)(const void*, int);
  OsslBio* (*BIO_new)(const void*);
  const void* (*BIO_s_mem)();
  long (*BIO_ctrl)(OsslBio*, int, long, void*);
  int (*BIO_free)(OsslBio*);
  OsslPkcs12* (*d2i_PKCS12_bio)(OsslBio*, OsslPkcs12**);
  int (*PKCS12_parse)(OsslPkcs12*, const char*, OsslPkey**, OsslX509**, OsslStack**);
  void (*PKCS12_free)(OsslPkcs12*);
  int (*PEM_write_bio_PrivateKey)(OsslBio*, OsslPkey*, const void*, const unsigned char*, int, void*, void*);
  int (*i2d_X509)(OsslX509*, unsigned char**);
  void (*X509_free)(OsslX509*);
  void (*EVP_PKEY_free)(OsslPkey*);
  int (*sk_num)(const OsslStack*);
  void* (*sk_value)(const OsslStack*, int);
  void (*sk_pop_free)(OsslStack*, void (*)(void*));
  void (*CRYPTO_free)(void*, const char*, int);
  // Optional
  unsigned char* (*OPENSSL_asc2uni)(const char*, int, unsigned char**, int*);
  char* (*OPENSSL_uni2utf8)(const unsigned char*, int);
  void (*ERR_clear_error)();
};

/////////////////////////////////////////////////////////////////////////////////////////
std::optional<Crypto> resolveCrypto(QLibrary* lib)
{
  auto resolve = [lib](auto& fn, std::initializer_list<const char*> symbols) {
    for (const char* symbol : symbols)
    {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib->resolve(symbol));
      if (fn)
        return true;
    }
    return false;
  };

  Crypto c{};
  bool ok = resolve(c.BIO_new_mem_buf, {"BIO_new_mem_buf"}) &&
            resolve(c.BIO_new, {"BIO_new"}) &&
            resolve(c.BIO_s_mem, {"BIO_s_mem"}) &&
            resolve(c.BIO_ctrl, {"BIO_ctrl"}) &&
            resolve(c.BIO_free, {"BIO_free"}) &&
            resolve(c.d2i_PKCS12_bio, {"d2i_PKCS12_bio"}) &&
            resolve(c.PKCS12_parse, {"PKCS12_parse"}) &&
            resolve(c.PKCS12_free, {"PKCS12_free"}) &&
            resolve(c.PEM_write_bio_PrivateKey, {"PEM_write_bio_PrivateKey"}) &&
            resolve(c.i2d_X509, {"i2d_X509"}) &&
            resolve(c.X509_free, {"X509_free"}) &&
            resolve(c.EVP_PKEY_free, {"EVP_PKEY_free"}) &&
            resolve(c.sk_num, {"OPENSSL_sk_num", "sk_num"}) &&
            resolve(c.sk_value, {"OPENSSL_sk_value", "sk_value"}) &&
            resolve(c.sk_pop_free, {"OPENSSL_sk_pop_free", "sk_pop_free"}) &&
            resolve(c.CRYPTO_free, {"CRYPTO_free"});
  if (!ok)
  {
    qWarning() << "Pkcs12: libcrypto" << lib->fileName() << "lacks required functions";
    return std::nullopt;
  }
  resolve(c.OPENSSL_asc2uni, {"OPENSSL_asc2uni"});
  resolve(c.OPENSSL_uni2utf8, {"OPENSSL_uni2utf8"});
  resolve(c.ERR_clear_error, {"ERR_clear_error"});
  return c;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Functions of the index-th usable libcrypto in order of preference, or nullptr when
// there are fewer. Libraries are only loaded once needed.
const Crypto* crypto(int index = 0)
{
  static QMutex s_mutex;
  static std::deque<Crypto> s_libs; // deque: pointers stay valid as it grows
  static int s_next = 0;

  QMutexLocker locker(&s_mutex);
  while (index >= static_cast<int>(s_libs.size()))
  {
    QLibrary* lib = loadCryptoLibrary(s_next);
    if (!lib)
      return nullptr;

    // Several names can resolve to the same library
    std::optional<Crypto> c = resolveCrypto(lib);
    if (c && std::none_of(s_libs.begin(), s_libs.end(),
                          [&c](const Crypto& l) { return l.PKCS12_parse == c->PKCS12_parse; }))
      s_libs.push_back(*c);
  }
  return &s_libs[index];
}

/////////////////////////////////////////////////////////////////////////////////////////
QSslKey keyFromPem(const QByteArray& pem)
{
  for (QSsl::KeyAlgorithm alg : {QSsl::Rsa, QSsl::Ec, QSsl::Dsa})
  {
    QSslKey key(pem, alg, QSsl::Pem, QSsl::PrivateKey);
    if (!key.isNull())
      return key;
  }
  return QSslKey();
}

/////////////////////////////////////////////////////////////////////////////////////////
QSslCertificate toQt(const Crypto& c, OsslX509* x509)
{
  unsigned char* der = nullptr;
  int len = c.i2d_X509(x509, &der);
  if (len <= 0 || !der)
    return QSslCertificate();
  QSslCertificate cert(QByteArray(reinterpret_cast<const char*>(der), len), QSsl::Der);
  c.CRYPTO_free(der, "", 0);
  return cert;
}

/////////////////////////////////////////////////////////////////////////////////////////
QSslKey toQt(const Crypto& c, OsslPkey* pkey)
{
  OsslBio* bio = c.BIO_new(c.BIO_s_mem());
  if (!bio)
    return QSslKey();

  QSslKey key;
  char* data = nullptr;
  if (c.PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1)
  {
    long len = c.BIO_ctrl(bio, BIO_CTRL_INFO, 0, &data);
    if (len > 0 && data)
    {
      QByteArray pem(data, static_cast<qsizetype>(len));
      key = keyFromPem(pem);
      // Don't leave the unencrypted key behind in freed memory
      pem.fill('\0');
      std::memset(data, 0, static_cast<size_t>(len));
    }
  }
  c.BIO_free(bio);
  return key;
}

/////////////////////////////////////////////////////////////////////////////////////////
struct Parsed
{
  Result status = Result::InvalidData;
  QSslCertificate leaf;
  QSslKey key;
  QList<QSslCertificate> extra;
};

/////////////////////////////////////////////////////////////////////////////////////////
Parsed parseWithOpenSsl(const Crypto& c, const QByteArray& data, const QByteArray& pass)
{
  Parsed out;
  auto clearErrors = [&c]() {
    // libcrypto is shared with Qt's TLS backend, don't leave our errors in its queue
    if (c.ERR_clear_error)
      c.ERR_clear_error();
  };

  OsslPkcs12* p12 = nullptr;
  if (OsslBio* bio = c.BIO_new_mem_buf(data.constData(), static_cast<int>(data.size())))
  {
    p12 = c.d2i_PKCS12_bio(bio, nullptr);
    c.BIO_free(bio);
  }
  if (!p12)
  {
    clearErrors();
    return out; // not a PKCS#12 container
  }

  OsslPkey* pkey = nullptr;
  OsslX509* x509 = nullptr;
  OsslStack* cas = nullptr;
  bool ok = c.PKCS12_parse(p12, pass.constData(), &pkey, &x509, &cas) == 1;

  if (!ok && !pass.isEmpty() && c.OPENSSL_asc2uni && c.OPENSSL_uni2utf8)
  {
    // Files written by older OpenSSL/LibreSSL used the raw password bytes instead of
    // decoding them as UTF-8. Re-encode the bytes so OpenSSL 3 derives the same key.
    unsigned char* uni = nullptr;
    int uniLen = 0;
    c.OPENSSL_asc2uni(pass.constData(), static_cast<int>(pass.size()), &uni, &uniLen);
    if (uni)
    {
      if (char* legacy = c.OPENSSL_uni2utf8(uni, uniLen))
      {
        ok = c.PKCS12_parse(p12, legacy, &pkey, &x509, &cas) == 1;
        c.CRYPTO_free(legacy, "", 0);
      }
      c.CRYPTO_free(uni, "", 0);
    }
  }
  c.PKCS12_free(p12);

  if (ok)
  {
    out.status = Result::Success;
    if (x509)
      out.leaf = toQt(c, x509);
    if (pkey)
      out.key = toQt(c, pkey);
    for (int i = 0; cas && i < c.sk_num(cas); ++i)
      out.extra << toQt(c, static_cast<OsslX509*>(c.sk_value(cas, i)));
  }
  else
  {
    out.status = pass.isEmpty() ? Result::PasswordRequired : Result::InvalidPassword;
  }

  if (x509)
    c.X509_free(x509);
  if (pkey)
    c.EVP_PKEY_free(pkey);
  if (cas)
    c.sk_pop_free(cas, reinterpret_cast<void (*)(void*)>(c.X509_free));
  clearErrors();
  return out;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Parses with each available libcrypto until one succeeds, as the preferred one cannot
// read every file (the LibreSSL macOS ships rejects files without a MAC). Returns the
// first library's failure, or std::nullopt when there is no libcrypto.
std::optional<Parsed> parseWithAnyOpenSsl(const QByteArray& data, const QByteArray& pass)
{
  std::optional<Parsed> first;
  for (int i = 0; const Crypto* c = crypto(i); ++i)
  {
    Parsed parsed = parseWithOpenSsl(*c, data, pass);
    if (parsed.status == Result::Success)
      return parsed;
    if (!first)
      first = parsed;
  }
  return first;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Certificates of a PKCS#12 file read with the operating system's API, used for CA bundles.
// std::nullopt when the platform has no suitable API.
std::optional<Parsed> readNative(const QByteArray& data, const QByteArray& pass)
{
#if defined(Q_OS_WIN)
  CRYPT_DATA_BLOB blob;
  blob.cbData = static_cast<DWORD>(data.size());
  blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(data.constData()));

  HCERTSTORE store = nullptr;
  if (pass.isEmpty())
  {
    store = PFXImportCertStore(&blob, nullptr, PKCS12_NO_PERSIST_KEY);
    if (!store)
      store = PFXImportCertStore(&blob, L"", PKCS12_NO_PERSIST_KEY);
  }
  else
  {
    QString password = QString::fromUtf8(pass);
    store = PFXImportCertStore(&blob, reinterpret_cast<LPCWSTR>(password.utf16()), PKCS12_NO_PERSIST_KEY);
  }

  Parsed out;
  if (store)
  {
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)))
      out.extra << QSslCertificate(QByteArray(reinterpret_cast<const char*>(ctx->pbCertEncoded),
                                              static_cast<qsizetype>(ctx->cbCertEncoded)), QSsl::Der);
    CertCloseStore(store, 0);
    out.status = Result::Success;
  }
  else
  {
    DWORD err = GetLastError();
    if (err == ERROR_INVALID_PASSWORD || err == 0x80070056 || err == 0x8009000B)
      out.status = pass.isEmpty() ? Result::PasswordRequired : Result::InvalidPassword;
  }
  return out;
#elif defined(Q_OS_DARWIN) && defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
  // Older macOS versions import the items into the default keychain, so the API is only
  // used where it can be told to keep them in memory.
  if (!__builtin_available(macOS 15.0, *))
    return std::nullopt;

  CFDataRef cfData = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(data.constData()), data.size());
  CFStringRef cfPass = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(pass.constData()),
                                               pass.size(), kCFStringEncodingUTF8, false);
  CFMutableDictionaryRef options = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(options, kSecImportExportPassphrase, cfPass);
  CFDictionarySetValue(options, kSecImportToMemoryOnly, kCFBooleanTrue);

  Parsed out;
  CFArrayRef items = nullptr;
  OSStatus status = SecPKCS12Import(cfData, options, &items);
  CFRelease(options);
  CFRelease(cfPass);
  CFRelease(cfData);

  auto append = [&out](SecCertificateRef cert) {
    if (CFDataRef der = SecCertificateCopyData(cert))
    {
      out.extra << QSslCertificate(QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(der)),
                                              static_cast<qsizetype>(CFDataGetLength(der))), QSsl::Der);
      CFRelease(der);
    }
  };

  if (status == errSecSuccess && items)
  {
    for (CFIndex i = 0; i < CFArrayGetCount(items); ++i)
    {
      auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, i));
      if (auto chain = static_cast<CFArrayRef>(CFDictionaryGetValue(dict, kSecImportItemCertChain)))
      {
        for (CFIndex j = 0; j < CFArrayGetCount(chain); ++j)
          append(static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(chain, j))));
      }
      if (auto identity = static_cast<SecIdentityRef>(const_cast<void*>(CFDictionaryGetValue(dict, kSecImportItemIdentity))))
      {
        SecCertificateRef cert = nullptr;
        if (SecIdentityCopyCertificate(identity, &cert) == errSecSuccess && cert)
        {
          append(cert);
          CFRelease(cert);
        }
      }
    }
    out.status = Result::Success;
  }
  else if (status == errSecAuthFailed)
  {
    // Also returned for PBES2/AES files SecPKCS12Import cannot decrypt
    out.status = pass.isEmpty() ? Result::PasswordRequired : Result::InvalidPassword;
  }
  if (items)
    CFRelease(items);
  return out;
#else
  Q_UNUSED(data);
  Q_UNUSED(pass);
  return std::nullopt;
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
// Result for data no available reader could open
Result unreadable(const QByteArray& data, const QByteArray& pass)
{
  if (!Pkcs12::isPkcs12Data(data))
    return Result::InvalidData;
  // A protected file is the common case, but with a password given we cannot tell a
  // wrong one from a cipher nothing on this system can decrypt.
  return pass.isEmpty() ? Result::PasswordRequired : Result::Unsupported;
}

/////////////////////////////////////////////////////////////////////////////////////////
Result keepUnique(const QList<QSslCertificate>& certs, QList<QSslCertificate>& out)
{
  out.clear();
  for (const QSslCertificate& cert : certs)
  {
    if (!cert.isNull() && !out.contains(cert))
      out << cert;
  }
  return out.isEmpty() ? Result::InvalidData : Result::Success;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////////////////
bool Pkcs12::isPkcs12Data(const QByteArray& data)
{
  if (data.size() < 4 || static_cast<unsigned char>(data[0]) != 0x30) // ASN.1 SEQUENCE
    return false;

  // PKCS#12 OID (1.2.840.113549.1.12)
  static const QByteArray pkcs12Oid("\x2A\x86\x48\x86\xF7\x0D\x01\x0C", 8);
  if (data.contains(pkcs12Oid))
    return true;

  // With PBES2/AES the 1.12 OID is inside the ciphertext. The outer PFX is always
  // SEQUENCE { INTEGER 3, ContentInfo (1.2.840.113549.1.7.*) }.
  static const QByteArray pfxV3("\x02\x01\x03", 3);
  static const QByteArray pkcs7Oid("\x2A\x86\x48\x86\xF7\x0D\x01\x07", 8);
  qsizetype v3Idx = data.indexOf(pfxV3);
  return v3Idx > 0 && v3Idx <= 10 && data.contains(pkcs7Oid);
}

/////////////////////////////////////////////////////////////////////////////////////////
Result Pkcs12::readCertificates(const QByteArray& data, const QByteArray& passPhrase,
                                QList<QSslCertificate>& out)
{
  out.clear();
  if (data.isEmpty())
    return Result::InvalidData;

  // PEM or DER certificates
  QList<QSslCertificate> certs = QSslCertificate::fromData(data, QSsl::Pem);
  if (certs.isEmpty())
    certs = QSslCertificate::fromData(data, QSsl::Der);
  if (!certs.isEmpty())
    return keepUnique(certs, out);

  // PKCS#12: Qt first. crypto() also loads the legacy provider Qt may need.
  crypto();
  QBuffer buffer(const_cast<QByteArray*>(&data));
  buffer.open(QIODevice::ReadOnly);
  QSslKey key;
  QSslCertificate leaf;
  if (QSslCertificate::importPkcs12(&buffer, &key, &leaf, &certs, passPhrase))
  {
    certs.prepend(leaf);
    return keepUnique(certs, out);
  }

  // Qt only reads containers with a private key and the ciphers of its backend
  std::optional<Parsed> native = readNative(data, passPhrase);
  if (native && native->status == Result::Success)
    return keepUnique(native->extra, out);

  if (std::optional<Parsed> parsed = parseWithAnyOpenSsl(data, passPhrase))
  {
    if (parsed->status != Result::Success)
      return parsed->status;
    parsed->extra.prepend(parsed->leaf);
    return keepUnique(parsed->extra, out);
  }

  if (native && native->status != Result::InvalidData)
    return native->status;
  return unreadable(data, passPhrase);
}

/////////////////////////////////////////////////////////////////////////////////////////
Result Pkcs12::readClientIdentity(const QByteArray& data, const QByteArray& passPhrase,
                                  QSslCertificate& cert, QSslKey& key)
{
  cert = QSslCertificate();
  key = QSslKey();
  if (data.isEmpty())
    return Result::InvalidData;

  crypto(); // loads the legacy provider Qt may need
  QBuffer buffer(const_cast<QByteArray*>(&data));
  buffer.open(QIODevice::ReadOnly);

  // caCertificates is nullptr: CA certificates bundled in a client file are ignored
  if (QSslCertificate::importPkcs12(&buffer, &key, &cert, nullptr, passPhrase) && !cert.isNull() && !key.isNull())
    return Result::Success;
  cert = QSslCertificate();
  key = QSslKey();

  std::optional<Parsed> parsed = parseWithAnyOpenSsl(data, passPhrase);
  if (!parsed)
    return unreadable(data, passPhrase);
  if (parsed->status != Result::Success)
    return parsed->status;
  if (parsed->leaf.isNull() || parsed->key.isNull())
    return Result::InvalidData; // e.g. a CA bundle without a private key
  cert = parsed->leaf;
  key = parsed->key;
  return Result::Success;
}

/////////////////////////////////////////////////////////////////////////////////////////
QSslKey Pkcs12::loadPrivateKey(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return QSslKey();
  return keyFromPem(file.readAll());
}

/////////////////////////////////////////////////////////////////////////////////////////
bool Pkcs12::isSchannelMissingCnBugPresent()
{
#if defined(Q_OS_WIN)
  if (QSslSocket::activeBackend() != "schannel")
    return false;
  return QVersionNumber::fromString(QString::fromLatin1(qVersion())) < QVersionNumber(6, 10, 2);
#else
  return false;
#endif
}
