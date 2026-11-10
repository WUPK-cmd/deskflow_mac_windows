/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/PointerMetrics.h"
#include "base/Log.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace deskflow {
namespace {
constexpr size_t metricCount = static_cast<size_t>(PointerMetric::Count);
std::array<PointerSummary, metricCount> summaries;
std::array<std::atomic<uint64_t>, metricCount> previous{};
PointerReportGate reportGate;
struct MetricInfo
{
  const char *name;
  bool duration;
};
constexpr std::array<MetricInfo, metricCount> names{
    {{"win_hook_move", false},
     {"win_hook_post_failure", false},
     {"win_message_age_us", true},
     {"win_warp_us", true},
     {"win_warp_discard", false},
     {"win_warp_drain_us", true},
     {"win_ignored_move", false},
     {"win_bogus_move", false},
     {"server_move_gap_us", true},
     {"server_serialize_us", true},
     {"socket_queued_bytes", false},
     {"socket_busy_us", true},
     {"socket_write_us", true},
     {"socket_retry", false},
     {"packet_read_calls", false},
     {"packet_read_bytes", false},
     {"packet_buffer_bytes", false},
     {"packet_read_us", true},
     {"client_batch_messages", false},
     {"client_batch_us", true},
     {"client_move_decoded", false},
     {"client_move_overwritten", false},
     {"client_move_direct", false},
     {"client_move_flushed", false},
     {"client_noop_us", true},
     {"mac_post_gap_us", true},
     {"mac_post_us", true},
     {"stream_queue_age_us", true},
     {"mac_display_query_us", true},
     {"mac_event_build_us", true}}
};
} // namespace
void PointerSummary::observe(uint64_t value)
{
  m_count.fetch_add(1, std::memory_order_relaxed);
  m_sum.fetch_add(value, std::memory_order_relaxed);
  auto old = m_max.load(std::memory_order_relaxed);
  while (value > old && !m_max.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
  }
  size_t bucket = 0;
  while (bucket < bounds.size() && value > bounds[bucket])
    ++bucket;
  m_buckets[bucket].fetch_add(1, std::memory_order_relaxed);
}
PointerSummary::Snapshot PointerSummary::snapshot() const
{
  Snapshot s;
  s.count = m_count.load(std::memory_order_relaxed);
  s.sum = m_sum.load(std::memory_order_relaxed);
  s.maximum = m_max.load(std::memory_order_relaxed);
  for (size_t i = 0; i < s.buckets.size(); ++i)
    s.buckets[i] = m_buckets[i].load(std::memory_order_relaxed);
  return s;
}
bool PointerReportGate::acquire(uint64_t nowUs)
{
  auto next = m_next.load(std::memory_order_relaxed);
  while (nowUs >= next) {
    if (m_next.compare_exchange_weak(next, nowUs + 1000000, std::memory_order_relaxed))
      return true;
  }
  return false;
}
bool PointerMetrics::enabled()
{
  static const bool active = [] {
    const char *value = std::getenv("DESKFLOW_POINTER_METRICS");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return active;
}
uint64_t PointerMetrics::nowUs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
}
void PointerMetrics::observe(PointerMetric metric, uint64_t value)
{
  if (!enabled())
    return;
  const auto index = static_cast<size_t>(metric);
  if (index < metricCount)
    summaries[index].observe(value);
}
void PointerMetrics::gap(PointerMetric metric)
{
  if (!enabled())
    return;
  const auto index = static_cast<size_t>(metric);
  if (index >= metricCount)
    return;
  const auto now = nowUs();
  const auto old = previous[index].exchange(now, std::memory_order_relaxed);
  if (old != 0 && now >= old)
    summaries[index].observe(now - old);
}
void PointerMetrics::report()
{
  if (!enabled())
    return;
  const auto now = nowUs();
  if (!reportGate.acquire(now))
    return;
  std::ostringstream out;
  out << "DF_POINTER_METRICS v=1 monotonic_us=" << now << " cumulative=1";
  for (size_t i = 0; i < metricCount; ++i) {
    const auto s = summaries[i].snapshot();
    if (s.count == 0)
      continue;
    out << ' ' << names[i].name << "={n:" << s.count << ",sum:" << s.sum << ",max:" << s.maximum;
    if (names[i].duration) {
      out << ",hist:";
      for (size_t j = 0; j < s.buckets.size(); ++j) {
        if (j != 0)
          out << ',';
        out << s.buckets[j];
      }
    }
    out << '}';
  }
  LOG_INFO("%s", out.str().c_str());
}
} // namespace deskflow
