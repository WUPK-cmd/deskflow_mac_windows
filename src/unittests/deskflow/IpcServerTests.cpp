/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"
#include "gui/ipc/IpcClient.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

namespace {

class TestServer : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestServer(const QString &name) : IpcServer(nullptr, name, QStringLiteral("test"))
  {
  }

  QList<QStringList> messages;

private:
  void processCommand(QLocalSocket *, const QString &, const QStringList &parts) override
  {
    messages.append(parts);
  }
};

} // namespace

class IpcServerTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void retainsPartialLineAfterHandshake();
  void rejectsCommandBeforeHandshake();
  void cancelledClientStopsRetrying();
  void clientRejectsOversizedLine();

private:
  Log m_log;
};

void IpcServerTests::retainsPartialLineAfterHandshake()
{
  const auto name = QStringLiteral("deskflow-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  TestServer server(name);
  server.listen();

  QLocalSocket client;
  client.connectToServer(name);
  QTRY_COMPARE(client.state(), QLocalSocket::ConnectedState);
  const auto hello = QStringLiteral("hello=%1\n").arg(kIpcVersionId).toUtf8();
  client.write(hello + "command=par");
  QTRY_VERIFY(client.canReadLine());
  QCOMPARE(client.readLine(), hello);
  QVERIFY(server.messages.isEmpty());

  client.write("tial=value\nnext\n");
  QTRY_COMPARE(server.messages.size(), 2);
  QCOMPARE(server.messages.at(0), (QStringList{QStringLiteral("command"), QStringLiteral("partial=value")}));
  QCOMPARE(server.messages.at(1), QStringList{QStringLiteral("next")});
}

void IpcServerTests::rejectsCommandBeforeHandshake()
{
  const auto name = QStringLiteral("deskflow-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  TestServer server(name);
  server.listen();

  QLocalSocket client;
  client.connectToServer(name);
  QTRY_COMPARE(client.state(), QLocalSocket::ConnectedState);
  client.write("stop\n");
  QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
  QVERIFY(server.messages.isEmpty());
}

void IpcServerTests::cancelledClientStopsRetrying()
{
  const auto name = QStringLiteral("deskflow-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  deskflow::gui::ipc::IpcClient client(nullptr, name, QStringLiteral("test"));
  client.connectToServer();
  client.disconnectFromServer();

  QLocalServer server;
  QVERIFY(server.listen(name));
  // Give a previously scheduled retry time to run.
  QTest::qWait(650);
  QVERIFY(!server.hasPendingConnections());

  client.connectToServer();
  QTRY_VERIFY(server.hasPendingConnections());
}

void IpcServerTests::clientRejectsOversizedLine()
{
  const auto name = QStringLiteral("deskflow-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  QLocalServer server;
  QVERIFY(server.listen(name));

  deskflow::gui::ipc::IpcClient client(nullptr, name, QStringLiteral("test"));
  QSignalSpy failed(&client, &deskflow::gui::ipc::IpcClient::connectionFailed);
  client.connectToServer();
  QTRY_VERIFY(server.hasPendingConnections());
  auto *peer = server.nextPendingConnection();
  peer->write(QByteArray(kMaxIpcMessageSize + 1, 'x'));
  QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
}

QTEST_GUILESS_MAIN(IpcServerTests)
#include "IpcServerTests.moc"
