/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "common/VersionInfo.h"
#include "gui/core/CoreProcess.h"

#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <utility>

using deskflow::core::ConnectionState;
using deskflow::core::ProcessState;
using deskflow::gui::CoreProcess;

namespace {

QString endpoint()
{
  return QStringLiteral("deskflow-recovery-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

// In-process wire peers replace only OS process/service launch; the production
// GUI lifecycle and both IPC clients run unchanged against real local sockets.
class Peer : public QLocalServer
{
public:
  explicit Peer(const QString &name, bool core = false) : m_core(core)
  {
    connect(this, &QLocalServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto *socket = nextPendingConnection();
        sockets.append(socket);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
          while (socket->canReadLine()) {
            const auto line = socket->readLine().trimmed();
            commands.append(line);
            if (line.startsWith("hello=")) {
              socket->write(QByteArray("hello=") + kIpcVersionId + "\n");
              if (m_core)
                socket->write("connectionState=Connected\nconnectedClients=laptop\nsecureSocket=TLSv1.3\n");
            } else if (line.startsWith("start=") && replyToStart) {
              const auto id = line.split('=').at(1);
              socket->write("startResult=" + id + "=ok\n");
            } else if (line == "stop" && m_core && replyToStop) {
              socket->write("bye\n");
              socket->disconnectFromServer();
            }
          }
        });
      }
    });
    if (!listen(name))
      qFatal("cannot create isolated IPC peer");
  }

  int starts() const
  {
    return std::count_if(commands.cbegin(), commands.cend(), [](const auto &line) {
      return line.startsWith("start=");
    });
  }

  void dropConnections()
  {
    for (auto *socket : std::as_const(sockets))
      socket->abort();
    sockets.clear();
    close();
  }

  QList<QByteArray> commands;
  QList<QLocalSocket *> sockets;
  bool replyToStop = true;
  bool replyToStart = true;

private:
  bool m_core;
};

} // namespace

class CoreProcessRecoveryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void adoptsMatchingOrphanAndCanStopIt();
  void reconnectsAfterWatchdogReplacement();
  void retryRestartsDesiredServiceCore();
  void stopCancelsServiceRecovery();
  void stopCancelsAdoptedCoreRestart();
  void ignoresLateServiceStartResultAfterStop();
  void lateDuplicateDoesNotUndoStop();
  void restartWaitsForSlowAdoptedCoreShutdown();
  void adoptedCoreUnexpectedLossCanBeCancelled();
  void adoptedCoreReconnectsAfterTransientIpcLoss();
  void adoptedCoreExitRetriesDesktopStartAfterIpcProbe();
  void stopCancelsAdoptedRestartAfterEarlyBye();
  void repeatedRestartWaitsForEarlyByeTransportClose();
  void defaultStopTargetsAdoptedCoreDuringModeSwitch();
  void startWhileAdoptedCoreClosingWaitsForTransportClose();

private:
  QTemporaryDir m_settings;
};

void CoreProcessRecoveryTests::initTestCase()
{
  QVERIFY(m_settings.isValid());
  Settings::setSettingsFile(m_settings.filePath(QStringLiteral("settings.ini")));
  Settings::setStateFile(m_settings.filePath(QStringLiteral("state.ini")));
  Settings::setValue(Settings::Core::ProcessMode, QVariant::fromValue(Settings::ProcessMode::Service));
}

void CoreProcessRecoveryTests::adoptsMatchingOrphanAndCanStopIt()
{
  const auto coreName = endpoint();
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, endpoint());
  process.setMode(Settings::CoreMode::Client);
  QSignalSpy clients(&process, &CoreProcess::connectedClientsChanged);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.processState(), ProcessState::Started);
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  QTRY_VERIFY(!clients.isEmpty());
  QCOMPARE(clients.last().at(0).toStringList(), QStringList{QStringLiteral("laptop")});
  QCOMPARE(process.secureSocketVersion(), QStringLiteral("TLSv1.3"));
  process.stop(Settings::ProcessMode::Desktop);
  QTRY_VERIFY(core.commands.contains("stop"));
  QTRY_COMPARE(process.processState(), ProcessState::Stopped);
}

