// Helpers for netplay over the internet (src/game/netplay.cpp): opening the host's port on the home router with
// UPnP, and finding the host's public address with STUN.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct sockaddr_in;

namespace sk1net_internet {
    // UPnP (local network only). open_port asks the router, on a background thread, to forward a UDP port to this
    // device; close_port removes the forwarding again. Both are safe to call when there is no UPnP router.
    enum class PortState { Idle, Working, Open, Failed };
    // Where progress messages go (the netplay log); stderr until set.
    void set_logger(void (*logger)(const char* message));
    void open_port(uint16_t port);
    void close_port();
    PortState port_state();
    std::string port_detail();     // why it failed, for the log and the waiting room
    std::string router_address();  // the public address the router reports (may be empty)

    // STUN (RFC 5389). The request must be sent from the game's own socket, so the address the server reports is
    // the one other players reach.
    constexpr const char* stun_host = "stun.l.google.com";
    constexpr uint16_t stun_port = 19302;
    bool resolve_stun_server(sockaddr_in& out);         // blocking DNS lookup
    size_t make_stun_request(uint8_t (&buffer)[20]);    // a binding request with a fresh transaction id
    bool is_stun_packet(const uint8_t* data, size_t size);
    // Reads the mapped address from a binding response to the last request; "a.b.c.d:port", or "" if not one.
    std::string parse_stun_response(const uint8_t* data, size_t size);
}
