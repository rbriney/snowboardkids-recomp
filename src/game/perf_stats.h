#pragma once

#include <atomic>
#include <cstdint>

// Lightweight performance counters, logged periodically (src/game/perf_stats.cpp). Diagnostics for audio hitching.
namespace sk1perf {
    // Called when the game queues audio, with how much audio (in microseconds) was still queued.
    void audio_queued(uint64_t queued_microseconds);
    // Called when incoming audio had samples dropped to stop latency building up.
    void audio_skipped();
    // Called at the start of each game tick (before any render wait) and after the render wait.
    void tick_begin();
    void tick_render_wait_done();
    // Logs and resets the counters if at least `interval_seconds` have passed since the last log.
    void maybe_log(int interval_seconds = 30);
}
