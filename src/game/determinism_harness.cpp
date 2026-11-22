// Determinism harness: records every game tick's controller inputs plus game-state hashes, or replays a recording
// and compares the hashes. Groundwork for lockstep netplay (see netplay notes); inactive unless one of these
// environment variables is set:
//
//   SK1_DET_RECORD=<path>   record to <path> (and the starting save to <path>.save)
//   SK1_DET_REPLAY=<path>   replay <path>, injecting its inputs, and write <path>.report.txt
//
// In either mode, if <path>.snapshots exists it lists tick numbers (whitespace-separated); after each listed tick
// the first 8 MB of RDRAM is written raw to <path>.tick<N>.rdram for offline comparison.
//
// While active, game saves go to an in-memory copy (seeded from the real save file when recording, from
// <path>.save when replaying), so the real save file is never modified.
//
// Hooks: patches/determinism_harness.c calls recomp_det_pre_tick at the start of updateGameTaskScheduler and
// recomp_det_post_tick after the game tasks have run.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "ultramodern/ultramodern.hpp"
#include "determinism_harness.h"
#include "netplay.h"
#include "perf_stats.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

constexpr char file_magic[8] = { 'S', 'K', '1', 'D', 'E', 'T', '0', '1' };
constexpr uint32_t port_count = 4;
constexpr uint32_t racer_count = 4;
constexpr uint32_t racer_size = 0x60C;
constexpr uint32_t rdram_hashed_size = 8 * 1024 * 1024;
constexpr uint32_t block_size = 64 * 1024;
constexpr uint32_t block_count = rdram_hashed_size / block_size;
constexpr uint32_t save_size = 0x8000;
// Named hashes: one per racer, then RNG/frame-counter state.
constexpr uint32_t named_count = racer_count + 1;
constexpr uint32_t rng_named_index = racer_count;

struct PortInput {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
};

struct TickRecord {
    uint32_t tick;
    uint8_t connected_mask;
    uint8_t padding[3];
    PortInput inputs[port_count];
    uint32_t named[named_count];
    uint32_t blocks[block_count];
};
static_assert(sizeof(TickRecord) == 4 + 4 + 16 + 4 * named_count + 4 * block_count);

enum class Mode { Off, Record, Replay };

struct BlockStats {
    int64_t first_mismatch = -1;
    uint64_t mismatches = 0;
};

struct State {
    Mode mode = Mode::Off;
    bool initialised = false;
    // Hash all of RDRAM in 64 KB blocks each tick. Costly on phones (8 MB per tick), so automatic netplay recordings
    // keep only the inputs and the named state hashes; explicit recordings and replays keep the blocks.
    bool hash_blocks = true;
    std::filesystem::path path;
    FILE* record_file = nullptr;
    std::vector<TickRecord> replay;
    bool replay_finished = false;
    uint32_t tick = 0;
    TickRecord current{};
    std::set<uint32_t> snapshot_ticks;

    // Replay comparison results.
    int64_t first_named_mismatch = -1;
    std::array<int64_t, named_count> named_first{};
    std::array<uint64_t, named_count> named_mismatches{};
    std::array<BlockStats, block_count> block_stats{};
    uint64_t compared_ticks = 0;

    std::mutex save_mutex;
    bool save_ready = false;
    std::vector<uint8_t> save;
} state;

// Keeps the automatic netplay recordings folder to the newest few sessions (each side writes about 1 MB a minute).
void prune_old_recordings(const std::filesystem::path& folder) {
    constexpr size_t keep = 10;
    std::vector<std::filesystem::path> recordings;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() == ".bin") {
            recordings.push_back(entry.path());
        }
    }
    if (recordings.size() < keep) {
        return;
    }
    std::sort(recordings.begin(), recordings.end(), [](const auto& a, const auto& b) {
        std::error_code e;
        return std::filesystem::last_write_time(a, e) < std::filesystem::last_write_time(b, e);
    });
    for (size_t i = 0; i + keep - 1 < recordings.size(); i++) {
        std::filesystem::remove(recordings[i], ec);
        std::filesystem::path save = recordings[i];
        save += ".save";
        std::filesystem::remove(save, ec);
    }
}

