/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpc.h"

#include "CoreIpcServer.h"
#include "common/Constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>

void ipcSendToClient(const QString &command, const QString &args)
{
  // Queued because callers may not be on the main thread,
  // and QLocalSocket can only be written to from its owning thread.
  auto &server = deskflow::core::ipc::CoreIpcServer::instance();
  QMetaObject::invokeMethod(
      &server, [command, args] { deskflow::core::ipc::CoreIpcServer::instance().broadcastCommand(command, args); },
      Qt::QueuedConnection
  );
}

void ipcSendConnectionState(deskflow::core::ConnectionState state)
{
  const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionState>();
  ipcSendToClient(QStringLiteral("connectionState"), metaEnum.valueToKey(static_cast<int>(state)));
}

void ipcSendConnectionAddresses(const QList<QPair<QString, QString>> &addresses)
{
  QJsonArray paths;
  for (const auto &[local, remote] : addresses) {
    if (!local.isEmpty() && !remote.isEmpty())
      paths.append(QJsonArray{local, remote});
  }
  auto payload = QJsonDocument(paths).toJson(QJsonDocument::Compact);
  const auto command = QStringLiteral("connectionAddresses");
  // A diagnostic snapshot must never exceed the local IPC frame limit.
  if (command.size() + payload.size() + 2 > kMaxIpcMessageSize)
    payload = QByteArrayLiteral("[]");
  ipcSendToClient(command, QString::fromUtf8(payload));
}
