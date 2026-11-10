/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "PointerMetricsTests.h"
#include "base/ILogOutputter.h"
#include "base/PointerMetrics.h"
#include <QTest>
#include <array>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace deskflow;
void PointerMetricsTests::histogramBoundaries()
{
  PointerSummary summary;
  for (const uint64_t value : {0ULL, 250ULL, 251ULL, 1000ULL, 1000000ULL, 1000001ULL})
    summary.observe(value);
  const auto s = summary.snapshot();
  QCOMPARE(s.count, uint64_t(6));
  QCOMPARE(s.sum, uint64_t(2001502));
  QCOMPARE(s.maximum, uint64_t(1000001));
  QCOMPARE(s.buckets[0], uint64_t(2));
  QCOMPARE(s.buckets[1], uint64_t(2));
  QCOMPARE(s.buckets[9], uint64_t(1));
  QCOMPARE(s.buckets[10], uint64_t(1));
}
void PointerMetricsTests::concurrentObservations()
{
  PointerSummary summary;
  std::array<std::thread, 4> threads;
  for (auto &t : threads)
    t = std::thread([&] {
      for (int i = 0; i < 10000; ++i)
        summary.observe(8000);
    });
  for (auto &t : threads)
    t.join();
  const auto s = summary.snapshot();
  QCOMPARE(s.count, uint64_t(40000));
  QCOMPARE(s.sum, uint64_t(320000000));
  QCOMPARE(s.maximum, uint64_t(8000));
  QCOMPARE(s.buckets[3], uint64_t(40000));
}
void PointerMetricsTests::reportingIsRateLimited()
{
  PointerReportGate gate;
  QVERIFY(gate.acquire(10));
  QVERIFY(!gate.acquire(10));
  QVERIFY(!gate.acquire(1000009));
  QVERIFY(gate.acquire(1000010));
  QVERIFY(!gate.acquire(1000011));
  QVERIFY(gate.acquire(9000000));
}
void PointerMetricsTests::messageClockWrapIsHandled()
{
  QCOMPARE(PointerMetrics::messageAgeUs(20, 10), uint64_t(10000));
  QCOMPARE(PointerMetrics::messageAgeUs(0x10, 0xfffffff0), uint64_t(32000));
}
void PointerMetricsTests::runtimeOptInMatchesEnvironment()
{
  const char *env = std::getenv("DESKFLOW_POINTER_METRICS");
  QCOMPARE(PointerMetrics::enabled(), env != nullptr && std::strcmp(env, "1") == 0);
  PointerMetrics::observe(PointerMetric::Count); // invalid IDs must be safely ignored
  PointerMetrics::gap(PointerMetric::Count);
}
void PointerMetricsTests::reportingHasSingleConcurrentWinner()
{
  PointerReportGate gate;
  std::atomic<int> winners{0};
  std::array<std::thread, 8> threads;
  for (auto &thread : threads)
    thread = std::thread([&] {
      if (gate.acquire(100))
        ++winners;
    });
  for (auto &thread : threads)
    thread.join();
  QCOMPARE(winners.load(), 1);
}

void PointerMetricsTests::reportContainsOnlyAggregates()
{
  class Capture : public ILogOutputter
  {
  public:
    QStringList messages;
    void open(const QString &) override
    {
    }
    void close() override
    {
    }
    bool write(LogLevel::Level, const QString &message) override
    {
      messages.append(message);
      return true;
    }
  };
  Log log;
  auto *capture = new Capture;
  log.insert(capture);
  PointerMetrics::observe(PointerMetric::WinHookMove);
  PointerMetrics::report();
  PointerMetrics::report();
  if (PointerMetrics::enabled()) {
    QCOMPARE(capture->messages.size(), 1);
    const auto &line = capture->messages[0];
    QVERIFY(line.contains("DF_POINTER_METRICS v=1"));
    QVERIFY(line.contains("win_hook_move={n:1,sum:0,max:0}"));
    QVERIFY(!line.contains("coordinate"));
    QVERIFY(!line.contains("clipboard"));
  } else {
    QVERIFY(capture->messages.empty());
  }
}

QTEST_MAIN(PointerMetricsTests)
