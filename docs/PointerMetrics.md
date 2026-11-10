# Opt-in pointer diagnostics (experimental)

Baseline: 1b5fbf5c249427037fb4c21f09ecbc08f7397c83. This instrumentation does not change the mouse protocol, warp/filter logic, TLS, input event order, or clipboard behavior. It is not a proven stutter fix. It is disabled unless the process starts with DESKFLOW_POINTER_METRICS=1. Keep the normal log level at INFO; DEBUG/VERBOSE is unnecessary and can include existing input content logs.

## Privacy and bounded cost

No mouse coordinates, key values, clipboard contents, peer address, payload, or fingerprint is passed to this diagnostic helper. Only counts, durations, and queue byte totals enter fixed-size atomic accumulators. Observation does not allocate or log. At most one aggregate line per second is emitted by the normal event thread, not by a hook or while holding a socket lock. The line prefix is DF_POINTER_METRICS v=1. Logger overhead can affect the event thread, so compare enabled and disabled runs; this is a diagnostic build, not a default-on feature.

Samples are cumulative from process start. n is count, sum is total, max is maximum. Duration metrics have _us suffix and fixed histogram buckets with upper bounds 250, 1000, 4000, 8000, 16000, 32000, 64000, 128000, 256000, 1000000 microseconds and an overflow bucket. Byte/count metrics omit duration buckets. Counters use n with sum/max zero. Concurrent fields are approximate snapshots, not an atomic cross-thread snapshot; subtract consecutive windows cautiously. Last partial second may not be logged if no further system event occurs.

## Interpretation

- win_hook_move is the low-level hook relay count, including synthetic warp moves. win_hook_post_failure counts failed queue posts. Neither is a count of physical hardware reports.
- win_message_age_us uses unsigned GetTickCount minus MSG.time and handles 32-bit wrap. It measures post-to-consumption time, not hardware latency. Windows timestamp resolution is coarse.
- win_warp_us times the existing recenter call. win_warp_discard counts messages intentionally removed between PRE/POST markers; they include synthetic warp moves, so this is not a lost-input percentage. win_warp_drain_us measures that existing drain path.
- server_move_gap_us measures application mouse packet call gaps across active clients. Idle pauses are included, so evaluate only continuous movement windows using count deltas. server_serialize_us includes encoding and stream enqueue.
- socket_queued_bytes is userspace FIFO size after writes (all traffic). socket_busy_us is the age of the current nonempty FIFO period when bytes are successfully drained, not individual packet latency. Socket metrics aggregate sockets and are not a physical network RTT. socket_retry counts TLS WANT retries. socket_write_us measures write attempts, not peer receipt.
- Other metric IDs are reserved for the coordinated Mac receive/injection worktree. Monotonic clock epochs on different hosts must never be directly subtracted as one-way latency.

## Safe reproduction plan

Do not replace or restart an active installation without coordinating with the user. Retain the existing binaries/settings. After an authorized switch to paired diagnostic builds, record three 30-second continuous movement windows with 5 seconds idle between: Windows local, Windows to Mac with no new clipboard changes, then repeat. Keep microphone off and do not type while sampling. A clipboard-on/off comparison is a separate coordinated setting change; an artificial public string can test large-transfer contention without exposing real clipboard content. Stop on loss of control and restore original binaries/configuration.

Classify where a gap first appears: Windows message age/warp, server serialization/socket FIFO, Mac stream queue/batch, Mac display-query/event-build/post. Only implement a behavior optimization after replay or live metrics identifies the affected stage. Preserve mouse/button ordering and protocol bytes in tests.

## Offline validation

PointerMetricsTests covers histogram bounds, atomic concurrency, 1Hz report gate (including contention), message-clock wrap, opt-in environment behavior, and aggregate-only report output. ClientProxyTests adds synthetic absolute/relative/button packet-order checks for protocol versions 1.0, 1.1, 1.6, 1.7, and 1.8. Run the test processes twice with DESKFLOW_POINTER_METRICS=0 and =1; no real input, clipboard, sockets, or active installation is used by these synthetic tests.

## Recording privacy guard

With diagnostics enabled, the core file logger accepts only the strict numeric DF_POINTER_METRICS schema. Ordinary INFO/WARN/ERROR messages are excluded from the recording. Screen-transition console messages also omit boundary coordinates in diagnostic mode. Normal builds with metrics disabled retain normal logging. FileLogOutputterTests verifies rejection of ordinary messages and appended text, and acceptance of a complete aggregate line.
