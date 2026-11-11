/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcClient.h"

#include "common/Constants.h"
#include "common/VersionInfo.h"

#include <QDebug>
#include <QLocalSocket>
#include <QTimer>

namespace deskflow::gui::ipc {

IpcClient::IpcClient(QObject *parent, const QString &socketName, const QString &typeName)
    : QObject(parent),
      m_socket{new QLocalSocket(this)},
      m_socketName(socketName), // NOSONAR - Qt memory
      m_typeName(typeName)
{
  m_socket->setReadBufferSize(kMaxIpcMessageSize + 1);
  connect(m_socket, &QLocalSocket::connected, this, &IpcClient::handleConnectionSuccess);
  connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::handleDisconnected);
  connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleErrorOccurred);
  connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::handleReadyRead);
  m_retryTimer.setSingleShot(true);
  connect(&m_retryTimer, &QTimer::timeout, this, &IpcClient::attemptConnection);
  m_handshakeTimer.setSingleShot(true);
  connect(&m_handshakeTimer, &QTimer::timeout, this, &IpcClient::failConnection);
}

void IpcClient::connectToServer()
{
  if (m_state == State::Connecting || m_state == State::Handshaking || m_state == State::RecoveryHandshaking) {
    qWarning().noquote() << QStringLiteral("%1 ipc client already connecting to server").arg(m_typeName);
    return;
  }

  disconnectFromServer();
  m_socket->abort();
  m_retryCount = 0;
  m_state = State::Connecting;
  attemptConnection();
}

void IpcClient::attemptConnection()
{
  if (m_state != State::Connecting)
    return;

  if (const int retryLimit = 10; m_retryCount >= retryLimit) {
    qWarning().noquote() << QStringLiteral("%1 ipc client failed to connect after %2 attempts")
                                .arg(m_typeName, QString::number(retryLimit));
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }

  if (m_retryCount == 0) {
    qDebug().noquote() << QStringLiteral("%1 ipc client connecting to server: %2").arg(m_typeName, m_socketName);
  } else {
    qDebug().noquote() << QStringLiteral("%1 ipc client retrying connection, attempt: %2")
                              .arg(m_typeName, QString::number(m_retryCount + 1));
  }

  m_retryCount++;
  m_socket->connectToServer(m_socketName);
}

void IpcClient::disconnectFromServer()
{
  m_retryTimer.stop();
  m_handshakeTimer.stop();
  m_state = State::Unconnected;
  m_serverVersion.clear();
  m_socket->disconnectFromServer();
}

void IpcClient::handleDisconnected()
{
  if (m_state == State::ServerClosing) {
    disconnectFromServer();
    Q_EMIT serverShutdown();
    return;
  }
  if (m_state != State::Unconnected && m_state != State::Connecting)
    failConnection();
}

void IpcClient::handleErrorOccurred()
{
  if (m_state == State::Connecting) {
    if (!m_retryTimer.isActive())
      m_retryTimer.start(500);
    return;
  }

  qWarning().noquote() << QStringLiteral("%1 ipc client error: %2").arg(m_typeName, m_socket->errorString());

  handleDisconnected();
}

void IpcClient::handleReadyRead()
{
  while (m_state != State::Unconnected && m_state != State::Connecting && m_socket->canReadLine()) {
    const auto limit = qMin<qint64>(m_socket->bytesAvailable(), kMaxIpcMessageSize);
    const auto line = m_socket->readLine(limit + 1);
    if (!line.endsWith('\n')) {
      failConnection();
      return;
    }
    const auto message = QString::fromUtf8(QByteArrayView(line).chopped(1));

    qDebug().noquote() << QStringLiteral("%1 ipc client message: %2").arg(m_typeName, message);
    const auto separator = message.indexOf('=');
    const QStringList parts =
        separator < 0 ? QStringList{message} : QStringList{message.left(separator), message.mid(separator + 1)};

    if (m_state == State::Handshaking || m_state == State::RecoveryHandshaking) {
      handleHandshakeMessage(parts);
      continue;
    }

    if (parts.at(0) == QStringLiteral("bye")) {
      qDebug().noquote() << QStringLiteral("%1 ipc server is shutting down").arg(m_typeName);
      // The Core announces shutdown before draining its network peers. Its
      // transport closes only when the application has finished that teardown.
      m_state = State::ServerClosing;
      m_handshakeTimer.stop();
      return;
    }

    if (m_state == State::Connected)
      processCommand(parts.at(0), parts);
  }

  if (m_state != State::Unconnected && m_socket->bytesAvailable() >= kMaxIpcMessageSize)
    failConnection();
}

