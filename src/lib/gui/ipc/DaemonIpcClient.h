/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcClient.h"
#include "common/Constants.h"

#include <QObject>

namespace deskflow::gui::ipc {

class DaemonIpcClient : public IpcClient
{
  Q_OBJECT

public:
  explicit DaemonIpcClient(QObject *parent = nullptr, const QString &socketName = kDaemonIpcName);
  void sendLogLevel(const QString &logLevel);
  void sendStartProcess(quint64 requestId, const QString &mode, const QString &configFile);
  void sendStopProcess();
  void sendClearSettings();
  void requestLogPath();

Q_SIGNALS:
  void logPathReceived(const QString &logPath);
  void startResultReceived(quint64 requestId, bool started);

protected:
  void processCommand(const QString &command, const QStringList &parts) override;
};

} // namespace deskflow::gui::ipc
