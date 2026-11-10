/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DaemonApp.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/LogOutputters.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "deskflow/ipc/DaemonIpcServer.h"

#if defined(Q_OS_WIN)
#include "arch/win32/ArchDaemonWindows.h"
#include "deskflow/Computer.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsEventQueueBuffer.h"
#include "platform/MSWindowsWatchdog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#endif

#include <QCoreApplication>
#include <QDir>
#include <QLocalSocket>
#include <QPointer>
#include <QSettings>

using namespace deskflow::core;

namespace {

QSettings daemonSettings(bool foreground)
{
  const auto root = foreground ? QStringLiteral("HKEY_CURRENT_USER") : QStringLiteral("HKEY_LOCAL_MACHINE");
  return QSettings(root + QStringLiteral("\\SOFTWARE\\Deskflow\\Daemon"), QSettings::NativeFormat);
}

} // namespace

DaemonApp::DaemonApp(IEventQueue &events) : m_events(events)
{
  // do nothing
}

DaemonApp::~DaemonApp() = default;

void DaemonApp::saveLogLevel(const QString &logLevel) const
{
  LOG_DEBUG("log level changed: %s", logLevel.toUtf8().constData());
  if (CLOG->setFilter(logLevel.toUtf8().constData()))
    daemonSettings(m_foreground).setValue(QStringLiteral("LogLevel"), logLevel);
}

void DaemonApp::applyWatchdogCommand(
    ipc::DaemonIpcServer *server, QLocalSocket *client, const QString &requestId, const QString &mode,
    const QString &configFile, const std::shared_ptr<MSWindowsHandle> &token, quint32 sessionId
) const
{
#if defined(Q_OS_WIN)
  const auto path = QDir::fromNativeSeparators(configFile);
  if ((mode != QStringLiteral("server") && mode != QStringLiteral("client")) || path.size() <= 3 || path.at(1) != ':' ||
      path.at(2) != '/' || path.contains('"') || path.contains(QChar(u'\0'))) {
    LOG_ERR("cannot start core: invalid mode or settings path");
    server->replyToStart(client, requestId, false);
    return;
  }

  // The service does not open the settings file. The child reads it with the caller's token.
  const auto corePath = QStringLiteral("%1/%2").arg(QCoreApplication::applicationDirPath(), kCoreBinName);
  const auto command = QStringLiteral("\"%1\" %2 --settings \"%3\"").arg(corePath, mode, path).toStdWString();
  m_pWatchdog->setProcessConfig(
      command, token, sessionId,
      [server, client = QPointer(client), requestId](bool started) {
        QMetaObject::invokeMethod(
            server,
            [server, client, requestId, started] {
              if (client && client->state() == QLocalSocket::ConnectedState)
                server->replyToStart(client, requestId, started);
            },
            Qt::QueuedConnection
        );
      }
  );
#endif
}

void DaemonApp::clearWatchdogCommand()
{
  LOG_DEBUG("clearing watchdog command");

#if defined(Q_OS_WIN)
  m_pWatchdog->setProcessConfig({});
#else
  LOG_ERR("clearing watchdog command not implemented on this platform");
#endif
}

void DaemonApp::clearSettings()
{
  LOG_INFO("clearing daemon settings");
  daemonSettings(m_foreground).remove(QStringLiteral("LogLevel"));
}

void DaemonApp::connectIpcServer(ipc::DaemonIpcServer *ipcServer) const
{
  // Use direct connection as this object is on it's own thread,
  // and so is on a different event loop to the main Qt loop.
  connect(ipcServer, &ipc::DaemonIpcServer::logLevelChanged, this, &DaemonApp::saveLogLevel, Qt::DirectConnection);
  connect(
      ipcServer, &ipc::DaemonIpcServer::startRequested, this,
      [this, ipcServer](
          QLocalSocket *client, const QString &requestId, const QString &mode, const QString &configFile,
          const std::shared_ptr<MSWindowsHandle> &token, quint32 sessionId
      ) { applyWatchdogCommand(ipcServer, client, requestId, mode, configFile, token, sessionId); },
      Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::stopProcessRequested, this, &DaemonApp::clearWatchdogCommand,
      Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::clearSettingsRequested, this, &DaemonApp::clearSettings, Qt::DirectConnection
  );
}

