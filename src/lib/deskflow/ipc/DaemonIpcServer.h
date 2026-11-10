/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcServer.h"

#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <memory>

class MSWindowsHandle;

namespace deskflow::core::ipc {

class DaemonIpcServer : public IpcServer
{
  Q_OBJECT

public:
  explicit DaemonIpcServer(QObject *parent, const QString &logFilename, bool foreground);
  void replyToStart(QLocalSocket *client, const QString &requestId, bool started) const;

Q_SIGNALS:
  void startRequested(
      QLocalSocket *client, const QString &requestId, const QString &mode, const QString &configFile,
      const std::shared_ptr<MSWindowsHandle> &token, quint32 sessionId
  );

private:
  bool authorizeClient(QLocalSocket *clientSocket) const override;
  void processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts) override;
  void processLogLevel(QLocalSocket *&clientSocket, const QStringList &messageParts);
  void processStart(QLocalSocket *clientSocket, const QString &args);

  const QString m_logFilename;
  const bool m_foreground;
};

} // namespace deskflow::core::ipc
