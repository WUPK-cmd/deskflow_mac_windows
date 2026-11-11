/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace deskflow::core::ipc {

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName, bool crossUser)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  m_server->setSocketOptions(crossUser ? QLocalServer::WorldAccessOption : QLocalServer::UserAccessOption);
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen()
{
  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR("%s ipc server failed to listen on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  }
}

void IpcServer::handleNewConnection()
{
  QLocalSocket *clientSocket = m_server->nextPendingConnection();
  if (!clientSocket) {
    LOG_ERR("%s ipc server failed to get new connection", m_typeName.constData());
    return;
  }

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
  clientSocket->setReadBufferSize(kMaxIpcMessageSize + 1);

  connect(clientSocket, &QLocalSocket::readyRead, this, [this, clientSocket] { handleReadyRead(clientSocket); });
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
  if (clientSocket->bytesAvailable() > 0)
    handleReadyRead(clientSocket);
}

void IpcServer::handleReadyRead(QLocalSocket *clientSocket)
{
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  while (clientSocket->state() == QLocalSocket::ConnectedState && clientSocket->canReadLine()) {
    const auto limit = qMin<qint64>(clientSocket->bytesAvailable(), kMaxIpcMessageSize);
    const auto line = clientSocket->readLine(limit + 1);
    if (!line.endsWith('\n')) {
      clientSocket->abort();
      return;
    }
    processMessage(clientSocket, QString::fromUtf8(QByteArrayView(line).chopped(1)));
  }
  if (clientSocket->bytesAvailable() >= kMaxIpcMessageSize)
    clientSocket->abort();
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_readyClients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  if (clientSocket->error() == QLocalSocket::PeerClosedError) {
    LOG_DEBUG("%s ipc server client closed connection", m_typeName.constData());
  } else {
    LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  }

  m_readyClients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE("%s ipc server got message: %s", m_typeName.constData(), message.toUtf8().constData());
  if (!authorizeClient(clientSocket)) {
    writeToClientSocket(clientSocket, QStringLiteral("error=access denied"));
    clientSocket->disconnectFromServer();
    return;
  }

  const auto separator = message.indexOf('=');
  const QStringList parts =
      separator < 0 ? QStringList{message} : QStringList{message.left(separator), message.mid(separator + 1)};
  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (parts.size() < 2) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QString::fromLatin1(kIpcVersionId);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      m_readyClients.remove(clientSocket);
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->flush();
      return;
    }

    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    m_readyClients.insert(clientSocket);
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    // Current state belongs to every GUI, including one adopting an orphaned
    // core. Unlike one-off notifications it must survive the first handshake.
    for (const auto &state : std::as_const(m_stateMessages))
      writeToClientSocket(clientSocket, state);

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else if (m_readyClients.contains(clientSocket)) {
    processCommand(clientSocket, command, parts);
  } else {
    writeToClientSocket(clientSocket, QStringLiteral("error=hello required"));
    clientSocket->disconnectFromServer();
  }

  clientSocket->flush();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);
  const bool stateMessage = command == QStringLiteral("connectionState") ||
                            command == QStringLiteral("connectedClients") || command == QStringLiteral("secureSocket");
  if (stateMessage)
    m_stateMessages.insert(command, message);

  if (m_readyClients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(), message.toUtf8().constData()
    );
    if (!stateMessage)
      m_pendingMessages.append(message);
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_readyClients.size(),
      message.toUtf8().constData()
  );
  const auto clients = m_readyClients.values();
  for (auto *client : clients) {
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(), message.toUtf8().constData()
    );
  }
}

} // namespace deskflow::core::ipc
