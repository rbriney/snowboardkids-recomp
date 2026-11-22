#include "perf_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct Stats {
    std::mutex mutex;
    // Audio
    uint64_t audio_calls = 0;
    uint64_t audio_low = 0; // calls with less than 10 ms queued
    uint64_t audio_min_us = UINT64_MAX;
    uint64_t audio_sum_us = 0;
    uint64_t audio_skips = 0;
    // Ticks
    clock_type::time_point last_tick{};
    clock_type::time_point tick_start{};
    uint64_t ticks = 0;
    uint64_t tick_interval_max_us = 0;
    uint64_t tick_interval_sum_us = 0;
    uint64_t slow_ticks = 0; // more than 50 ms since the previous tick
    uint64_t render_wait_sum_us = 0;
    uint64_t render_wait_max_us = 0;
    clock_type::time_point last_log = clock_type::now();
} stats;

uint64_t micros(clock_type::duration d) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

void write_line(const char* text) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "SK1perf", "%s", text);
#else
    std::fprintf(stderr, "[perf] %s\n", text);
    // Optional file copy for diagnosing a desktop build: SK1_PERF_LOG=<path>.
    static FILE* f = [] {
        const char* path = std::getenv("SK1_PERF_LOG");
        return (path != nullptr && *path != '\0') ? std::fopen(path, "w") : nullptr;
    }();
    if (f != nullptr) {
        std::fprintf(f, "%s\n", text);
        std::fflush(f);
    }
#endif
}

} // namespace

void sk1perf::audio_queued(uint64_t queued_microseconds) {
    std::lock_guard lock{ stats.mutex };
    stats.audio_calls++;
    stats.audio_sum_us += queued_microseconds;
    stats.audio_min_us = std::min(stats.audio_min_us, queued_microseconds);
    if (queued_microseconds < 10000) {
        stats.audio_low++;
    }
}

void sk1perf::audio_skipped() {
    std::lock_guard lock{ stats.mutex };
    stats.audio_skips++;
}

void sk1perf::tick_begin() {
    const auto now = clock_type::now();
    std::lock_guard lock{ stats.mutex };
    if (stats.last_tick != clock_type::time_point{}) {
        const uint64_t interval = micros(now - stats.last_tick);
        stats.tick_interval_sum_us += interval;
        stats.tick_interval_max_us = std::max(stats.tick_interval_max_us, interval);
        if (interval > 50000) {
            stats.slow_ticks++;
        }
        stats.ticks++;
    }
    stats.last_tick = now;
    stats.tick_start = now;
}

void sk1perf::tick_render_wait_done() {
    const uint64_t wait = micros(clock_type::now() - stats.tick_start);
    std::lock_guard lock{ stats.mutex };
    stats.render_wait_sum_us += wait;
    stats.render_wait_max_us = std::max(stats.render_wait_max_us, wait);
}

void sk1perf::maybe_log(int interval_seconds) {
    const auto now = clock_type::now();
    std::lock_guard lock{ stats.mutex };
    if (now - stats.last_log < std::chrono::seconds(interval_seconds)) {
        return;
    }
    stats.last_log = now;
    char line[512];
    std::snprintf(line, sizeof(line),
        "audio: %llu queues, avg %.1f ms queued, min %.1f ms, %llu below 10 ms, %llu skipped | ticks: %llu, avg %.1f ms apart, "
        "max %.1f ms, %llu over 50 ms | render wait avg %.2f ms, max %.1f ms",
        (unsigned long long)stats.audio_calls,
        stats.audio_calls ? stats.audio_sum_us / 1000.0 / stats.audio_calls : 0.0,
        stats.audio_calls ? stats.audio_min_us / 1000.0 : 0.0, (unsigned long long)stats.audio_low,
        (unsigned long long)stats.audio_skips,
        (unsigned long long)stats.ticks, stats.ticks ? stats.tick_interval_sum_us / 1000.0 / stats.ticks : 0.0,
        stats.tick_interval_max_us / 1000.0, (unsigned long long)stats.slow_ticks,
        stats.ticks ? stats.render_wait_sum_us / 1000.0 / stats.ticks : 0.0, stats.render_wait_max_us / 1000.0);
    write_line(line);
    stats.audio_calls = stats.audio_low = stats.audio_sum_us = stats.audio_skips = 0;
    stats.audio_min_us = UINT64_MAX;
    stats.ticks = stats.slow_ticks = stats.tick_interval_sum_us = stats.tick_interval_max_us = 0;
    stats.render_wait_sum_us = stats.render_wait_max_us = 0;
}