// Trimmed recordings (idle stretches removed) zero the hashes of every tick after a cut, since the frame counter
// no longer lines up; those ticks are replayed but not compared.
bool recorded_hashes_cleared(const TickRecord& rec) {
    for (uint32_t value : rec.named) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

const char* named_label(uint32_t index) {
    static const char* labels[named_count] = { "racer 0", "racer 1", "racer 2", "racer 3", "rng/frame counter" };
    return labels[index];
}

void log_line(const char* fmt, const std::string& arg = {}) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "SK1det", fmt, arg.c_str());
    return;
#endif
    std::fprintf(stderr, "[det] ");
    std::fprintf(stderr, fmt, arg.c_str());
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void init() {
    if (state.initialised) {
        return;
    }
    state.initialised = true;
    state.named_first.fill(-1);
    const auto load_snapshot_ticks = [] {
        std::filesystem::path list = state.path;
        list += ".snapshots";
        std::ifstream in(list);
        uint32_t t;
        while (in >> t) {
            state.snapshot_ticks.insert(t);
        }
        if (!state.snapshot_ticks.empty()) {
            log_line("RDRAM snapshots requested at %s ticks", std::to_string(state.snapshot_ticks.size()));
        }
    };

    if (const char* p = std::getenv("SK1_DET_RECORD"); p != nullptr && *p != '\0') {
        state.mode = Mode::Record;
        state.path = std::filesystem::u8path(p);
        state.record_file = std::fopen(state.path.string().c_str(), "wb");
        if (state.record_file == nullptr) {
            log_line("cannot open %s for writing; harness disabled", state.path.string());
            state.mode = Mode::Off;
            return;
        }
        std::fwrite(file_magic, 1, sizeof(file_magic), state.record_file);
        log_line("recording to %s", state.path.string());
        load_snapshot_ticks();
        return;
    }

    if (const char* p = std::getenv("SK1_DET_REPLAY"); p != nullptr && *p != '\0') {
        // (replay handled below)
    } else if ((std::getenv("SK1_NET_HOST") != nullptr && *std::getenv("SK1_NET_HOST") != '\0') ||
               (std::getenv("SK1_NET_JOIN") != nullptr && *std::getenv("SK1_NET_JOIN") != '\0')) {
        // Netplay sessions record themselves, so a desync (or a good race) can be replayed and examined later.
        std::filesystem::path folder = std::filesystem::current_path();
        if (const char* app = std::getenv("APP_FOLDER_PATH"); app != nullptr && *app != '\0') {
            folder = std::filesystem::u8path(app);
        }
        folder /= "netplay-recordings";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        prune_old_recordings(folder);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        const bool hosting = std::getenv("SK1_NET_HOST") != nullptr && *std::getenv("SK1_NET_HOST") != '\0';
        state.path = folder / (std::string(hosting ? "host-" : "join-") + stamp + ".bin");
        state.hash_blocks = false;
        state.record_file = std::fopen(state.path.string().c_str(), "wb");
        if (state.record_file != nullptr) {
            // Large buffer: on Android this folder is on shared storage, where frequent small writes stall the game.
            std::setvbuf(state.record_file, nullptr, _IOFBF, 1 << 20);
            state.mode = Mode::Record;
            std::fwrite(file_magic, 1, sizeof(file_magic), state.record_file);
            log_line("recording this netplay session to %s", state.path.string());
        }
        return;
    }

    if (const char* p = std::getenv("SK1_DET_REPLAY"); p != nullptr && *p != '\0') {
        state.path = std::filesystem::u8path(p);
        std::ifstream in(state.path, std::ios::binary);
        char magic[sizeof(file_magic)]{};
        if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, file_magic, sizeof(magic)) != 0) {
            log_line("%s is not a recording; harness disabled", state.path.string());
            return;
        }
        TickRecord rec;
        while (in.read(reinterpret_cast<char*>(&rec), sizeof(rec))) {
            state.replay.push_back(rec);
        }
        state.mode = Mode::Replay;
        load_snapshot_ticks();
        log_line("replaying %s", state.path.string() + " (" + std::to_string(state.replay.size()) + " ticks)");
    }
}