void DaemonApp::run(QThread &daemonThread)
{
  LOG_INFO("starting daemon");

  // Important: Move the daemon app to the daemon thread before creating any more Qt objects
  // owned by the daemon app, as they will be created on the daemon thread.
  moveToThread(&daemonThread);

  connect(&daemonThread, &QThread::started, this, [this, &daemonThread]() {
    LOG_DEBUG("daemon thread started");

    if (m_foreground) {
      LOG_DEBUG("running daemon in foreground");
      mainLoop();
    } else {
      LOG_DEBUG("running daemon in background (daemonizing)");
      ARCH->daemonize([this] { return daemonLoop(); });
    }

    daemonThread.quit();
    LOG_DEBUG("daemon thread finished");
  });

#if defined(Q_OS_WIN)
  m_pWatchdog = std::make_unique<MSWindowsWatchdog>(m_foreground, *m_pFileLogOutputter);
#endif

  LOG_DEBUG("starting daemon thread");
  daemonThread.start();
}

int DaemonApp::daemonLoop()
{
#if defined(Q_OS_WIN)
  // Runs the daemon through the Windows service controller, which controls the program lifecycle.
  return ArchDaemonWindows::runDaemon([this]() { return mainLoop(); });
#else
  return mainLoop();
#endif
}

int DaemonApp::mainLoop()
{
#if defined(Q_OS_WIN)
  if (m_pWatchdog == nullptr) {
    LOG_ERR("watchdog not initialized");
    return s_exitFailed;
  }
  ArchDaemonWindows::daemonRunning(true);
#endif

  try {
#if defined(Q_OS_WIN)
    // Install the platform event queue to handle service stop events.
    // This must be done on the same thread as the event loop, otherwise the service stop
    // request will not add the quit event to the event queue, and the service won't stop.
    m_events.adoptBuffer(new MSWindowsEventQueueBuffer(&m_events));

    LOG_DEBUG("starting watchdog threads");
    m_pWatchdog->startAsync();
#endif

    LOG_INFO("daemon is running");
    m_events.loop();
  } catch (std::exception &e) { // NOSONAR - Catching all exceptions
    LOG_CRIT("daemon error: %s", e.what());
  } catch (...) { // NOSONAR - Catching remaining exceptions
    LOG_CRIT("daemon unknown error");
  }

  LOG_INFO("daemon is stopping");

#if defined(Q_OS_WIN)
  try {
    LOG_DEBUG("stopping process watchdog");
    m_pWatchdog->stop();
  } catch (std::exception &e) { // NOSONAR - Catching all exceptions
    LOG_CRIT("daemon stop watchdog error: %s", e.what());
  } catch (...) { // NOSONAR - Catching remaining exceptions
    LOG_CRIT("daemon stop watchdog unknown error");
  }
  ArchDaemonWindows::daemonRunning(false);
#endif

  return s_exitSuccess;
}

QString DaemonApp::logFilename() const
{
  const auto directory = m_foreground ? Settings::UserDir : QCoreApplication::applicationDirPath();
  return QDir(directory).filePath(QStringLiteral("deskflow-daemon.log"));
}

QString DaemonApp::logLevel() const
{
  return daemonSettings(m_foreground).value(QStringLiteral("LogLevel")).toString();
}

void DaemonApp::setForeground()
{
  m_foreground = true;
  showConsole();
}

void DaemonApp::initLogging()
{
  if (m_foreground)
    QDir().mkpath(Settings::UserDir);
#if defined(Q_OS_WIN)
  if (!m_foreground) {
    // Only use MS debug outputter when the process is daemonized, since stdout won't be accessible
    // in that case, but is accessible when running in the foreground.
    CLOG->insert(new MSWindowsDebugOutputter()); // NOSONAR - Adopted by `Log`
  }
#endif

  m_pFileLogOutputter = new FileLogOutputter(qPrintable(logFilename())); // NOSONAR - Adopted by `Log`
  CLOG->insert(m_pFileLogOutputter);
}

void DaemonApp::showConsole()
{
#if defined(Q_OS_WIN)
  // The daemon bin is compiled using the Win32 subsystem which works best for Windows services,
  // so when running as a foreground process we need to allocate a console (or we won't see output).
  // It is important to do this inside the arg check loop so that we can attach console ahead
  // of log output generated by handling other args.
  AllocConsole();
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
#endif
}