void IpcClient::handleHandshakeMessage(const QStringList &parts)
{
  if (parts.at(0) == QStringLiteral("error")) {
    const auto detail = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qCritical().noquote() << QStringLiteral("%1 ipc server rejected connection: %2").arg(m_typeName, detail);
    failConnection();
    return;
  }

  const auto versionId = QString::fromLatin1(kIpcVersionId);

  if (m_state == State::RecoveryHandshaking) {
    if (parts.at(0) != QStringLiteral("hello") || parts.value(1) != m_serverVersion) {
      failConnection();
      return;
    }
    m_state = State::RecoveryConnected;
    // Recovery grants only a stop, never normal control or a Connected signal.
    m_socket->write("stop\n");
    return;
  }

  if (parts.at(0) == QStringLiteral("versionMismatch")) {
    const auto serverVersion = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc version mismatch (client: %2, server: %3)").arg(m_typeName, versionId, serverVersion);
    m_serverVersion = serverVersion;
    m_state = State::VersionMismatch;
    Q_EMIT versionMismatch();
    return;
  }

  if (parts.at(0) != QStringLiteral("hello")) {
    return;
  }

  if (parts.value(1) != versionId) {
    qCritical().noquote() << QStringLiteral("%1 ipc server hello has an invalid version").arg(m_typeName);
    failConnection();
    return;
  }

  m_handshakeTimer.stop();
  m_state = State::Connected;
  qDebug().noquote() << QStringLiteral("%1 ipc client connected").arg(m_typeName);
  Q_EMIT connected();
}

void IpcClient::handleConnectionSuccess()
{
  if (m_state != State::Connecting) {
    m_socket->abort();
    return;
  }
  m_retryTimer.stop();
  m_state = State::Handshaking;
  m_handshakeTimer.start(5000);
  const auto versionId = QString::fromLatin1(kIpcVersionId);
  m_socket->write(QStringLiteral("hello=%1\n").arg(versionId).toUtf8());
  qDebug().noquote() << QStringLiteral("%1 ipc client sent hello with version: %2").arg(m_typeName, versionId);
}

void IpcClient::failConnection()
{
  disconnectFromServer();
  Q_EMIT connectionFailed();
}

void IpcClient::sendMessage(const QString &message)
{
  if (m_state != State::Connected) {
    qWarning().noquote() << QStringLiteral("%1 cannot send command, ipc client not connected").arg(m_typeName);
    return;
  }

  m_socket->write(message.toUtf8() + "\n");
  qDebug().noquote() << QStringLiteral("%1 ipc client sent message: %2").arg(m_typeName, message);
}

void IpcClient::sendStopMessage()
{
  if (m_state == State::ServerClosing)
    return;
  if (m_state == State::Connected) {
    sendMessage(QStringLiteral("stop"));
    return;
  }

  // Build IDs may differ while the wire protocol is identical. Repeating hello
  // with the advertised build ID is understood by existing ipc2 cores, and the
  // server still applies its same-user socket ACL and normal authorization.
  const auto versionId = QString::fromLatin1(kIpcVersionId);
  if (m_state != State::VersionMismatch || !versionId.endsWith(QStringLiteral("/ipc2")) ||
      !m_serverVersion.endsWith(QStringLiteral("/ipc2")) || m_serverVersion.contains('=')) {
    qWarning().noquote() << QStringLiteral("%1 cannot recover an incompatible ipc server").arg(m_typeName);
    failConnection();
    return;
  }
  m_state = State::RecoveryHandshaking;
  m_handshakeTimer.start(5000);
  m_socket->write(QStringLiteral("hello=%1\n").arg(m_serverVersion).toUtf8());
}

} // namespace deskflow::gui::ipc
