// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#pragma once

#include "base/Log.h"

#include <QObject>

class OutputSchedulingTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void ordinaryInputOvertakesTrailingClipboard_data();
  void ordinaryInputOvertakesTrailingClipboard();
  void modifiedKeysRemainClipboardBarriers_data();
  void modifiedKeysRemainClipboardBarriers();
  void heartbeatBypassesQueuedPasteBarrier();
  void bulkFeedWaitsForTimerWhileInputRemainsImmediate();
  void destructionCancelsBulkTimer();
  void terminationCancelsBulkTimer_data();
  void terminationCancelsBulkTimer();
  void inputKeepsRelativeOrderWhileOvertakingBulk();
  void reportsCompletionOnlyWhenAllScheduledOutputIsDrained();
  void reportsWriteErrorAfterOutputShutdown();
  void coalescesLargeBurstToOneFinalPosition();
  void keepsLatestUnsentAbsolutePosition();
  void sendsOnePacketPerOutputCompletion();
  void prioritizesPositionBeforeTrailingClipboardPackets();
  void respectsControlBarriers_data();
  void respectsControlBarriers();
  void preservesClipboardBeforePasteKeys();
  void preservesDragMovesUntilEveryButtonIsReleased();
  void preservesRelativeMotion();
  void ignoresStaleAndRepeatedOutputCompletion();
  void closeCancelsPendingPackets();
  void shutdownOutputCancelsPendingPackets();
  void flushDrainsScheduledPacketsBeforeReturning();
  void outputFailureCancelsPendingPackets_data();
  void outputFailureCancelsPendingPackets();
  void unbufferedOutputPreservesImmediateOrder();
  void streamsHaveIndependentPendingPositions();
  void clipboardChunksAreBoundedAndTransfersStaySerialized();

private:
  Log m_log;
};
