#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"

// Lockstep netplay for up to four players (src/game/netplay.cpp). Each machine runs the whole game; only controller
// inputs are exchanged, keyed by game tick. Configured by environment variables (on Android, from netplay.txt in the
// app's external files folder):
//
//   SK1_NET_HOST=<udp port>          host; takes the first player slots and supplies the save data for the session
//   SK1_NET_JOIN=<address>:<port>    join a host; takes the next free player slots
//   SK1_NET_LOCAL_PLAYERS=<1-4>      players on this machine (default 1); with more than one, the controller
//                                    assignment screen opens so each local player picks a controller
//   SK1_NET_PLAYERS=<2-4>            host only: total players in the session (default: host's players + 1); the
//                                    session starts once that many have joined
//   SK1_NET_DELAY=<ticks>            host only: input delay (default 3 ticks = 100 ms)
//
// During a session both machines use an in-memory copy of the host's save, so neither real save file changes.
//
// Testing aids:
//   SK1_NET_BIND=<address>           bind to one local address (e.g. 127.0.0.1 for copies on one machine)
//   SK1_NET_INSTANCE=<name>          suffix for this copy's log file (several copies on one machine)
//   SK1_NET_SCRIPT=1                 take this player's input from the recording replayed with SK1_DET_REPLAY
//                                    (the harness then only compares state), for unattended end-to-end tests
//   SK1_NET_SIM_LOSS=<percent>       drop this share of outgoing packets
//   SK1_NET_SIM_LATENCY=<ms>         delay outgoing packets
//   SK1_NET_SIM_JITTER=<ms>          add up to this much random extra delay (can reorder packets)
namespace sk1net {
    enum class PlayerState : uint8_t { Connected = 0, Lagging = 1, Disconnected = 2, Dropped = 3 };
    struct PlayerStatus {
        bool host = false;  // this player's machine is the host
        bool local = false; // this player is on this machine
        PlayerState state = PlayerState::Connected;
        bool ready = false; // pressed A during the current pause
    };
    // What the status overlay shows (src/game/netplay_overlay.cpp).
    struct OverlayStatus {
        bool visible = false;
        bool paused = false; // session-wide pause (otherwise: this machine waiting for the host)
        bool am_host = false;
        bool can_continue_without = false; // a player is disconnected; the host may drop them
        uint32_t total_ports = 0;
        PlayerStatus players[4];
    };
    OverlayStatus overlay_status();

    bool active();
    // Called at the start of each game tick with the game's ControllerInputState[4] array. Blocks until every
    // player's input for this tick has arrived, then writes all four ports.
    void pre_tick(uint8_t* rdram, gpr inputs);
    // Called after the tick's game logic with a hash of the game state, for desync detection.
    void post_tick(uint32_t state_hash);
    // Redirect game saves to the session's in-memory save. Return false when netplay is inactive.
    bool save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size);
    bool save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size);
    // The session's save as the game started with it (false until the session has started).
    bool session_save(std::vector<uint8_t>& out);
}
