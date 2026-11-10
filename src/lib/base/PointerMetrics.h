/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace deskflow {
enum class PointerMetric
{
  WinHookMove,
  WinHookPostFailure,
  WinMessageAgeUs,
  WinWarpUs,
  WinWarpDiscard,
  WinWarpDrainUs,
  WinIgnoredMove,
  WinBogusMove,
  ServerMoveGapUs,
  ServerSerializeUs,
  SocketQueuedBytes,
  SocketBusyUs,
  SocketWriteUs,
  SocketRetry,
  PacketReadCalls,
  PacketReadBytes,
  PacketBufferBytes,
  PacketReadUs,
  ClientBatchMessages,
  ClientBatchUs,
  ClientMoveDecoded,
  ClientMoveOverwritten,
  ClientMoveDirect,
  ClientMoveFlushed,
  ClientNoopUs,
  MacPostGapUs,
  MacPostUs,
  StreamQueueAgeUs,
  MacDisplayQueryUs,
  MacEventBuildUs,
  Count
};

// Fixed memory, no payloads and no allocations in observation paths. Counters are
// cumulative; a concurrent snapshot is approximate, never a cross-thread barrier.
struct PointerSummary
{
  static constexpr std::array<uint64_t, 10> bounds{250, 1000, 4000, 8000, 16000, 32000, 64000, 128000, 256000, 1000000};
  struct Snapshot
  {
    uint64_t count = 0, sum = 0, maximum = 0;
    std::array<uint64_t, bounds.size() + 1> buckets{};
  };
  void observe(uint64_t value);
  Snapshot snapshot() const;

private:
  std::atomic<uint64_t> m_count{0}, m_sum{0}, m_max{0};
  std::array<std::atomic<uint64_t>, bounds.size() + 1> m_buckets{};
};

class PointerReportGate
{
public:
  bool acquire(uint64_t nowUs);

private:
  std::atomic<uint64_t> m_next{0};
};

class PointerMetrics
{
public:
  static bool enabled();
  static uint64_t nowUs();
  static void observe(PointerMetric metric, uint64_t value = 0);
  static void gap(PointerMetric metric);
  // Call on the event thread, never inside a hook or with a socket lock held.
  static void report();
  static constexpr uint64_t messageAgeUs(uint32_t now, uint32_t posted)
  {
    return static_cast<uint64_t>(static_cast<uint32_t>(now - posted)) * 1000;
  }
  class Scope
  {
  public:
    explicit Scope(PointerMetric metric) : m_metric(metric), m_start(enabled() ? nowUs() : 0)
    {
    }
    ~Scope()
    {
      if (m_start != 0)
        observe(m_metric, nowUs() - m_start);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    PointerMetric m_metric;
    uint64_t m_start;
  };
};
} // namespace deskflow
