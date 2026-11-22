#pragma once

#include <cstdint>

#include "recomp.h"

// Two-player lockstep netplay (src/game/netplay.cpp). Each machine runs the whole game; only controller inputs are
// exchanged, keyed by game tick. Configured by environment variables (on Android, from netplay.txt in the app's
// external files folder):
//
//   SK1_NET_HOST=<udp port>          host; plays as player 1 and supplies the save data for the session
//   SK1_NET_JOIN=<address>:<port>    join a host; plays as player 2
//   SK1_NET_DELAY=<ticks>            input delay chosen by the host (default 3 ticks = 100 ms)
//
// During a session both machines use an in-memory copy of the host's save, so neither real save file changes.
namespace sk1net {
    bool active();
    // Called at the start of each game tick with the game's ControllerInputState[4] array. Blocks until every
    // player's input for this tick has arrived, then writes all four ports.
    void pre_tick(uint8_t* rdram, gpr inputs);
    // Called after the tick's game logic with a hash of the game state, for desync detection.
    void post_tick(uint32_t state_hash);
    // Redirect game saves to the session's in-memory save. Return false when netplay is inactive.
    bool save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size);
    bool save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size);
}
