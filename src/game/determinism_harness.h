#pragma once

#include <cstdint>
#include <vector>

#include "recomp.h"

// Determinism harness (src/game/determinism_harness.cpp). Active when SK1_DET_RECORD or SK1_DET_REPLAY is set.
namespace sk1det {
    bool active();
    // While replaying a netplay recording, the number of players in that session (0 otherwise), so the menus that
    // netplay locks to the session's player count behave the same in the replay.
    int replay_player_count();
    // Redirect game saves to the harness's in-memory copy while it is active. Return false when inactive.
    bool save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size);
    bool save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size);
    // A replayed recording's input for one port at one tick (false if not replaying or out of range).
    // The save a replayed recording started from (false if not replaying or it has no .save file).
    bool replay_save(std::vector<uint8_t>& out);
    bool recorded_input(uint32_t tick, uint32_t port, uint16_t& buttons, int8_t& stick_x, int8_t& stick_y);
}

extern "C" void recomp_det_pre_tick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_det_post_tick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_det_sync_render(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_det_ticks_per_update(uint8_t* rdram, recomp_context* ctx);
