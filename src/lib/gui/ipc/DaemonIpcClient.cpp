/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DaemonIpcClient.h"

#include "common/Constants.h"

namespace deskflow::gui::ipc {

DaemonIpcClient::DaemonIpcClient(QObject *parent) : IpcClient(parent, kDaemonIpcName, QStringLiteral("daemon"))
{
}

void DaemonIpcClient::sendLogLevel(const QString &logLevel)
{
  sendMessage(QStringLiteral("logLevel=%1").arg(logLevel));
}

void DaemonIpcClient::sendStartProcess(quint64 requestId, const QString &mode, const QString &configFile)
{
  sendMessage(QStringLiteral("start=%1=%2=%3").arg(QString::number(requestId), mode, configFile));
}

void DaemonIpcClient::sendStopProcess()
{
  sendMessage(QStringLiteral("stop"));
}

void DaemonIpcClient::sendClearSettings()
{
  sendMessage(QStringLiteral("clearSettings"));
}

void DaemonIpcClient::requestLogPath()
{
  sendMessage(QStringLiteral("logPath"));
}

void DaemonIpcClient::processCommand(const QString &command, const QStringList &parts)
{
  if (command == QStringLiteral("logPath") && parts.size() == 2) {
    Q_EMIT logPathReceived(parts.at(1));
  } else if (command == QStringLiteral("startResult") && parts.size() == 2) {
    const auto result = parts.at(1).split('=');
    bool validId = false;
    const auto requestId = result.at(0).toULongLong(&validId);
    if (validId && result.size() == 2)
      Q_EMIT startResultReceived(requestId, result.at(1) == QStringLiteral("ok"));
  }
}

} // namespace deskflow::gui::ipc
