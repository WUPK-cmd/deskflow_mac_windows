/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/VersionChecker.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <cstring>
#include <utility>

namespace {

// Supply the HTTP boundary only; response parsing, ordering, signals and
// deferred reply ownership all run through the production checker.
class VersionReply : public QNetworkReply
{
public:
  VersionReply(QByteArray body, int status, NetworkError error) : m_body(std::move(body))
  {
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (error != NoError)
      setError(error, QStringLiteral("test HTTP error"));
    open(QIODevice::ReadOnly);
    setFinished(true);
  }

  void abort() override
  {
  }

protected:
  qint64 readData(char *data, qint64 maxSize) override
  {
    const auto count = std::min(maxSize, static_cast<qint64>(m_body.size()) - m_offset);
    if (count <= 0)
      return -1;
    std::memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(count));
    m_offset += count;
    return count;
  }

private:
  QByteArray m_body;
  qint64 m_offset = 0;
};

} // namespace

class VersionCheckerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void compareVersions_data();
  void compareVersions();
  void responses_data();
  void responses();
};

void VersionCheckerTests::compareVersions_data()
{
  QTest::addColumn<QString>("current");
  QTest::addColumn<QString>("available");
  QTest::addColumn<int>("expected");

  QTest::newRow("release-candidate-number") << "1.27.0-rc2" << "1.27.0-rc10" << 1;
  QTest::newRow("higher-major-lower-minor") << "1.99.99" << "2.0.0" << 1;
  QTest::newRow("lower-major-higher-minor") << "2.0.0" << "1.99.99" << -1;
  QTest::newRow("lower-minor-higher-patch") << "1.28.0" << "1.27.99" << -1;
  QTest::newRow("stable-after-rc") << "1.27.0-rc9" << "1.27.0" << 1;
  QTest::newRow("rc-after-beta") << "1.27.0-beta9" << "1.27.0-rc1" << 1;
  QTest::newRow("four-component-build") << "1.27.0.12" << "1.27.0.13" << 1;
  QTest::newRow("tag-prefix") << "1.27.0" << "v1.27.0" << 0;
  QTest::newRow("stable-alias") << "1.27.0-stable" << "1.27.0" << 0;
  QTest::newRow("missing-build-zero") << "1.27.0.0" << "1.27.0" << 0;
  QTest::newRow("build-metadata") << "1.27.0+abc" << "1.27.0+def" << 0;
  QTest::newRow("short-current") << "1" << "2.0.0" << 0;
  QTest::newRow("short-available") << "1.27.0" << "2" << 0;
  QTest::newRow("invalid-number") << "1.27.0" << "1.no.1" << 0;
  QTest::newRow("overflow-number") << "1.27.0" << "999999999999999999999.0.0" << 0;
  QTest::newRow("extra-component") << "1.27.0" << "1.27.0.1.2" << 0;
  QTest::newRow("unknown-stage") << "1.27.0" << "9999.0.0-nonsense" << 0;
}

void VersionCheckerTests::compareVersions()
{
  QFETCH(QString, current);
  QFETCH(QString, available);
  QFETCH(int, expected);
  QCOMPARE(VersionChecker::compareVersions(current, available), expected);
}

void VersionCheckerTests::responses_data()
{
  QTest::addColumn<QByteArray>("body");
  QTest::addColumn<int>("status");
  QTest::addColumn<int>("error");
  QTest::addColumn<QString>("expected");

  QTest::newRow("github-release") << QByteArray(R"({"tag_name":"v9999.0.0","draft":false,"prerelease":false})") << 200
                                  << int(QNetworkReply::NoError) << "9999.0.0";
  QTest::newRow("plain-version") << QByteArray("  v9999.0.0\n") << 200 << int(QNetworkReply::NoError) << "9999.0.0";
  QTest::newRow("no-release-404") << QByteArray(R"({"message":"Not Found"})") << 404
                                  << int(QNetworkReply::ContentNotFoundError) << "";
  QTest::newRow("status-without-network-error") << QByteArray("9999.0.0") << 404 << int(QNetworkReply::NoError) << "";
  QTest::newRow("transport-error") << QByteArray("9999.0.0") << 200 << int(QNetworkReply::TimeoutError) << "";
  QTest::newRow("empty") << QByteArray() << 200 << int(QNetworkReply::NoError) << "";
  QTest::newRow("short") << QByteArray("9") << 200 << int(QNetworkReply::NoError) << "";
  QTest::newRow("html-error") << QByteArray("<html>upstream unavailable</html>") << 200 << int(QNetworkReply::NoError)
                              << "";
  QTest::newRow("malformed-json") << QByteArray(R"({"tag_name":)") << 200 << int(QNetworkReply::NoError) << "";
  QTest::newRow("missing-tag") << QByteArray(R"({"message":"API rate limit exceeded"})") << 200
                               << int(QNetworkReply::NoError) << "";
  QTest::newRow("non-string-tag") << QByteArray(R"({"tag_name":9999})") << 200 << int(QNetworkReply::NoError) << "";
  QTest::newRow("invalid-tag") << QByteArray(R"({"tag_name":"continuous"})") << 200 << int(QNetworkReply::NoError)
                               << "";
  QTest::newRow("draft-release") << QByteArray(R"({"tag_name":"v9999.0.0","draft":true})") << 200
                                 << int(QNetworkReply::NoError) << "";
  QTest::newRow("prerelease") << QByteArray(R"({"tag_name":"v9999.0.0-rc1","prerelease":true})") << 200
                              << int(QNetworkReply::NoError) << "";
  QTest::newRow("older-release") << QByteArray(R"({"tag_name":"v0.0.1"})") << 200 << int(QNetworkReply::NoError) << "";
}

void VersionCheckerTests::responses()
{
  QFETCH(QByteArray, body);
  QFETCH(int, status);
  QFETCH(int, error);
  QFETCH(QString, expected);

  VersionChecker checker;
  QSignalSpy updates(&checker, &VersionChecker::updateFound);
  auto *reply = new VersionReply(body, status, static_cast<QNetworkReply::NetworkError>(error));
  QPointer<QNetworkReply> alive(reply);
  checker.replyFinished(reply);

  QCOMPARE(updates.count(), expected.isEmpty() ? 0 : 1);
  if (!expected.isEmpty())
    QCOMPARE(updates.first().first().toString(), expected);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY2(alive.isNull(), "Every HTTP response, including failures, must release its reply");
}

QTEST_GUILESS_MAIN(VersionCheckerTests)
#include "VersionCheckerTests.moc"
