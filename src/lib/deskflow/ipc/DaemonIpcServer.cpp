/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DaemonIpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"

#include <QLocalSocket>

#include "platform/MSWindowsHandle.h"
#include <Wtsapi32.h>
#include <cstdlib>
#include <vector>

namespace {

std::vector<BYTE> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type)
{
  DWORD size = 0;
  GetTokenInformation(token, type, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  if (!GetTokenInformation(token, type, data.data(), size, &size))
    data.clear();
  return data;
}

std::unique_ptr<MSWindowsHandle> authorizedClientToken(QLocalSocket *socket, bool foreground, DWORD &sessionId)
{
  const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
  DWORD expectedSession = WTSGetActiveConsoleSessionId();
  if (foreground && !ProcessIdToSessionId(GetCurrentProcessId(), &expectedSession))
    return {};
  if (expectedSession == MAXDWORD || !GetNamedPipeClientSessionId(pipe, &sessionId) || sessionId != expectedSession)
    return {};

  HANDLE expected = nullptr;
  if (foreground ? !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &expected)
                 : !WTSQueryUserToken(expectedSession, &expected))
    return {};
  const MSWindowsHandle expectedToken(expected);

  if (!ImpersonateNamedPipeClient(pipe))
    return {};
  HANDLE caller = nullptr;
  const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &caller);
  if (!RevertToSelf())
    std::abort();
  if (!opened)
    return {};

  auto client = std::make_unique<MSWindowsHandle>(caller);
  DWORD callerSession = 0;
  DWORD size = 0;
  if (!GetTokenInformation(caller, TokenSessionId, &callerSession, sizeof(callerSession), &size) ||
      callerSession != expectedSession)
    return {};

  BYTE interactiveSid[SECURITY_MAX_SID_SIZE];
  DWORD sidSize = sizeof(interactiveSid);
  BOOL interactive = FALSE;
  if (!CreateWellKnownSid(WinInteractiveSid, nullptr, interactiveSid, &sidSize) ||
      !CheckTokenMembership(caller, interactiveSid, &interactive) || !interactive)
    return {};

  const auto callerUser = tokenInfo(caller, TokenUser);
  const auto expectedUser = tokenInfo(expected, TokenUser);
  if (callerUser.empty() || expectedUser.empty() ||
      !EqualSid(
          reinterpret_cast<const TOKEN_USER *>(callerUser.data())->User.Sid,
          reinterpret_cast<const TOKEN_USER *>(expectedUser.data())->User.Sid
      ))
    return {};

  if (foreground) {
    const auto callerLevel = tokenInfo(caller, TokenIntegrityLevel);
    const auto expectedLevel = tokenInfo(expected, TokenIntegrityLevel);
    if (IsTokenRestricted(caller) || callerLevel.empty() || expectedLevel.empty() ||
        !EqualSid(
            reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(callerLevel.data())->Label.Sid,
            reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(expectedLevel.data())->Label.Sid
        ))
      return {};
  }

  return client;
}

} // namespace

namespace deskflow::core::ipc {

const auto kAckMessage = "ok";
const auto kErrorMessage = "error";

DaemonIpcServer::DaemonIpcServer(QObject *parent, const QString &logFilename, bool foreground)
    : IpcServer(parent, kDaemonIpcName, QStringLiteral("daemon"), !foreground),
      m_logFilename(logFilename),
      m_foreground(foreground)
{
  // do nothing
}

bool DaemonIpcServer::authorizeClient(QLocalSocket *clientSocket) const
{
  DWORD sessionId = 0;
  return bool(authorizedClientToken(clientSocket, m_foreground, sessionId));
}

void DaemonIpcServer::processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts)
{
  if (command == QStringLiteral("logLevel")) {
    processLogLevel(clientSocket, parts);
  } else if (command == QStringLiteral("start")) {
    processStart(clientSocket, parts.value(1));
  } else if (command == QStringLiteral("stop")) {
    LOG_DEBUG("daemon ipc server got stop message");
    Q_EMIT stopProcessRequested();
    writeToClientSocket(clientSocket, kAckMessage);
  } else if (command == QStringLiteral("logPath")) {
    LOG_DEBUG("daemon ipc server got log path request");
    writeToClientSocket(clientSocket, QStringLiteral("logPath=%1").arg(m_logFilename.toUtf8()));
  } else if (command == QStringLiteral("clearSettings")) {
    LOG_DEBUG("daemon ipc server got clear settings message");
    Q_EMIT clearSettingsRequested();
    writeToClientSocket(clientSocket, kAckMessage);
  } else {
    LOG_WARN("daemon ipc server got unknown command: %s", command.toUtf8().constData());
  }
}

void DaemonIpcServer::processLogLevel(QLocalSocket *&clientSocket, const QStringList &messageParts)
{
  if (messageParts.size() < 2) {
    LOG_ERR("daemon ipc server got invalid log level message");
    writeToClientSocket(clientSocket, kErrorMessage);
    return;
  }

  const auto &logLevel = messageParts.at(1);
  if (logLevel.isEmpty()) {
    LOG_ERR("daemon ipc server got empty log level");
    writeToClientSocket(clientSocket, kErrorMessage);
    return;
  }

  LOG_DEBUG("daemon ipc server got new log level: %s", logLevel.toUtf8().constData());
  Q_EMIT logLevelChanged(logLevel);
  writeToClientSocket(clientSocket, kAckMessage);
}

void DaemonIpcServer::replyToStart(QLocalSocket *client, const QString &requestId, bool started) const
{
  writeToClientSocket(client, QStringLiteral("startResult=%1=%2").arg(requestId, started ? "ok" : "error"));
  client->flush();
}

void DaemonIpcServer::processStart(QLocalSocket *clientSocket, const QString &args)
{
  const auto requestEnd = args.indexOf('=');
  const auto modeEnd = args.indexOf('=', requestEnd + 1);
  const auto requestId = args.left(requestEnd);
  bool validId = false;
  requestId.toULongLong(&validId);
  if (!validId || requestEnd < 0 || modeEnd < 0) {
    writeToClientSocket(clientSocket, kErrorMessage);
    return;
  }

  DWORD sessionId = 0;
  const auto caller = authorizedClientToken(clientSocket, m_foreground, sessionId);
  HANDLE primary = nullptr;
  if (caller && (m_foreground || DuplicateTokenEx(
                                     caller->get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, nullptr,
                                     SecurityImpersonation, TokenPrimary, &primary
                                 ))) {
    const auto token = std::make_shared<MSWindowsHandle>(primary);
    Q_EMIT startRequested(
        clientSocket, requestId, args.mid(requestEnd + 1, modeEnd - requestEnd - 1), args.mid(modeEnd + 1), token,
        sessionId
    );
    return;
  }
  replyToStart(clientSocket, requestId, false);
}

} // namespace deskflow::core::ipc