uint32_t hash_words(uint8_t* rdram, gpr vaddr, uint32_t size) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t offset = 0; offset < size; offset += 4) {
        h = (h ^ static_cast<uint32_t>(MEM_W(offset, vaddr))) * 0x100000001b3ULL;
    }
    return static_cast<uint32_t>(h ^ (h >> 32));
}

uint32_t hash_rdram_block(const uint8_t* rdram, uint32_t offset) {
    const uint64_t* words = reinterpret_cast<const uint64_t*>(rdram + offset);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < block_size / 8; i++) {
        h = (h ^ words[i]) * 0x100000001b3ULL;
    }
    return static_cast<uint32_t>(h ^ (h >> 32));
}

void write_report(bool final_report) {
    std::filesystem::path report_path = state.path;
    report_path += ".report.txt";
    std::ofstream out(report_path, std::ios::trunc);
    out << "Snowboard Kids determinism replay report\n";
    out << "recording: " << state.path.string() << "\n";
    out << "recorded ticks: " << state.replay.size() << ", compared: " << state.compared_ticks
        << (final_report ? " (replay finished)" : " (in progress)") << "\n\n";

    if (state.first_named_mismatch < 0) {
        out << "RESULT: game state matched on every compared tick.\n\n";
    } else {
        out << "RESULT: game state diverged at tick " << state.first_named_mismatch << ".\n\n";
    }

    out << "Named state (first mismatching tick, mismatching ticks):\n";
    for (uint32_t i = 0; i < named_count; i++) {
        out << "  " << named_label(i) << ": ";
        if (state.named_first[i] < 0) {
            out << "always matched\n";
        } else {
            out << "first " << state.named_first[i] << ", " << state.named_mismatches[i] << " ticks\n";
        }
    }

    out << "\nRDRAM 64 KB blocks that ever differed (address, first mismatching tick, mismatching ticks):\n";
    for (uint32_t b = 0; b < block_count; b++) {
        const BlockStats& s = state.block_stats[b];
        if (s.first_mismatch >= 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "  0x%08X  first %lld  %llu ticks\n", 0x80000000u + b * block_size,
                static_cast<long long>(s.first_mismatch), static_cast<unsigned long long>(s.mismatches));
            out << line;
        }
    }
}

void load_save_locked() {
    if (state.save_ready) {
        return;
    }
    state.save_ready = true;
    state.save.assign(save_size, 0);

    std::filesystem::path save_copy = state.path;
    save_copy += ".save";
    if (state.mode == Mode::Record) {
        std::ifstream in(ultramodern::get_save_file_path(), std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(state.save.data()), save_size);
        }
        std::ofstream out(save_copy, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(state.save.data()), save_size);
        log_line("saved starting save data to %s", save_copy.string());
    } else {
        std::ifstream in(save_copy, std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(state.save.data()), save_size);
            log_line("using recorded save data from %s", save_copy.string());
        } else {
            log_line("%s missing; replaying with an empty save", save_copy.string());
        }
    }
}

} // namespace

bool sk1det::recorded_input(uint32_t tick, uint32_t port, uint16_t& buttons, int8_t& stick_x, int8_t& stick_y) {
    init();
    if (state.mode != Mode::Replay || tick >= state.replay.size() || port >= port_count) {
        return false;
    }
    buttons = state.replay[tick].inputs[port].buttons;
    stick_x = state.replay[tick].inputs[port].stick_x;
    stick_y = state.replay[tick].inputs[port].stick_y;
    return true;
}

