/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QTimer>

class QLocalSocket;

namespace deskflow::gui::ipc {

class IpcClient : public QObject
{
  Q_OBJECT

  // Represents underlying socket state and whether the server responded to the hello message.
  enum class State
  {
    Unconnected,
    Connecting,
    Handshaking,
    Connected,
    VersionMismatch,
    RecoveryHandshaking,
    RecoveryConnected,
    ServerClosing,
  };

public:
  explicit IpcClient(QObject *parent, const QString &socketName, const QString &typeName);
  void connectToServer();
  void disconnectFromServer();

  bool isConnected() const
  {
    return m_state == State::Connected;
  }
  bool isClosing() const
  {
    return m_state == State::ServerClosing;
  }

Q_SIGNALS:
  void connected();
  void connectionFailed();
  void serverShutdown();
  void versionMismatch();

private Q_SLOTS:
  void handleDisconnected();
  void handleErrorOccurred();
  void handleReadyRead();

protected:
  virtual void processCommand(const QString &command, const QStringList &parts)
  {
    Q_UNUSED(command)
    Q_UNUSED(parts)
  }

  void sendMessage(const QString &message);
  void sendStopMessage();

private:
  void attemptConnection();
  void handleHandshakeMessage(const QStringList &parts);
  void handleConnectionSuccess();
  void failConnection();

  QLocalSocket *m_socket;
  State m_state{State::Unconnected};
  QString m_socketName;
  QTimer m_retryTimer;
  QTimer m_handshakeTimer;
  int m_retryCount{0};
  QString m_typeName;
  QString m_serverVersion;
};

} // namespace deskflow::gui::ipc
