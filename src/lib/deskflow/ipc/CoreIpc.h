/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Enums.h"

#include <QList>
#include <QPair>
#include <QString>

void ipcSendToClient(const QString &command, const QString &args = "");
void ipcSendConnectionState(deskflow::core::ConnectionState state);
void ipcSendConnectionAddresses(const QList<QPair<QString, QString>> &addresses);