void CoreProcessRecoveryTests::reconnectsAfterWatchdogReplacement()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 4000);
  core.dropConnections();
  QTRY_COMPARE(process.connectionState(), ConnectionState::Disconnected);
  Peer replacement(coreName, true);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 7000);
  QCOMPARE(process.processState(), ProcessState::Started);
  QCOMPARE(daemon.starts(), 1); // watchdog owns the replacement, GUI must adopt it
  process.stop(Settings::ProcessMode::Service);
}

void CoreProcessRecoveryTests::retryRestartsDesiredServiceCore()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 4000);
  daemon.dropConnections();
  core.dropConnections();
  QTRY_COMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
  Peer restartedDaemon(daemonName);
  Peer restartedCore(coreName, true);
  process.retryDaemon();
  QTRY_COMPARE(restartedDaemon.starts(), 1);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 4000);
  QCOMPARE(process.processState(), ProcessState::Started);
  process.stop(Settings::ProcessMode::Service);
}

void CoreProcessRecoveryTests::stopCancelsServiceRecovery()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 4000);
  daemon.dropConnections();
  core.dropConnections();
  QTRY_COMPARE(process.connectionState(), ConnectionState::Disconnected);
  process.stop(Settings::ProcessMode::Service);
  Peer restartedDaemon(daemonName);
  process.retryDaemon();
  QTRY_VERIFY(restartedDaemon.commands.contains("logPath"));
  QTest::qWait(1200);
  QCOMPARE(restartedDaemon.starts(), 0);
  QCOMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
}

void CoreProcessRecoveryTests::stopCancelsAdoptedCoreRestart()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.restart(); // configured target mode is Service
  QTRY_VERIFY(core.commands.contains("stop"));
  process.stop(Settings::ProcessMode::Desktop);
  for (auto *socket : std::as_const(core.sockets)) {
    socket->write("bye\n");
    socket->disconnectFromServer();
  }
  QTest::qWait(1200);
  QCOMPARE(daemon.starts(), 0);
  QCOMPARE(process.processState(), ProcessState::Stopped);
}

void CoreProcessRecoveryTests::ignoresLateServiceStartResultAfterStop()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  daemon.replyToStart = false;
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QTRY_COMPARE(daemon.starts(), 1);
  const auto request = *std::find_if(daemon.commands.cbegin(), daemon.commands.cend(), [](const auto &line) {
    return line.startsWith("start=");
  });
  const auto requestId = request.split('=').at(1);
  process.stop(Settings::ProcessMode::Service);
  QTRY_COMPARE(process.processState(), ProcessState::Stopped);
  for (auto *socket : std::as_const(daemon.sockets))
    socket->write("startResult=" + requestId + "=ok\n");
  QTest::qWait(1200);
  QCOMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
  QVERIFY(core.commands.isEmpty());
}

void CoreProcessRecoveryTests::lateDuplicateDoesNotUndoStop()
{
  const auto coreName = endpoint();
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, endpoint());
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  process.stop(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTest::qWait(250);
  QCOMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
  QVERIFY(core.commands.isEmpty());
}

void CoreProcessRecoveryTests::restartWaitsForSlowAdoptedCoreShutdown()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.restart();
  QTRY_VERIFY(core.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->write("bye\n");
  QTest::qWait(1300);
  QCOMPARE(daemon.starts(), 0);
  QCOMPARE(process.processState(), ProcessState::Stopping);
  for (auto *socket : std::as_const(core.sockets))
    socket->disconnectFromServer();
  QTRY_COMPARE(daemon.starts(), 1);
  process.stop(Settings::ProcessMode::Service);
}

void CoreProcessRecoveryTests::adoptedCoreUnexpectedLossCanBeCancelled()
{
  const auto coreName = endpoint();
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, endpoint());
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  core.dropConnections();
  QTRY_COMPARE_WITH_TIMEOUT(process.processState(), ProcessState::RetryPending, 500);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
  process.stop(Settings::ProcessMode::Desktop);
  Peer replacement(coreName, true);
  QTest::qWait(1500);
  QCOMPARE(process.processState(), ProcessState::Stopped);
  QVERIFY(replacement.commands.isEmpty());
}

void CoreProcessRecoveryTests::adoptedCoreReconnectsAfterTransientIpcLoss()
{
  const auto coreName = endpoint();
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, endpoint());
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  core.dropConnections();
  QTRY_COMPARE_WITH_TIMEOUT(process.processState(), ProcessState::RetryPending, 500);
  Peer replacement(coreName, true);
  QSignalSpy errors(&process, &CoreProcess::error);
  QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), ConnectionState::Connected, 4000);
  QCOMPARE(process.processState(), ProcessState::Started);
  QVERIFY(errors.isEmpty());
  process.stop(Settings::ProcessMode::Desktop);
  QTRY_VERIFY(replacement.commands.contains("stop"));
}

