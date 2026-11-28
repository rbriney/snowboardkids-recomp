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
//   SK1_NET_PLAYERS=<2-4>            host only: start automatically once this many players have joined (tests);
//                                    otherwise up to four can join and the host presses Start to begin
//   SK1_NET_DELAY=<ticks>            host only: input delay (default 3 ticks = 100 ms)
//   SK1_NET_OWN_VIEW=1               with one player on this device, show only that player's view, full screen,
//                                    in multiplayer races instead of the split screen
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
    enum class PlayerState : uint8_t { Connected = 0, Lagging = 1, Disconnected = 2, Dropped = 3, Away = 4 };
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
        bool waiting_to_start = false; // before the first tick: host waiting for players, or joiner connecting
        bool host_away = false;        // joiner: the host has switched to another app
        bool welcomed = false;         // joiner: the host has answered
        bool loading = false;          // started from the lobby: waiting for every device to finish loading the game
        bool can_start = false;        // host, before the start: enough players have joined and all have the save
        uint32_t players_joined = 0;   // host, before the start
        std::string address;           // host: this device's address and port; joiner: the host's
        bool am_host = false;
        bool can_continue_without = false; // a player is disconnected; the host may drop them
        uint32_t total_ports = 0;
        // Host, before the start: internet play.
        std::string public_address;  // "a.b.c.d:port" from STUN or the router, or "" while unknown
        bool public_lookup_done = false; // STUN gave up (public_address may still come from the router)
        int router_state = 0;        // sk1net_internet::PortState
        std::string router_detail;   // why the router could not open the port
        std::string pings;           // e.g. "23 ms, 41 ms" (one per joining device)
        uint32_t planned_delay = 0;  // input delay the ping calls for, in ticks
        PlayerStatus players[4];
    };
    OverlayStatus overlay_status();
    // A message for the player (the session ended, the games went out of sync...), for a few seconds after it
    // happened; "" otherwise. Shown as a banner over the game.
    std::string current_notice();

    // This device's address on the local network (the one other devices would use to reach it), or "" if unknown.
    std::string local_address();

    // A game being hosted on the local network, found by LAN discovery.
    struct DiscoveredHost {
        std::string address; // "a.b.c.d:port"
        uint32_t players_joined = 0;
        uint32_t total_players = 0;
    };
    // Broadcasts discovery requests on the local network and collects replies from hosts that are waiting for
    // players. Runs on its own thread until stop_discovery(); discovered_hosts() returns the hosts heard from in the
    // last few seconds.
    void start_discovery(uint16_t port);
    void stop_discovery();
    std::vector<DiscoveredHost> discovered_hosts();

    // Lobby sessions: the waiting room runs in the Netplay screen, before the game is launched. begin_lobby_session
    // reads the SK1_NET_* settings and waits for players on a background thread; the lobby launches the game once
    // lobby_state() says Started, and returns to its pages when it says Ended (finish_lobby_session() gives the
    // reason). save_path is the host's save file (the game has not been started yet, so it cannot tell us).
    enum class LobbyState { Waiting, Started, Ended };
    bool begin_lobby_session(const std::string& save_path);
    LobbyState lobby_state();
    void lobby_request_start();  // host: start with the players who have joined
    void lobby_request_cancel(); // host: cancel the game; joiner: leave it
    std::string finish_lobby_session();

    bool active();
    // The number of players in a running session, or 0 without one.
    int running_player_count();
    // Which local controller should rumble for a game port: the port itself without netplay; during netplay the
    // local player index if that port's player is on this device, otherwise -1 (someone else's rumble).
    int local_player_for_port(int port);
    // Called at the start of each game tick with the game's ControllerInputState[4] array. Blocks until every
    // player's input for this tick has arrived, then writes all four ports.
    // music_active: whether this device's music is playing; the host sends its own with its inputs.
    void pre_tick(uint8_t* rdram, gpr inputs, bool music_active);
    // Whether the host's music was playing, as sent with the host's input for the current tick.
    bool host_music_active();
    // Called after the tick's game logic with a hash of the game state, for desync detection, and the words it was
    // made from (kept for recent ticks and written to a file if the devices disagree).
    void post_tick(uint32_t state_hash, const uint32_t* state_words = nullptr, size_t word_count = 0);
    // Redirect game saves to the session's in-memory save. Return false when netplay is inactive.
    bool save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size);
    bool save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size);
    // The session's save as the game started with it (false until the session has started).
    bool session_save(std::vector<uint8_t>& out);
}
