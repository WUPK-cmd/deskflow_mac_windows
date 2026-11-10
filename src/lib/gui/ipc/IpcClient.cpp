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
  if (m_state == State::Connecting || m_state == State::Handshaking) {
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
  m_socket->disconnectFromServer();
}

void IpcClient::handleDisconnected()
{
  if (m_state == State::Handshaking || m_state == State::Connected)
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
  while ((m_state == State::Handshaking || m_state == State::Connected) && m_socket->canReadLine()) {
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

    if (m_state == State::Handshaking) {
      handleHandshakeMessage(parts);
      continue;
    }

    if (parts.at(0) == QStringLiteral("bye")) {
      qDebug().noquote() << QStringLiteral("%1 ipc server is shutting down").arg(m_typeName);
      disconnectFromServer();
      Q_EMIT serverShutdown();
      return;
    }

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

  if (parts.at(0) == QStringLiteral("versionMismatch")) {
    const auto serverVersion = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc version mismatch (client: %2, server: %3)").arg(m_typeName, versionId, serverVersion);
    m_handshakeTimer.stop();
    m_state = State::Connected;
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

} // namespace deskflow::gui::ipc
