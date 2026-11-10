/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QObject>
class PointerMetricsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void histogramBoundaries();
  void concurrentObservations();
  void reportingIsRateLimited();
  void messageClockWrapIsHandled();
  void runtimeOptInMatchesEnvironment();
  void reportingHasSingleConcurrentWinner();
  void reportContainsOnlyAggregates();
};
