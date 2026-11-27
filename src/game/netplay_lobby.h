#pragma once

#include <functional>
#include <string>

// Netplay lobby screen (src/game/netplay_lobby.cpp), opened from the launcher. Lets players host a game (choosing
// how many players in total and on this device) or join one found on the local network or typed in, then starts the
// game with those settings. The choices are remembered in netplay.cfg in the app folder.
namespace sk1net_lobby {
    // The Netplay screen, including the waiting room. start_game is called (on the UI thread) once the host has
    // started the session; game_id names the host's save file (saves/<game_id>.bin).
    void open(const std::u8string& game_id, std::function<void()> start_game);
    void close();
    bool is_open();
    // Called every UI frame (from the netplay overlay's frame hook) to refresh the list of games found.
    void frame_update();
}
