/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QObject>

class ServerProxyTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void handleKeepAliveAlarm_timeout_queuesDisconnectRequest();
  void handleData_incompleteMessage_queuesDisconnectRequest();
  void parseHandshakeMessage_protocolError_queuesRefusalRequest();
  void mouseMove_idleBatch_coalescesToLatest();
  void mouseMove_dragBatch_preservesEveryPosition();
  void mouseMove_multipleButtons_staysReliableUntilLastRelease();
  void mouseMove_inputBarriers_preserveOrder();
  void mouseMove_leaveAndEnter_clearHeldButtons();
  void mouseRelativeMove_batch_accumulatesDeltas();
  void mouseMove_relativeThenAbsolute_flushesRelativeFirst();
  void mouseMove_absoluteThenRelative_flushesAbsoluteFirst();

private:
  Log m_log;
};
