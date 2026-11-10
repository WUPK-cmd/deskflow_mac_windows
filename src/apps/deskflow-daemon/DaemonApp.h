/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QThread>
#include <memory>

class Event;
class IEventQueue;
class FileLogOutputter;
class QLocalSocket;
class MSWindowsHandle;
class QCoreApplication;

namespace deskflow::core::ipc {
class DaemonIpcServer;
}

#if defined(Q_OS_WIN)
class MSWindowsWatchdog;
#endif

extern const char *const kLogFilename;

class DaemonApp : public QObject
{
  Q_OBJECT

public:
  explicit DaemonApp(IEventQueue &events);
  ~DaemonApp() override;

  void run(QThread &daemonThread);
  void setForeground();
  void initLogging();
  void connectIpcServer(deskflow::core::ipc::DaemonIpcServer *ipcServer) const;

  QString logFilename() const;
  QString logLevel() const;

private:
  void daemonize();
  void handleError(const char *message);
  int mainLoop();
  int daemonLoop();
  void saveLogLevel(const QString &logLevel) const;
  void applyWatchdogCommand(
      deskflow::core::ipc::DaemonIpcServer *server, QLocalSocket *client, const QString &requestId, const QString &mode,
      const QString &configFile, const std::shared_ptr<MSWindowsHandle> &token, quint32 sessionId
  ) const;
  void clearWatchdogCommand();
  void clearSettings();

  static void showConsole();

#if defined(Q_OS_WIN)
  std::unique_ptr<MSWindowsWatchdog> m_pWatchdog;
#endif

  IEventQueue &m_events;
  FileLogOutputter *m_pFileLogOutputter = nullptr;
  bool m_foreground = false;
};