void CoreProcessRecoveryTests::adoptedCoreExitRetriesDesktopStartAfterIpcProbe()
{
  // Exercise the real QProcess failure path without spawning a Core or helper.
  // A runner with a Core beside the test binary must not launch it.
  const auto corePath = QCoreApplication::applicationDirPath() + '/' + kCoreBinName;
  if (QFileInfo::exists(corePath))
    QSKIP("Core binary is present; this isolated test must not launch it");
  const auto coreName = endpoint();
  Peer core(coreName, true);
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, endpoint());
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  QSignalSpy errors(&process, &CoreProcess::error);
  core.dropConnections();
  QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 9000);
  QCOMPARE(errors.at(0).at(0).value<CoreProcess::Error>(), CoreProcess::Error::StartFailed);
  QCOMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(process.connectionState(), ConnectionState::Disconnected);
  process.stop(Settings::ProcessMode::Desktop);
}

void CoreProcessRecoveryTests::stopCancelsAdoptedRestartAfterEarlyBye()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.restart();
  QTRY_VERIFY(core.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->write("bye\n");
  QTest::qWait(150);
  QCOMPARE(daemon.starts(), 0);
  process.stop(Settings::ProcessMode::Desktop);
  QTest::qWait(150);
  QCOMPARE(process.processState(), ProcessState::Stopping);
  for (auto *socket : std::as_const(core.sockets))
    socket->disconnectFromServer();
  QTRY_COMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(daemon.starts(), 0);
}

void CoreProcessRecoveryTests::repeatedRestartWaitsForEarlyByeTransportClose()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.restart();
  QTRY_VERIFY(core.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->write("bye\n");
  QTest::qWait(150);
  process.restart();
  QTest::qWait(150);
  QCOMPARE(daemon.starts(), 0);
  QCOMPARE(process.processState(), ProcessState::Stopping);
  for (auto *socket : std::as_const(core.sockets))
    socket->disconnectFromServer();
  QTRY_COMPARE(daemon.starts(), 1);
  process.stop(Settings::ProcessMode::Service);
}

void CoreProcessRecoveryTests::defaultStopTargetsAdoptedCoreDuringModeSwitch()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.restart();
  QTRY_VERIFY(core.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->write("bye\n");
  QTest::qWait(150);
  process.stop(); // Settings names the new Service mode; the closing Core is Desktop.
  QTest::qWait(150);
  QCOMPARE(process.processState(), ProcessState::Stopping);
  QVERIFY(!daemon.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->disconnectFromServer();
  QTRY_COMPARE(process.processState(), ProcessState::Stopped);
  QCOMPARE(daemon.starts(), 0);
}

void CoreProcessRecoveryTests::startWhileAdoptedCoreClosingWaitsForTransportClose()
{
  const auto coreName = endpoint();
  const auto daemonName = endpoint();
  Peer daemon(daemonName);
  Peer core(coreName, true);
  core.replyToStop = false;
  ServerConfig config;
  QTest::ignoreMessage(QtCriticalMsg, "core server binary does not exist");
  CoreProcess process(config, coreName, daemonName);
  process.setMode(Settings::CoreMode::Client);
  process.start(Settings::ProcessMode::Service);
  QVERIFY(
      QMetaObject::invokeMethod(
          &process, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, s_exitDuplicate),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QTRY_COMPARE(process.connectionState(), ConnectionState::Connected);
  process.stop(Settings::ProcessMode::Desktop);
  QTRY_VERIFY(core.commands.contains("stop"));
  for (auto *socket : std::as_const(core.sockets))
    socket->write("bye\n");
  QTest::qWait(150);
  process.start(Settings::ProcessMode::Service);
  QTest::qWait(150);
  QCOMPARE(daemon.starts(), 0);
  QCOMPARE(process.processState(), ProcessState::Stopping);
  for (auto *socket : std::as_const(core.sockets))
    socket->disconnectFromServer();
  QTRY_COMPARE(daemon.starts(), 1);
  process.stop(Settings::ProcessMode::Service);
}

QTEST_MAIN(CoreProcessRecoveryTests)
#include "CoreProcessRecoveryTests.moc"
