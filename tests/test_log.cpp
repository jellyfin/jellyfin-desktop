#include <QtTest/QtTest>
#include "../src/utils/Log.h"

class TestLog : public QObject
{
  Q_OBJECT

private slots:
  // ParseLogLevel tests
  void testParseLogLevel_data();
  void testParseLogLevel();

  // CensorAuthTokens tests
  void testCensorAuthTokens_data();
  void testCensorAuthTokens();
};

///////////////////////////////////////////////////////////////////////////////
// ParseLogLevel
///////////////////////////////////////////////////////////////////////////////

void TestLog::testParseLogLevel_data()
{
  QTest::addColumn<QString>("input");
  QTest::addColumn<int>("expected");

  // Log levels.
  QTest::newRow("debug") << "debug" << 0;
  QTest::newRow("info")  << "info"  << 1;
  QTest::newRow("warn")  << "warn"  << 2;
  QTest::newRow("error") << "error" << 3;
  QTest::newRow("fatal") << "fatal" << 4;

  // Invalid log levels.
  QTest::newRow("empty string")    << ""         << -1;
  QTest::newRow("unknown level")   << "whoohoo"  << -1;
  QTest::newRow("numeric")         << "0"        << -1;
  QTest::newRow("uppercase DEBUG") << "DEBUG"    << -1;
  QTest::newRow("mixed case Info") << "InFo"     << -1;
  QTest::newRow("warning")         << "warning"  << -1;
  QTest::newRow("critical")        << "critical" << -1;
  QTest::newRow("space before")    << " debug"   << -1;
  QTest::newRow("trailing space")  << "debug "   << -1;
}

void TestLog::testParseLogLevel()
{
  QFETCH(QString, input);
  QFETCH(int, expected);

  QCOMPARE(Log::ParseLogLevel(input), expected);
}

///////////////////////////////////////////////////////////////////////////////
// CensorAuthTokens
///////////////////////////////////////////////////////////////////////////////

void TestLog::testCensorAuthTokens_data()
{
  QTest::addColumn<QString>("input");
  QTest::addColumn<QString>("expected");

  const QString masked32 = QString(32, 'x');

  // Base case: No tokens in string
  QTest::newRow("no token")
  << "GET /Items?UserId=abc123"
  << "GET /Items?UserId=abc123";

  // Base case: Empty string
  QTest::newRow("empty string")
  << ""
  << "";

  // API key
  QTest::newRow("api_key")
    << "url?api_key=abcdef0123456789abcdef0123456789&other=1"
    << "url?api_key=" + masked32 + "&other=1";

  // X-MediaBrowser-Token, URL-encoded
  QTest::newRow("X-MediaBrowser-Token URL-encoded")
    << "header=X-MediaBrowser-Token%3Dabcdef0123456789abcdef0123456789done"
    << "header=X-MediaBrowser-Token%3D" + masked32 + "done";

  // X-MediaBrowser-Token, plain
  QTest::newRow("X-MediaBrowser-Token plain")
    << "X-MediaBrowser-Token=abcdef0123456789abcdef0123456789end"
    << "X-MediaBrowser-Token=" + masked32 + "end";

  // ApiKey= pattern
  QTest::newRow("ApiKey")
    << "ApiKey=abcdef0123456789abcdef0123456789rest"
    << "ApiKey=" + masked32 + "rest";

  // AccessToken= pattern
  QTest::newRow("AccessToken=")
    << "AccessToken=abcdef0123456789abcdef0123456789end"
    << "AccessToken=" + masked32 + "end";

  // AccessToken":" pattern (appears to be JSON formatted)
  QTest::newRow("AccessToken JSON")
    << R"({"AccessToken":"abcdef0123456789abcdef0123456789"})"
    << R"({"AccessToken":")" + masked32 + R"("})";

  // Multiple tokens
  QTest::newRow("two api_key tokens")
    << "url?api_key=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&redirect=url2?api_key=BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
    << "url?api_key=" + masked32 + "&redirect=url2?api_key=" + masked32;

  QTest::newRow("mixed token types")
    << "api_key=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA and AccessToken=BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
    << "api_key=" + masked32 + " and AccessToken=" + masked32;

  // Truncated token
  QTest::newRow("api_key truncated at end")
    << "api_key=short"
    << "api_key=short";

  QTest::newRow("api_key exactly 32 chars at end")
    << "api_key=abcdef0123456789abcdef0123456789"
    << "api_key=" + masked32;

  // Not quite an API key
  QTest::newRow("partial pattern match")
    << "not_an_api_key=something"
    << "not_an_api_key=something";

  // Token with multiple parameters
  QTest::newRow("token in log line")
    << "2024-01-01 12:00:00 [info] Request GET /Items?api_key=abcdef0123456789abcdef0123456789&format=json"
    << "2024-01-01 12:00:00 [info] Request GET /Items?api_key=" + masked32 + "&format=json";
}

void TestLog::testCensorAuthTokens()
{
  QFETCH(QString, input);
  QFETCH(QString, expected);

  QString msg = input;
  Log::CensorAuthTokens(msg);
  QCOMPARE(msg, expected);
}

QTEST_APPLESS_MAIN(TestLog)
#include "test_log.moc"