bool sk1det::replay_save(std::vector<uint8_t>& out) {
    init();
    if (state.mode != Mode::Replay) {
        return false;
    }
    std::filesystem::path save_copy = state.path;
    save_copy += ".save";
    std::ifstream in(save_copy, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(save_size, 0);
    in.read(reinterpret_cast<char*>(out.data()), save_size);
    return true;
}

bool sk1det::active() {
    init();
    return state.mode != Mode::Off;
}

bool sk1det::save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size) {
    if (!active()) {
        return false;
    }
    std::lock_guard lock{ state.save_mutex };
    load_save_locked();
    for (uint32_t i = 0; i < size; i++) {
        MEM_B(i, dst) = state.save[offset + i];
    }
    return true;
}

bool sk1det::save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size) {
    if (!active()) {
        return false;
    }
    std::lock_guard lock{ state.save_mutex };
    load_save_locked();
    for (uint32_t i = 0; i < size; i++) {
        state.save[offset + i] = MEM_B(i, src);
    }
    return true;
}

// s32 recomp_det_sync_render(void): nonzero while the harness or netplay is active, telling the tick patch to finish
// outstanding frames before each tick so renderer timing cannot change game state.
extern "C" void recomp_det_sync_render(uint8_t* rdram, recomp_context* ctx) {
    sk1perf::tick_begin(); // called at the very start of every tick (performance diagnostics)
    _return<s32>(ctx, (sk1det::active() || sk1net::active()) ? 1 : 0);
}

// s32 recomp_det_ticks_per_update(void): game ticks per scheduler update. 1 normally; SK1_DET_SPEED=<n> (1-8)
// fast-forwards a replay that is not part of a netplay session.
extern "C" void recomp_det_ticks_per_update(uint8_t* rdram, recomp_context* ctx) {
    static const int32_t speed = [] {
        const char* value = std::getenv("SK1_DET_SPEED");
        return value != nullptr ? std::clamp(std::atoi(value), 1, 8) : 1;
    }();
    const bool fast = speed > 1 && state.mode == Mode::Replay && !state.replay_finished && !sk1net::active();
    _return<s32>(ctx, fast ? speed : 1);
}

// void recomp_det_pre_tick(ControllerInputState* inputs, u8* connectedMask)
extern "C" void recomp_det_pre_tick(uint8_t* rdram, recomp_context* ctx) {
    sk1perf::tick_render_wait_done();
    // Netplay supplies the inputs first, so a netplay session can also be recorded by the harness.
    sk1net::pre_tick(rdram, ctx->r4);
    if (!sk1det::active()) {
        return;
    }
    gpr inputs = ctx->r4;
    gpr mask = ctx->r5;

    TickRecord& rec = state.current;
    rec = {};
    rec.tick = state.tick;

    if (state.mode == Mode::Replay && state.tick < state.replay.size()) {
        const TickRecord& recorded = state.replay[state.tick];
        if (state.tick == 0 && MEM_BU(0, mask) != recorded.connected_mask) {
            log_line("warning: connected controllers differ from the recording (%s)",
                std::to_string(MEM_BU(0, mask)) + " vs " + std::to_string(recorded.connected_mask));
        }
        // During netplay the session supplies the inputs (scripted from this recording when SK1_NET_SCRIPT is
        // set) and the harness only compares the resulting game state.
        if (!sk1net::active()) {
            for (uint32_t port = 0; port < port_count; port++) {
                gpr addr = inputs + port * 6;
                MEM_H(0, addr) = recorded.inputs[port].buttons;
                MEM_B(2, addr) = recorded.inputs[port].stick_x;
                MEM_B(3, addr) = recorded.inputs[port].stick_y;
            }
        }
    } else if (state.mode == Mode::Replay && !state.replay_finished) {
        state.replay_finished = true;
        write_report(true);
        log_line("replay finished; report written to %s", state.path.string() + ".report.txt");
    }

    rec.connected_mask = MEM_BU(0, mask);
    for (uint32_t port = 0; port < port_count; port++) {
        gpr addr = inputs + port * 6;
        rec.inputs[port].buttons = MEM_HU(0, addr);
        rec.inputs[port].stick_x = MEM_B(2, addr);
        rec.inputs[port].stick_y = MEM_B(3, addr);
    }
}

// void recomp_det_post_tick(RacePlayer* players, u16* mainRngIndex, u16* secondaryRngIndex, s16* frameCounter)
extern "C" void recomp_det_post_tick(uint8_t* rdram, recomp_context* ctx) {
    const bool harness = sk1det::active();
    const bool netplay = sk1net::active();
    if (!harness && !netplay) {
        return;
    }
    gpr players = ctx->r4;
    gpr main_rng = ctx->r5;
    gpr secondary_rng = ctx->r6;
    gpr frame_counter = ctx->r7;

    uint32_t named[named_count];
    for (uint32_t i = 0; i < racer_count; i++) {
        named[i] = hash_words(rdram, players + i * racer_size, racer_size);
    }
    named[rng_named_index] = (static_cast<uint32_t>(MEM_HU(0, main_rng)) << 20) ^
        (static_cast<uint32_t>(MEM_HU(0, secondary_rng)) << 8) ^ static_cast<uint16_t>(MEM_H(0, frame_counter));
    if (netplay) {
        uint32_t combined = 0x811c9dc5u;
        for (uint32_t value : named) {
            combined = (combined ^ value) * 0x01000193u;
        }
        sk1net::post_tick(combined);
    }
    if (!harness) {
        return;
    }

    TickRecord& rec = state.current;
    std::memcpy(rec.named, named, sizeof(named));
    for (uint32_t b = 0; b < block_count; b++) {
        rec.blocks[b] = state.hash_blocks ? hash_rdram_block(rdram, b * block_size) : 0;
    }

    if (state.snapshot_ticks.count(state.tick) != 0) {
        std::filesystem::path snapshot = state.path;
        snapshot += ".tick" + std::to_string(state.tick) + ".rdram";
        std::ofstream out(snapshot, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(rdram), rdram_hashed_size);
        log_line("wrote %s", snapshot.string());
    }

    if (state.mode == Mode::Record && state.tick == 0 && netplay) {
        std::vector<uint8_t> session_save;
        if (sk1net::session_save(session_save)) {
            std::filesystem::path save_copy = state.path;
            save_copy += ".save";
            std::ofstream out(save_copy, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(session_save.data()), session_save.size());
        }
    }

    if (state.mode == Mode::Record) {
        std::fwrite(&rec, sizeof(rec), 1, state.record_file);
        // Flush about once a minute (the stdio buffer holds the rest), never every tick.
        if ((state.tick % 1800) == 0) {
            std::fflush(state.record_file);
        }
    } else if (state.mode == Mode::Replay && state.tick < state.replay.size() &&
               !recorded_hashes_cleared(state.replay[state.tick])) {
        const TickRecord& recorded = state.replay[state.tick];
        state.compared_ticks++;
        bool named_mismatch = false;
        for (uint32_t i = 0; i < named_count; i++) {
            if (rec.named[i] != recorded.named[i]) {
                named_mismatch = true;
                state.named_mismatches[i]++;
                if (state.named_first[i] < 0) {
                    state.named_first[i] = state.tick;
                }
            }
        }
        // Recordings made without block hashes (automatic netplay recordings) store zeros: nothing to compare.
        const bool recorded_blocks = std::any_of(std::begin(recorded.blocks), std::end(recorded.blocks),
            [](uint32_t value) { return value != 0; });
        for (uint32_t b = 0; recorded_blocks && b < block_count; b++) {
            if (rec.blocks[b] != recorded.blocks[b]) {
                BlockStats& s = state.block_stats[b];
                s.mismatches++;
                if (s.first_mismatch < 0) {
                    s.first_mismatch = state.tick;
                }
            }
        }
        if (named_mismatch && state.first_named_mismatch < 0) {
            state.first_named_mismatch = state.tick;
            log_line("DIVERGED at tick %s", std::to_string(state.tick));
            write_report(false);
        }
        if ((state.tick % 300) == 0) {
            write_report(false);
        }
    }

    state.tick++;
}
