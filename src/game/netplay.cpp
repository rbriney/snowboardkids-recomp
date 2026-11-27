// Lockstep netplay over UDP for up to four players. See netplay.h for configuration.
//
// Every machine runs the full game. The game's four controller ports are shared out between the machines in the
// session: the host takes the first ports (one per local player), and each joining machine takes the next ones in
// join order. At the start of game tick t each machine samples its local controllers and schedules them for tick
// t + delay. Tick t only runs once every port's input for tick t is known, so all machines feed identical inputs to
// identical game code (see the determinism harness results).
//
// Topology is a star: joiners talk only to the host, which relays each port's inputs to everyone else as they
// arrive. Each packet repeats every input the receiver has not acknowledged, so a lost packet needs no retransmission
// logic. Packets also carry a recent game-state hash for desync detection.
//
// Pauses: when the host has been waiting more than a moment for a joiner's input (or the joiner has gone quiet), it
// pauses the session. Every host packet carries the pause state, the players' statuses and who has pressed A, and the
// status overlay shows them on every machine. Play resumes once every connected player has pressed A. A joiner that
// disconnects can come back (from the same address) and catch up; the host can instead press Start to continue
// without it, in which case the host supplies neutral input for its ports from the first tick it has not yet relayed.
// Everyone still receives the same inputs, so nobody desyncs. Pausing only holds ticks; it never changes game state.
//
// Handshake: a joiner sends HELLO (its local player count and the save chunks it already has) until the host has sent
// WELCOME (its ports) and all 32 save chunks, then READY. Once every expected port is taken and every joiner is ready,
// the host sends START (the port owners) and play begins.

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "recompinput/players.h"
#include "ultramodern/ultramodern.hpp"
#include "netplay.h"
#include "determinism_harness.h"
#include "netplay_overlay.h"
#include "recompinput/profiles.h"

// RT64 (rt64.patch): keep presenting while the game is held, so the status overlay is drawn.
extern "C" void rt64_set_force_present(int enable);

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

using clock_type = std::chrono::steady_clock;

constexpr uint32_t packet_magic = 0x4E314B53; // "SK1N"
constexpr uint16_t protocol_version = 5; // 5: Away packets, no joiner timeout
constexpr uint32_t save_size = 0x8000;
constexpr uint32_t chunk_size = 1024;
constexpr uint32_t chunk_count = save_size / chunk_size;
constexpr uint32_t all_chunks = 0xFFFFFFFFu;
constexpr uint32_t port_count = 4;
constexpr uint32_t max_clients = 3;
constexpr uint32_t max_inputs_per_block = 48;
constexpr uint32_t hash_history = 512;
constexpr uint32_t no_tick = 0xFFFFFFFFu;
constexpr uint8_t host_id = 0;
constexpr uint8_t unassigned_id = 0xFF;
constexpr uint8_t no_owner = 0xFF;
constexpr auto pause_after = std::chrono::milliseconds(1500); // host pauses after waiting this long for a joiner
constexpr auto disconnected_after = std::chrono::seconds(4);  // a silent joiner is shown as disconnected
constexpr auto scripted_drop_after = std::chrono::seconds(3); // unattended tests: drop a disconnected joiner
constexpr auto resend_interval = std::chrono::milliseconds(15);
constexpr auto hello_interval = std::chrono::milliseconds(200);
constexpr uint32_t stats_interval = 900; // 30 seconds of game time
static_assert(chunk_count == 32);

enum PacketType : uint8_t {
    Hello = 1, Welcome = 2, SaveChunk = 3, Ready = 4, Start = 5, Input = 6, Full = 7,
    Discover = 8, // LAN discovery: anyone -> broadcast
    HostInfo = 9, // LAN discovery: a waiting host -> the asker
    Away = 10,    // the sender's app is going into the background (Android); it will be silent until it returns
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint8_t type;
    uint8_t sender; // host_id, a joiner's id (1-3), or unassigned_id before WELCOME
};
struct HelloPacket {
    Header header;
    uint8_t local_players;
    uint8_t padding[3];
    uint32_t have_mask;
};
struct WelcomePacket {
    Header header;
    uint8_t client_id;
    uint8_t first_port;
    uint8_t local_players;
    uint8_t total_ports;
    uint8_t delay;
    uint8_t padding[3];
};
struct SaveChunkPacket {
    Header header;
    uint8_t index;
    uint8_t padding[3];
    uint8_t data[chunk_size];
};
struct AwayPacket {
    Header header;
    uint32_t sequence; // the sender's last INPUT packet sequence; only newer INPUT packets mean it is back
};
struct HostInfoPacket {
    Header header;
    uint16_t port;
    uint8_t players_joined;
    uint8_t total_players;
};
struct StartPacket {
    Header header;
    uint8_t total_ports;
    uint8_t owners[port_count];
    uint8_t padding[3];
};
struct WireInput {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
};
struct InputBlockHeader {
    uint8_t port;
    uint8_t count;
    uint8_t padding[2];
    uint32_t first_tick;
};
struct InputPacketHeader {
    Header header;
    uint32_t sequence;         // per sender, increasing; older pause states are ignored
    uint32_t acks[port_count]; // the sender holds every input below acks[p] for port p
    uint32_t hash_tick;        // tick of state_hash, or no_tick
    uint32_t state_hash;
    uint8_t pause_id;          // host: current pause number; joiner: the pause its ready bits answer
    uint8_t paused;            // host only
    uint8_t ready_mask;        // ports whose player has pressed A during pause pause_id
    uint8_t port_states[port_count]; // host only: sk1net::PlayerState per port
    uint8_t block_count;
    // followed by block_count x (InputBlockHeader + count x WireInput)
};
#pragma pack(pop)

constexpr size_t max_packet_size =
    sizeof(InputPacketHeader) + port_count * (sizeof(InputBlockHeader) + max_inputs_per_block * sizeof(WireInput));
static_assert(max_packet_size < 1400, "input packets must fit in one UDP datagram");

struct PortStream {
    std::vector<WireInput> inputs; // indexed by tick
    std::vector<uint8_t> have;
    uint32_t next = 0; // every input below this tick is known

    void ensure(uint32_t t) {
        if (t >= inputs.size()) {
            inputs.resize(t + 1024);
            have.resize(t + 1024, 0);
        }
    }
    void set(uint32_t t, WireInput in) {
        ensure(t);
        if (!have[t]) {
            inputs[t] = in;
            have[t] = 1;
        }
        while (next < have.size() && have[next]) {
            next++;
        }
    }
    WireInput get(uint32_t t) const { return t < inputs.size() ? inputs[t] : WireInput{}; }
};

struct Client {
    bool used = false;
    sockaddr_in address{};
    uint8_t first_port = 0;
    uint8_t local_players = 1;
    bool ready = false;
    bool dropped = false;
    clock_type::time_point last_receive{};
    std::array<uint32_t, port_count> acks{}; // the joiner holds every input below acks[p]
    uint32_t last_sequence = 0;
    bool away = false;           // sent Away and has not sent a newer INPUT packet since
    uint32_t away_sequence = 0;  // its INPUT sequence number when it went away
};

struct Session {
    bool initialised = false;
    bool enabled = false;
    bool started = false;
    bool ended = false;
    bool is_host = false;
    uint8_t my_id = unassigned_id;
    uint8_t local_players = 1;
    uint8_t first_port = 0;
    uint8_t total_ports = 2;
    uint32_t delay = 3;
    std::array<uint8_t, port_count> owners{ no_owner, no_owner, no_owner, no_owner };

    socket_t sock = invalid_socket;
    sockaddr_in host_address{};
    clock_type::time_point last_host_receive{};
    std::array<uint32_t, port_count> host_acks{}; // joiner: the host holds every input below host_acks[p]
    std::array<Client, max_clients> clients{};
    uint8_t next_free_port = 0;
    uint16_t local_port = 0;
    std::string local_address_text; // host: "a.b.c.d:port" for the waiting screen

    std::mutex save_mutex;
    std::vector<uint8_t> save;
    uint32_t have_mask = 0;
    bool welcomed = false;

    std::array<PortStream, port_count> ports{};
    uint32_t tick = 0;

    std::array<uint32_t, hash_history> my_hash_tick{};
    std::array<uint32_t, hash_history> my_hash{};
    std::array<std::array<uint32_t, hash_history>, max_clients + 1> peer_hash_tick{};
    std::array<std::array<uint32_t, hash_history>, max_clients + 1> peer_hash{};
    uint32_t last_hash_tick = no_tick;
    bool desync_reported = false;

    FILE* log = nullptr;

    // Testing aids (see netplay.h).
    bool scripted = false;
    uint32_t sim_loss_percent = 0;
    uint32_t sim_latency_ms = 0;
    uint32_t sim_jitter_ms = 0;
    std::mt19937 sim_rng{ 12345 };
    struct DelayedPacket {
        clock_type::time_point due;
        sockaddr_in to;
        std::vector<uint8_t> data;
    };
    std::deque<DelayedPacket> sim_queue;

    // Pause state. The host owns it; joiners copy it from the host's packets.
    bool paused = false;
    uint8_t pause_id = 0;
    uint8_t ready_mask = 0; // ports ready during the current pause
    std::array<sk1net::PlayerState, port_count> port_states{};
    std::array<bool, port_count> a_was_down{};
    uint32_t sequence = 0;
    uint32_t host_last_sequence = 0;
    bool waiting_for_host = false; // joiner: shown when the host has been quiet a while
    bool host_away = false;        // joiner: the host sent Away and has not sent a newer INPUT packet since
    uint32_t host_away_sequence = 0;
    std::mutex status_mutex;
    sk1net::OverlayStatus status;

    // Statistics, logged every stats_interval ticks.
    std::chrono::microseconds stall_total{ 0 };
    std::chrono::microseconds stall_max{ 0 };
    uint32_t stalled_ticks = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
} session;

void log_msg(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "SK1net", "%s", buffer);
#else
    std::fprintf(stderr, "[net] %s\n", buffer);
#endif
    if (session.log != nullptr) {
        std::fprintf(session.log, "%s\n", buffer);
        std::fflush(session.log);
    }
}

// Player-facing notice: a message box on its own thread so the game never waits on it. Suppressed for scripted
// (unattended) runs, where nobody could dismiss it.
void notify_player(const std::string& message) {
    if (session.scripted) {
        return;
    }
    std::thread([message] {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Snowboard Kids netplay", message.c_str(), nullptr);
    }).detach();
}

std::string address_text(const sockaddr_in& a) {
    char text[64];
    inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(a.sin_port));
}

bool same_address(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// The sequence number of an INPUT packet, or 0 for anything else.
uint32_t input_sequence(const uint8_t* data, size_t size, const Header& h) {
    if (h.type != Input || size < sizeof(Header) + sizeof(uint32_t)) {
        return 0;
    }
    uint32_t sequence;
    std::memcpy(&sequence, data + sizeof(Header), sizeof(sequence));
    return sequence;
}

// Defined further down.
void publish_status();
int SDLCALL app_event_watch(void*, SDL_Event* event);

void fill_header(Header& h, PacketType type) {
    h.magic = packet_magic;
    h.version = protocol_version;
    h.type = type;
    h.sender = session.my_id;
}

void send_now(const sockaddr_in& to, const void* data, size_t size) {
    sendto(session.sock, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
        reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    session.packets_sent++;
}

// Sends delayed packets whose time has come (network simulation only).
void flush_delayed() {
    const auto now = clock_type::now();
    while (!session.sim_queue.empty() && session.sim_queue.front().due <= now) {
        const auto& p = session.sim_queue.front();
        send_now(p.to, p.data.data(), p.data.size());
        session.sim_queue.pop_front();
    }
}

void send_to(const sockaddr_in& to, const void* data, size_t size) {
    if (session.sim_loss_percent != 0 && (session.sim_rng() % 100) < session.sim_loss_percent) {
        return;
    }
    if (session.sim_latency_ms != 0 || session.sim_jitter_ms != 0) {
        uint32_t delay_ms = session.sim_latency_ms;
        if (session.sim_jitter_ms != 0) {
            delay_ms += session.sim_rng() % (session.sim_jitter_ms + 1);
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        Session::DelayedPacket packet{ clock_type::now() + std::chrono::milliseconds(delay_ms), to,
            std::vector<uint8_t>(bytes, bytes + size) };
        // Keep the queue ordered by due time (jitter can reorder packets, as a real network can).
        auto it = session.sim_queue.end();
        while (it != session.sim_queue.begin() && std::prev(it)->due > packet.due) {
            --it;
        }
        session.sim_queue.insert(it, std::move(packet));
        flush_delayed();
        return;
    }
    send_now(to, data, size);
}

bool owns_port(uint8_t id, uint32_t port) {
    return port < port_count && session.owners[port] == id;
}

bool is_local_port(uint32_t port) {
    return port >= session.first_port && port < uint32_t(session.first_port) + session.local_players;
}

// Builds and sends one INPUT packet: every port in send_ports, from the receiver's acknowledgement onwards.
void send_input_packet(const sockaddr_in& to, const std::array<uint32_t, port_count>& receiver_acks,
    const std::array<bool, port_count>& send_ports) {
    uint8_t buffer[max_packet_size];
    InputPacketHeader header{};
    fill_header(header.header, Input);
    header.sequence = ++session.sequence;
    for (uint32_t p = 0; p < port_count; p++) {
        header.acks[p] = session.ports[p].next;
        header.port_states[p] = static_cast<uint8_t>(session.port_states[p]);
    }
    header.pause_id = session.pause_id;
    header.paused = session.paused ? 1 : 0;
    header.ready_mask = session.ready_mask;
    header.hash_tick = session.last_hash_tick;
    header.state_hash =
        session.last_hash_tick == no_tick ? 0 : session.my_hash[session.last_hash_tick % hash_history];
    size_t offset = sizeof(InputPacketHeader);
    for (uint32_t p = 0; p < session.total_ports; p++) {
        if (!send_ports[p]) {
            continue;
        }
        const uint32_t first = std::max(receiver_acks[p], session.delay);
        const uint32_t end = session.ports[p].next; // exclusive
        if (end <= first) {
            continue;
        }
        const uint32_t count = std::min(end - first, max_inputs_per_block);
        InputBlockHeader block{};
        block.port = static_cast<uint8_t>(p);
        block.count = static_cast<uint8_t>(count);
        block.first_tick = first;
        std::memcpy(buffer + offset, &block, sizeof(block));
        offset += sizeof(block);
        for (uint32_t i = 0; i < count; i++) {
            const WireInput in = session.ports[p].get(first + i);
            std::memcpy(buffer + offset, &in, sizeof(in));
            offset += sizeof(in);
        }
        header.block_count++;
    }
    std::memcpy(buffer, &header, sizeof(header));
    send_to(to, buffer, offset);
}

void send_inputs() {
    if (session.is_host) {
        for (uint8_t c = 0; c < max_clients; c++) {
            const Client& client = session.clients[c];
            if (!client.used || client.dropped) {
                continue;
            }
            // Everything the joiner does not supply itself.
            std::array<bool, port_count> ports{};
            for (uint32_t p = 0; p < port_count; p++) {
                ports[p] = !owns_port(uint8_t(c + 1), p);
            }
            send_input_packet(client.address, client.acks, ports);
        }
    } else {
        std::array<bool, port_count> ports{};
        for (uint32_t p = 0; p < port_count; p++) {
            ports[p] = owns_port(session.my_id, p);
        }
        send_input_packet(session.host_address, session.host_acks, ports);
    }
}

void report_desync(uint32_t t, uint8_t peer) {
    if (session.desync_reported) {
        return;
    }
    session.desync_reported = true;
    log_msg("DESYNC: game state differs from %s at tick %u", peer == host_id ? "the host's" : "a joiner's", t);
    notify_player("The games have gone out of sync, so they may no longer show the same race. "
                  "Restart the games to play again.");
}

void check_hash(uint32_t t, uint8_t peer) {
    const uint32_t slot = t % hash_history;
    if (session.my_hash_tick[slot] != t || session.peer_hash_tick[peer][slot] != t) {
        return;
    }
    if (session.my_hash[slot] != session.peer_hash[peer][slot]) {
        report_desync(t, peer);
    }
}

void handle_input(const uint8_t* data, size_t size, uint8_t sender) {
    if (size < sizeof(InputPacketHeader)) {
        return;
    }
    InputPacketHeader header;
    std::memcpy(&header, data, sizeof(header));
    size_t offset = sizeof(header);
    for (uint32_t b = 0; b < header.block_count; b++) {
        if (offset + sizeof(InputBlockHeader) > size) {
            return;
        }
        InputBlockHeader block;
        std::memcpy(&block, data + offset, sizeof(block));
        offset += sizeof(block);
        if (offset + block.count * sizeof(WireInput) > size || block.port >= port_count) {
            return;
        }
        // Accept a port's input only from the machine entitled to send it: its owner, or the host relaying it.
        const bool allowed = !is_local_port(block.port) &&
            (session.is_host ? owns_port(sender, block.port) : sender == host_id);
        for (uint32_t i = 0; i < block.count; i++) {
            WireInput in;
            std::memcpy(&in, data + offset, sizeof(in));
            offset += sizeof(in);
            if (allowed) {
                session.ports[block.port].set(block.first_tick + i, in);
            }
        }
    }
    if (session.is_host) {
        if (sender >= 1 && sender <= max_clients) {
            Client& client = session.clients[sender - 1];
            for (uint32_t p = 0; p < port_count; p++) {
                client.acks[p] = std::max(client.acks[p], header.acks[p]);
            }
            // A joiner's A presses count only for the pause they answer, and only for its own ports.
            if (session.paused && header.pause_id == session.pause_id) {
                for (uint32_t p = 0; p < port_count; p++) {
                    if ((header.ready_mask & (1u << p)) && owns_port(sender, p)) {
                        session.ready_mask |= uint8_t(1u << p);
                    }
                }
            }
        }
    } else {
        for (uint32_t p = 0; p < port_count; p++) {
            session.host_acks[p] = std::max(session.host_acks[p], header.acks[p]);
        }
        if (header.sequence > session.host_last_sequence) {
            session.host_last_sequence = header.sequence;
            if (header.pause_id != session.pause_id) {
                // A new pause: our own A presses must be fresh.
                session.ready_mask = 0;
                session.a_was_down.fill(true);
            }
            session.pause_id = header.pause_id;
            session.paused = header.paused != 0;
            // Others' ready bits come from the host; keep our own until the host has them.
            const uint8_t local_mask = uint8_t(((1u << session.local_players) - 1) << session.first_port);
            session.ready_mask = uint8_t((header.ready_mask & ~local_mask) | (session.ready_mask & local_mask));
            for (uint32_t p = 0; p < port_count; p++) {
                session.port_states[p] = static_cast<sk1net::PlayerState>(header.port_states[p] & 7);
            }
        }
    }
    if (header.hash_tick != no_tick && sender <= max_clients) {
        const uint32_t slot = header.hash_tick % hash_history;
        session.peer_hash_tick[sender][slot] = header.hash_tick;
        session.peer_hash[sender][slot] = header.state_hash;
        check_hash(header.hash_tick, sender);
    }
}

void send_welcome(uint8_t client_index, uint32_t have_mask) {
    const Client& client = session.clients[client_index];
    WelcomePacket w{};
    fill_header(w.header, Welcome);
    w.client_id = uint8_t(client_index + 1);
    w.first_port = client.first_port;
    w.local_players = client.local_players;
    w.total_ports = session.total_ports;
    w.delay = static_cast<uint8_t>(session.delay);
    send_to(client.address, &w, sizeof(w));
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (have_mask & (1u << i)) {
            continue;
        }
        SaveChunkPacket c{};
        fill_header(c.header, SaveChunk);
        c.index = static_cast<uint8_t>(i);
        std::memcpy(c.data, session.save.data() + i * chunk_size, chunk_size);
        send_to(client.address, &c, sizeof(c));
    }
}

void send_start(const sockaddr_in& to) {
    StartPacket s{};
    fill_header(s.header, Start);
    s.total_ports = session.total_ports;
    std::memcpy(s.owners, session.owners.data(), port_count);
    send_to(to, &s, sizeof(s));
}

void start_session() {
    session.started = true;
    std::string layout;
    for (uint32_t p = 0; p < session.total_ports; p++) {
        layout += " P" + std::to_string(p + 1) + "=" +
            (session.owners[p] == host_id ? std::string("host") : "joiner " + std::to_string(session.owners[p]));
    }
    log_msg("session started: %u players, input delay %u ticks;%s", session.total_ports, session.delay,
        layout.c_str());
    // Several players on this machine need their controllers assigned to its ports.
    if (session.local_players > 1) {
        recompinput::players::request_game_player_count(session.local_players);
    }
}

void host_process(const uint8_t* data, size_t size, const Header& h, const sockaddr_in& from) {
    // Find the joiner by address.
    int index = -1;
    for (uint8_t c = 0; c < max_clients; c++) {
        if (session.clients[c].used && same_address(session.clients[c].address, from)) {
            index = c;
            break;
        }
    }
    if (h.type == Hello && size >= sizeof(HelloPacket)) {
        HelloPacket p;
        std::memcpy(&p, data, sizeof(p));
        if (index < 0) {
            const uint8_t wanted = std::clamp<uint8_t>(p.local_players, 1, port_count);
            int free_slot = -1;
            for (uint8_t c = 0; c < max_clients; c++) {
                if (!session.clients[c].used) {
                    free_slot = c;
                    break;
                }
            }
            if (session.started || free_slot < 0 || session.next_free_port + wanted > session.total_ports) {
                Header full;
                fill_header(full, Full);
                send_to(from, &full, sizeof(full));
                return;
            }
            Client& client = session.clients[free_slot];
            client = Client{};
            client.used = true;
            client.address = from;
            client.first_port = session.next_free_port;
            client.local_players = wanted;
            for (uint32_t i = 0; i < wanted; i++) {
                session.owners[client.first_port + i] = uint8_t(free_slot + 1);
            }
            session.next_free_port += wanted;
            index = free_slot;
            log_msg("joiner %d connected from %s with %u player(s): ports %u-%u", index + 1,
                address_text(from).c_str(), wanted, client.first_port + 1, client.first_port + wanted);
        }
        session.clients[index].last_receive = clock_type::now();
        send_welcome(uint8_t(index), p.have_mask);
        return;
    }
    if (index < 0 || h.sender != index + 1) {
        return;
    }
    Client& client = session.clients[index];
    client.last_receive = clock_type::now();
    if (h.type == Away) {
        if (!client.away && size >= sizeof(AwayPacket)) {
            AwayPacket away;
            std::memcpy(&away, data, sizeof(away));
            client.away = true;
            client.away_sequence = away.sequence;
            log_msg("joiner %d switched to another app", index + 1);
        }
        return;
    }
    // Only an INPUT packet sent after the Away means the joiner is back (older ones can arrive late).
    if (client.away && input_sequence(data, size, h) > client.away_sequence) {
        client.away = false;
        log_msg("joiner %d is back", index + 1);
    }
    switch (h.type) {
        case Ready:
            if (!client.ready) {
                client.ready = true;
                log_msg("joiner %d ready", index + 1);
            }
            if (session.started) {
                send_start(client.address);
            }
            break;
        case Input:
            if (session.started && !client.dropped) {
                handle_input(data, size, h.sender);
            }
            break;
    }
}

void joiner_process(const uint8_t* data, size_t size, const Header& h, const sockaddr_in& from) {
    if (!same_address(from, session.host_address) || h.sender != host_id) {
        return;
    }
    session.last_host_receive = clock_type::now();
    if (h.type == Away) {
        if (!session.host_away && size >= sizeof(AwayPacket)) {
            AwayPacket away;
            std::memcpy(&away, data, sizeof(away));
            session.host_away = true;
            session.host_away_sequence = away.sequence;
            log_msg("the host switched to another app");
            publish_status();
        }
        return;
    }
    // Only an INPUT packet sent after the Away means the host is back (older ones can arrive late).
    if (session.host_away && input_sequence(data, size, h) > session.host_away_sequence) {
        session.host_away = false;
        log_msg("the host is back");
        publish_status();
    }
    switch (h.type) {
        case Welcome:
            if (size >= sizeof(WelcomePacket)) {
                WelcomePacket p;
                std::memcpy(&p, data, sizeof(p));
                if (!session.welcomed) {
                    session.welcomed = true;
                    session.my_id = p.client_id;
                    session.first_port = p.first_port;
                    session.local_players = p.local_players;
                    session.total_ports = std::clamp<uint8_t>(p.total_ports, 2, port_count);
                    session.delay = p.delay;
                    log_msg("welcomed as joiner %u: ports %u-%u of %u, input delay %u", p.client_id,
                        p.first_port + 1, p.first_port + p.local_players, session.total_ports, session.delay);
                }
            }
            break;
        case SaveChunk:
            if (size >= sizeof(SaveChunkPacket)) {
                const SaveChunkPacket* p = reinterpret_cast<const SaveChunkPacket*>(data);
                if (p->index < chunk_count && !(session.have_mask & (1u << p->index))) {
                    std::lock_guard lock{ session.save_mutex };
                    std::memcpy(session.save.data() + p->index * chunk_size, p->data, chunk_size);
                    session.have_mask |= 1u << p->index;
                }
            }
            break;
        case Start:
            if (size >= sizeof(StartPacket) && session.welcomed && session.have_mask == all_chunks &&
                !session.started) {
                StartPacket p;
                std::memcpy(&p, data, sizeof(p));
                session.total_ports = std::clamp<uint8_t>(p.total_ports, 2, port_count);
                std::memcpy(session.owners.data(), p.owners, port_count);
                start_session();
            }
            break;
        case Input:
            if (session.started) {
                handle_input(data, size, h.sender);
            }
            break;
        case Full:
            if (!session.welcomed && !session.ended) {
                session.ended = true;
                log_msg("the host's session is full or already running");
                notify_player("Could not join: the host's netplay session is full or has already started.");
            }
            break;
    }
}

void process_packet(const uint8_t* data, size_t size, const sockaddr_in& from) {
    if (size < sizeof(Header)) {
        return;
    }
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != packet_magic) {
        return;
    }
    if (h.version != protocol_version) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            log_msg("ignoring packets from %s: different netplay version (%u, this is %u)",
                address_text(from).c_str(), h.version, protocol_version);
        }
        return;
    }
    if (h.type == Discover) {
        // Only a host that is still waiting for players advertises itself.
        if (session.is_host && !session.started) {
            HostInfoPacket info{};
            fill_header(info.header, HostInfo);
            info.port = session.local_port;
            info.players_joined = session.next_free_port;
            info.total_players = session.total_ports;
            send_now(from, &info, sizeof(info));
        }
        return;
    }
    if (session.is_host) {
        host_process(data, size, h, from);
    } else {
        joiner_process(data, size, h, from);
    }
}

// Receive and process packets, waiting up to timeout for the first one.
void pump(std::chrono::milliseconds timeout) {
    flush_delayed();
    if (!session.sim_queue.empty()) {
        const auto until_due = std::chrono::duration_cast<std::chrono::milliseconds>(
            session.sim_queue.front().due - clock_type::now());
        timeout = std::max(std::chrono::milliseconds(0), std::min(timeout, until_due));
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(session.sock, &fds);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    if (select(static_cast<int>(session.sock) + 1, &fds, nullptr, nullptr, &tv) <= 0) {
        return;
    }
    uint8_t buffer[2048];
    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int received = recvfrom(session.sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) {
            break;
        }
        session.packets_received++;
        process_packet(buffer, static_cast<size_t>(received), from);
    }
}

bool set_non_blocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void open_log(const std::string& name) {
    std::filesystem::path folder = std::filesystem::current_path();
    if (const char* app = std::getenv("APP_FOLDER_PATH"); app != nullptr && *app != '\0') {
        folder = std::filesystem::u8path(app);
    }
    session.log = std::fopen((folder / name).string().c_str(), "w");
}

uint32_t env_uint(const char* name, uint32_t min_value, uint32_t max_value, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::clamp<uint32_t>(static_cast<uint32_t>(std::max(std::atoi(value), 0)), min_value, max_value);
}

void init() {
    if (session.initialised) {
        return;
    }
    session.initialised = true;

    const char* host = std::getenv("SK1_NET_HOST");
    const char* join = std::getenv("SK1_NET_JOIN");
    const bool hosting = host != nullptr && *host != '\0';
    const bool joining = join != nullptr && *join != '\0';
    if (!hosting && !joining) {
        return;
    }
    // A log per role and instance, so several copies on one machine do not share one.
    const char* instance = std::getenv("SK1_NET_INSTANCE");
    open_log(std::string(hosting ? "netplay-host" : "netplay-join") +
        (instance != nullptr && *instance != '\0' ? std::string("-") + instance : std::string()) + ".log");

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_msg("WSAStartup failed; netplay disabled");
        return;
    }
#endif
    session.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (session.sock == invalid_socket || !set_non_blocking(session.sock)) {
        log_msg("cannot create a UDP socket; netplay disabled");
        return;
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" (e.g. the host not running yet) makes later receives fail.
    BOOL report_reset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(session.sock, _WSAIOW(IOC_VENDOR, 12), &report_reset, sizeof(report_reset), nullptr, 0, &bytes, nullptr,
        nullptr);
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (const char* bind_address = std::getenv("SK1_NET_BIND"); bind_address != nullptr && *bind_address != 0) {
        inet_pton(AF_INET, bind_address, &local.sin_addr);
    }
    session.local_players = static_cast<uint8_t>(env_uint("SK1_NET_LOCAL_PLAYERS", 1, port_count, 1));
    if (hosting) {
        session.is_host = true;
        session.my_id = host_id;
        session.first_port = 0;
        local.sin_port = htons(static_cast<uint16_t>(std::atoi(host)));
        session.local_port = static_cast<uint16_t>(std::atoi(host));
        session.delay = env_uint("SK1_NET_DELAY", 1, 30, 3);
        const uint32_t minimum = std::min<uint32_t>(session.local_players + 1, port_count);
        session.total_ports = static_cast<uint8_t>(env_uint("SK1_NET_PLAYERS", minimum, port_count, minimum));
        for (uint32_t i = 0; i < session.local_players; i++) {
            session.owners[i] = host_id;
        }
        session.next_free_port = session.local_players;
    } else {
        session.is_host = false;
        std::string target = join;
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos) {
            log_msg("SK1_NET_JOIN must be <address>:<port>; netplay disabled");
            return;
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(target.substr(0, colon).c_str(), target.substr(colon + 1).c_str(), &hints, &result) != 0 ||
            result == nullptr) {
            log_msg("cannot resolve %s; netplay disabled", join);
            return;
        }
        std::memcpy(&session.host_address, result->ai_addr, sizeof(session.host_address));
        freeaddrinfo(result);
    }
    if (bind(session.sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        log_msg("cannot bind UDP port %u; netplay disabled", ntohs(local.sin_port));
        return;
    }

    session.scripted = env_uint("SK1_NET_SCRIPT", 0, 1, 0) != 0;
    session.sim_loss_percent = env_uint("SK1_NET_SIM_LOSS", 0, 90, 0);
    session.sim_latency_ms = env_uint("SK1_NET_SIM_LATENCY", 0, 2000, 0);
    session.sim_jitter_ms = env_uint("SK1_NET_SIM_JITTER", 0, 2000, 0);
    session.sim_rng.seed(hosting ? 1 : 2 + static_cast<uint32_t>(std::atoi(instance != nullptr ? instance : "0")));
    if (session.scripted) {
        log_msg("scripted input: playing this machine's ports from the replayed recording");
    }
    if (session.sim_loss_percent != 0 || session.sim_latency_ms != 0 || session.sim_jitter_ms != 0) {
        log_msg("simulating %u%% outgoing packet loss, %u ms latency, up to %u ms jitter", session.sim_loss_percent,
            session.sim_latency_ms, session.sim_jitter_ms);
    }

    session.save.assign(save_size, 0);
    // Ticks below the input delay have no scheduled input: every port is neutral there.
    for (PortStream& stream : session.ports) {
        for (uint32_t t = 0; t < session.delay; t++) {
            stream.set(t, WireInput{});
        }
    }
    session.my_hash_tick.fill(no_tick);
    for (auto& ticks : session.peer_hash_tick) {
        ticks.fill(no_tick);
    }
    session.enabled = true;
    SDL_AddEventWatch(app_event_watch, nullptr);
    if (hosting) {
        session.local_address_text = sk1net::local_address() + ":" + std::to_string(session.local_port);
    }
    sk1net_overlay::install();
    if (hosting) {
        log_msg("hosting on UDP port %u with %u local player(s); waiting for %u more player(s)",
            ntohs(local.sin_port), session.local_players, session.total_ports - session.local_players);
    } else {
        log_msg("joining %s with %u local player(s)", join, session.local_players);
    }
}

void publish_status();

void end_session(const char* reason) {
    if (!session.ended) {
        session.ended = true;
        session.paused = false;
        session.waiting_for_host = false;
        publish_status(); // hides the overlay and stops forced presents
        log_msg("session ended: %s", reason);
        notify_player(std::string("Netplay session ended: ") + reason +
                      ". The game continues offline; restart the games to play together again.");
    }
}

// Host: stop waiting for a joiner that has gone quiet. From the first tick not yet relayed for its ports, the host
// supplies neutral input for them and relays that like any other input, so every machine stays in step.
void drop_client(uint8_t index) {
    Client& client = session.clients[index];
    if (client.dropped) {
        return;
    }
    client.dropped = true;
    for (uint32_t i = 0; i < client.local_players; i++) {
        session.port_states[client.first_port + i] = sk1net::PlayerState::Dropped;
    }
    log_msg("continuing without joiner %u: neutral input for ports %u-%u", index + 1, client.first_port + 1,
        client.first_port + client.local_players);
}

// Android pauses the whole game while the app is in the background, so it goes silent. Tell the others first, so they
// can show that this player switched away rather than lost their connection. SDL calls this event watch synchronously
// from the thread that delivers the app event, just before the game is frozen.
// Set when the host's app comes back from the background; the game thread then pauses the session, so that every
// player confirms with A before the race continues.
std::atomic<bool> pause_after_returning{ false };

int SDLCALL app_event_watch(void*, SDL_Event* event) {
    if (!session.enabled || session.ended) {
        return 0;
    }
    if (event->type == SDL_APP_DIDENTERFOREGROUND) {
        if (session.is_host && session.started) {
            pause_after_returning = true;
        }
        return 0;
    }
    if (event->type != SDL_APP_WILLENTERBACKGROUND) {
        return 0;
    }
    AwayPacket away{};
    fill_header(away.header, Away);
    away.sequence = session.sequence;
    // A few copies: there is no chance to resend once the app is frozen.
    for (int i = 0; i < 3; i++) {
        if (session.is_host) {
            for (const Client& client : session.clients) {
                if (client.used && !client.dropped) {
                    send_now(client.address, &away, sizeof(away));
                }
            }
        } else if (session.welcomed) {
            send_now(session.host_address, &away, sizeof(away));
        }
    }
    log_msg("switching to another app: told the other players");
    return 0;
}

// Neutral input for ports whose joiner has dropped, up to the newest tick this machine has scheduled.
void fill_dropped_ports(uint32_t until) {
    for (uint8_t c = 0; c < max_clients; c++) {
        const Client& client = session.clients[c];
        if (!client.used || !client.dropped) {
            continue;
        }
        for (uint32_t i = 0; i < client.local_players; i++) {
            PortStream& stream = session.ports[client.first_port + i];
            while (stream.next <= until) {
                stream.set(stream.next, WireInput{});
            }
        }
    }
}

void publish_status();

// Blocks the game thread until the handshake completes (at the first game tick).
void wait_for_start() {
    if (session.is_host) {
        // The host's save is the session save (when replaying a recording for a test, the save it started from).
        std::vector<uint8_t> recorded;
        if (sk1det::replay_save(recorded)) {
            std::lock_guard lock{ session.save_mutex };
            session.save = recorded;
        } else {
            std::ifstream in(ultramodern::get_save_file_path(), std::ios::binary);
            if (in) {
                std::lock_guard lock{ session.save_mutex };
                in.read(reinterpret_cast<char*>(session.save.data()), save_size);
            }
        }
        session.have_mask = all_chunks;
    }
    auto last_send = clock_type::time_point{};
    auto last_log = clock_type::now();
    while (!session.started && !session.ended) {
        const auto now = clock_type::now();
        if (session.is_host) {
            bool all_ready = session.next_free_port == session.total_ports;
            for (const Client& client : session.clients) {
                if (client.used && !client.ready) {
                    all_ready = false;
                }
            }
            if (all_ready) {
                start_session();
                for (const Client& client : session.clients) {
                    if (client.used) {
                        send_start(client.address);
                    }
                }
                break;
            }
        } else if (now - last_send >= hello_interval) {
            last_send = now;
            if (session.welcomed && session.have_mask == all_chunks) {
                Header r;
                fill_header(r, Ready);
                send_to(session.host_address, &r, sizeof(r));
            } else {
                HelloPacket p{};
                fill_header(p.header, Hello);
                p.local_players = session.local_players;
                p.have_mask = session.have_mask;
                send_to(session.host_address, &p, sizeof(p));
            }
        }
        static auto last_publish = clock_type::time_point{};
        if (now - last_publish >= std::chrono::milliseconds(250)) {
            last_publish = now;
            publish_status();
        }
        if (now - last_log >= std::chrono::seconds(5)) {
            last_log = now;
            if (session.is_host) {
                log_msg("still waiting: %u of %u players connected", session.next_free_port, session.total_ports);
            } else {
                log_msg("still waiting for the host");
            }
        }
        pump(std::chrono::milliseconds(20));
    }
    const auto now = clock_type::now();
    session.last_host_receive = now;
    for (Client& client : session.clients) {
        client.last_receive = now;
    }
    publish_status();
}

// Copies the state the overlay needs into its mutex-protected snapshot (read on the UI thread).
void publish_status() {
    sk1net::OverlayStatus status;
    status.paused = session.paused;
    status.waiting_to_start = !session.started;
    status.host_away = session.host_away;
    status.visible = session.enabled && !session.ended &&
        (!session.started || session.paused || session.waiting_for_host);
    status.players_joined = session.next_free_port;
    if (session.is_host) {
        status.address = session.local_address_text;
    } else {
        status.address = address_text(session.host_address);
    }
    status.am_host = session.is_host;
    status.total_ports = session.total_ports;
    for (uint32_t p = 0; p < port_count; p++) {
        sk1net::PlayerStatus& player = status.players[p];
        player.host = session.owners[p] == host_id;
        player.local = is_local_port(p);
        player.state = session.port_states[p];
        // A joiner knows more about the host's own players than the host's last packet said.
        if (!session.is_host && player.host && session.started && !session.paused && session.waiting_for_host) {
            player.state = session.host_away ? sk1net::PlayerState::Away : sk1net::PlayerState::Lagging;
        }
        player.ready = (session.ready_mask & (1u << p)) != 0;
        if (player.state == sk1net::PlayerState::Disconnected || player.state == sk1net::PlayerState::Away) {
            status.can_continue_without = true;
        }
    }
    static bool forcing_present = false;
    if (status.visible != forcing_present) {
        forcing_present = status.visible;
        rt64_set_force_present(forcing_present ? 1 : 0);
        log_msg("overlay %s", forcing_present ? "shown (forcing presents)" : "hidden");
    }
    std::lock_guard lock{ session.status_mutex };
    session.status = status;
}

// Edge-detected button press on one of this machine's controllers (index = local player).
bool local_button_pressed(uint32_t local_index, uint16_t mask) {
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
    recompinput::profiles::get_n64_input(static_cast<int>(local_index), &buttons, &x, &y);
    const bool down = (buttons & mask) != 0;
    const bool pressed = down && !session.a_was_down[local_index];
    session.a_was_down[local_index] = down;
    return pressed;
}

constexpr uint16_t n64_button_a = 0x8000;
constexpr uint16_t n64_button_start = 0x1000;

// Host: refresh each joiner's displayed state from how recently it was heard from and whether it is behind.
void host_update_port_states(uint32_t needed_tick) {
    const auto now = clock_type::now();
    for (uint8_t c = 0; c < max_clients; c++) {
        const Client& client = session.clients[c];
        if (!client.used || client.dropped) {
            continue;
        }
        bool behind = false;
        for (uint32_t i = 0; i < client.local_players; i++) {
            behind |= session.ports[client.first_port + i].next <= needed_tick;
        }
        sk1net::PlayerState state = sk1net::PlayerState::Connected;
        if (client.away) {
            state = sk1net::PlayerState::Away;
        } else if (now - client.last_receive > disconnected_after) {
            state = sk1net::PlayerState::Disconnected;
        } else if (behind && now - client.last_receive > std::chrono::milliseconds(500)) {
            state = sk1net::PlayerState::Lagging;
        }
        for (uint32_t i = 0; i < client.local_players; i++) {
            session.port_states[client.first_port + i] = state;
        }
    }
}

void host_begin_pause(const char* reason) {
    if (session.paused) {
        return;
    }
    session.paused = true;
    session.pause_id++;
    session.ready_mask = 0;
    session.a_was_down.fill(true); // a button already held does not count as ready
    log_msg("paused (%s)", reason);
    publish_status();
    send_inputs();
}

// Holds this machine's next tick while the session is paused: keeps the network going, collects this machine's A
// presses (and, on the host, Start to continue without disconnected players) and resumes when the host says so.
void hold_while_paused(uint32_t needed_tick) {
    auto last_send = clock_type::time_point{};
    auto disconnected_since = clock_type::time_point{};
    while (session.paused && !session.ended) {
        pump(std::chrono::milliseconds(5));
        const auto now = clock_type::now();
        for (uint32_t i = 0; i < session.local_players; i++) {
            const uint32_t port = session.first_port + i;
            if (session.scripted || local_button_pressed(i, n64_button_a)) {
                session.ready_mask |= uint8_t(1u << port);
            }
        }
        if (session.is_host) {
            host_update_port_states(needed_tick);
            bool any_disconnected = false;
            bool all_ready = true;
            for (uint32_t p = 0; p < session.total_ports; p++) {
                const sk1net::PlayerState state = session.port_states[p];
                if (state == sk1net::PlayerState::Dropped) {
                    continue;
                }
                if (state == sk1net::PlayerState::Disconnected || state == sk1net::PlayerState::Away) {
                    any_disconnected = true;
                    all_ready = false;
                } else if (!(session.ready_mask & (1u << p))) {
                    all_ready = false;
                }
            }
            if (any_disconnected) {
                if (disconnected_since == clock_type::time_point{}) {
                    disconnected_since = now;
                }
                const bool host_chose = !session.scripted && local_button_pressed(0, n64_button_start);
                const bool test_timeout = session.scripted && now - disconnected_since > scripted_drop_after;
                if (host_chose || test_timeout) {
                    for (uint8_t c = 0; c < max_clients; c++) {
                        const Client& client = session.clients[c];
                        if (client.used && !client.dropped &&
                            (session.port_states[client.first_port] == sk1net::PlayerState::Disconnected ||
                             session.port_states[client.first_port] == sk1net::PlayerState::Away)) {
                            drop_client(c);
                        }
                    }
                    fill_dropped_ports(needed_tick + session.delay);
                }
            } else {
                disconnected_since = {};
            }
            if (all_ready) {
                session.paused = false;
                log_msg("resumed: every connected player is ready");
            }
        } else if (now - session.last_host_receive > pause_after && local_button_pressed(0, n64_button_start)) {
            // The host is unreachable: the joiner may stop waiting and play on alone.
            end_session("you stopped waiting for the host");
        }
        if (now - last_send >= resend_interval) {
            last_send = now;
            send_inputs();
        }
        publish_status();
    }
    // Make sure everyone hears about the resume promptly.
    send_inputs();
    publish_status();
}

bool all_inputs_ready(uint32_t t) {
    for (uint32_t p = 0; p < session.total_ports; p++) {
        if (session.ports[p].next <= t) {
            return false;
        }
    }
    return true;
}

} // namespace

// s32 recomp_get_netplay_view_port(void): with SK1_NET_OWN_VIEW=1 and one player on this device, the port this device
// shows full screen in multiplayer races (patches/widescreen.c); otherwise -1 (normal split screen).
extern "C" void recomp_get_netplay_view_port(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = env_uint("SK1_NET_OWN_VIEW", 0, 1, 0) != 0;
    const bool own_view = enabled && session.enabled && session.started && !session.ended &&
        session.local_players == 1;
    _return<s32>(ctx, own_view ? static_cast<s32>(session.first_port) : -1);
}

sk1net::OverlayStatus sk1net::overlay_status() {
    std::lock_guard lock{ session.status_mutex };
    sk1net::OverlayStatus status = session.status;
    if (session.ended) {
        status.visible = false;
    }
    return status;
}

int sk1net::local_player_for_port(int port) {
    if (!session.enabled || !session.started || session.ended) {
        return port;
    }
    if (port < 0 || !is_local_port(static_cast<uint32_t>(port))) {
        return -1;
    }
    return port - session.first_port;
}

bool sk1net::active() {
    init();
    return session.enabled && !session.ended;
}

void sk1net::pre_tick(uint8_t* rdram, gpr inputs) {
    if (!active()) {
        return;
    }
    if (!session.started) {
        wait_for_start();
        if (!session.started) {
            return;
        }
    }

    if (session.is_host && pause_after_returning.exchange(false)) {
        host_begin_pause("the host came back from another app");
    }
    if (session.paused) {
        hold_while_paused(session.tick);
    }

    // Sample this machine's controllers (the game's ports 0.. locally) and schedule them.
    const uint32_t scheduled = session.tick + session.delay;
    for (uint32_t i = 0; i < session.local_players; i++) {
        const uint32_t port = session.first_port + i;
        WireInput mine{};
        if (session.scripted) {
            sk1det::recorded_input(scheduled, port, mine.buttons, mine.stick_x, mine.stick_y);
        } else {
            const gpr addr = inputs + i * 6;
            mine.buttons = MEM_HU(0, addr);
            mine.stick_x = MEM_B(2, addr);
            mine.stick_y = MEM_B(3, addr);
        }
        session.ports[port].set(scheduled, mine);
    }
    if (session.is_host) {
        fill_dropped_ports(scheduled);
    }

    send_inputs();
    const auto wait_start = clock_type::now();
    auto last_send = wait_start;
    while (!all_inputs_ready(session.tick)) {
        pump(std::chrono::milliseconds(5));
        const auto now = clock_type::now();
        if (session.paused) {
            // The host paused while we were waiting (joiner), or we paused below (host).
            hold_while_paused(session.tick);
            continue;
        }
        if (session.is_host) {
            if (now - wait_start > pause_after) {
                host_update_port_states(session.tick);
                host_begin_pause("waiting for a player");
                continue;
            }
        } else {
            const bool waiting = now - session.last_host_receive > pause_after;
            if (waiting != session.waiting_for_host) {
                session.waiting_for_host = waiting;
                if (waiting) {
                    session.a_was_down.fill(true); // a Start already held does not count as leaving
                }
                publish_status();
            }
            // No timeout: the joiner waits as long as it likes, and Start leaves the session.
            if (waiting && local_button_pressed(0, n64_button_start)) {
                end_session("you stopped waiting for the host");
                return;
            }
        }
        if (now - last_send >= resend_interval) {
            last_send = now;
            send_inputs();
        }
    }
    if (session.waiting_for_host) {
        session.waiting_for_host = false;
        publish_status();
    }
    const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - wait_start);
    if (waited > std::chrono::milliseconds(1)) {
        session.stalled_ticks++;
        session.stall_total += waited;
        session.stall_max = std::max(session.stall_max, waited);
    }
    // Relay what has just arrived without waiting for the next tick, then drain anything else pending.
    if (session.is_host) {
        send_inputs();
    }
    pump(std::chrono::milliseconds(0));

    for (uint32_t port = 0; port < port_count; port++) {
        WireInput in{};
        if (port < session.total_ports) {
            in = session.ports[port].get(session.tick);
        } else if (session.scripted) {
            // Unused ports: neutral, or (in a scripted test) the recording's inputs, identical on every machine.
            sk1det::recorded_input(session.tick, port, in.buttons, in.stick_x, in.stick_y);
        }
        const gpr addr = inputs + port * 6;
        MEM_H(0, addr) = in.buttons;
        MEM_B(2, addr) = in.stick_x;
        MEM_B(3, addr) = in.stick_y;
    }
}

void sk1net::post_tick(uint32_t state_hash) {
    if (!active() || !session.started) {
        return;
    }
    const uint32_t slot = session.tick % hash_history;
    session.my_hash_tick[slot] = session.tick;
    session.my_hash[slot] = state_hash;
    session.last_hash_tick = session.tick;
    for (uint8_t peer = 0; peer <= max_clients; peer++) {
        check_hash(session.tick, peer);
    }
    if (session.tick != 0 && session.tick % stats_interval == 0) {
        log_msg("tick %u: stalled on %u of the last %u ticks, %.1f ms total, longest %.1f ms; packets sent %llu, "
                "received %llu", session.tick, session.stalled_ticks, stats_interval,
            session.stall_total.count() / 1000.0, session.stall_max.count() / 1000.0,
            static_cast<unsigned long long>(session.packets_sent),
            static_cast<unsigned long long>(session.packets_received));
        session.stalled_ticks = 0;
        session.stall_total = std::chrono::microseconds(0);
        session.stall_max = std::chrono::microseconds(0);
    }
    session.tick++;
}

bool sk1net::save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size) {
    init();
    if (!session.enabled) {
        return false;
    }
    if (!session.started && !session.ended) {
        wait_for_start();
    }
    std::lock_guard lock{ session.save_mutex };
    for (uint32_t i = 0; i < size; i++) {
        MEM_B(i, dst) = session.save[offset + i];
    }
    return true;
}

bool sk1net::session_save(std::vector<uint8_t>& out) {
    if (!session.enabled || !session.started) {
        return false;
    }
    std::lock_guard lock{ session.save_mutex };
    out = session.save;
    return true;
}

bool sk1net::save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size) {
    // Saves stay in memory for the rest of the process, even after a disconnect.
    init();
    if (!session.enabled) {
        return false;
    }
    std::lock_guard lock{ session.save_mutex };
    for (uint32_t i = 0; i < size; i++) {
        session.save[offset + i] = MEM_B(i, src);
    }
    return true;
}

// ---- LAN discovery (lobby) and local address ----

std::string sk1net::local_address() {
    // Ask the OS which local address it would use to reach a public address. No packet is sent.
#ifdef _WIN32
    static bool wsa_ready = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!wsa_ready) {
        return "";
    }
#endif
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == invalid_socket) {
        return "";
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
    std::string result;
    if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char text[64];
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
            result = text;
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return result;
}

namespace {

struct Discovery {
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running{ false };
    struct Entry {
        sk1net::DiscoveredHost host;
        clock_type::time_point seen;
    };
    std::vector<Entry> hosts;
} discovery;

void discovery_thread(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == invalid_socket || !set_non_blocking(s)) {
        return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local));

    sockaddr_in broadcast{};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(port);
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sockaddr_in loopback = broadcast; // a host on this same machine
    inet_pton(AF_INET, "127.0.0.1", &loopback.sin_addr);

    auto last_ask = clock_type::time_point{};
    while (discovery.running) {
        const auto now = clock_type::now();
        if (now - last_ask >= std::chrono::seconds(1)) {
            last_ask = now;
            Header ask{};
            ask.magic = packet_magic;
            ask.version = protocol_version;
            ask.type = Discover;
            ask.sender = unassigned_id;
            sendto(s, reinterpret_cast<const char*>(&ask), sizeof(ask), 0,
                reinterpret_cast<const sockaddr*>(&broadcast), sizeof(broadcast));
            sendto(s, reinterpret_cast<const char*>(&ask), sizeof(ask), 0,
                reinterpret_cast<const sockaddr*>(&loopback), sizeof(loopback));
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{ 0, 100000 };
        if (select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &tv) > 0) {
            uint8_t buffer[256];
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const int received = recvfrom(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received >= static_cast<int>(sizeof(HostInfoPacket))) {
                HostInfoPacket info;
                std::memcpy(&info, buffer, sizeof(info));
                if (info.header.magic == packet_magic && info.header.version == protocol_version &&
                    info.header.type == HostInfo) {
                    char text[64];
                    inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
                    sk1net::DiscoveredHost host;
                    host.address = std::string(text) + ":" + std::to_string(info.port);
                    host.players_joined = info.players_joined;
                    host.total_players = info.total_players;
                    std::lock_guard lock{ discovery.mutex };
                    auto it = std::find_if(discovery.hosts.begin(), discovery.hosts.end(),
                        [&](const auto& e) { return e.host.address == host.address; });
                    if (it != discovery.hosts.end()) {
                        it->host = host;
                        it->seen = clock_type::now();
                    } else {
                        discovery.hosts.push_back({ host, clock_type::now() });
                    }
                }
            }
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

} // namespace

void sk1net::start_discovery(uint16_t port) {
    if (discovery.running) {
        return;
    }
    sk1net::local_address(); // makes sure WSAStartup has run on Windows
    discovery.running = true;
    discovery.thread = std::thread(discovery_thread, port);
}

void sk1net::stop_discovery() {
    if (!discovery.running) {
        return;
    }
    discovery.running = false;
    if (discovery.thread.joinable()) {
        discovery.thread.join();
    }
    std::lock_guard lock{ discovery.mutex };
    discovery.hosts.clear();
}

std::vector<sk1net::DiscoveredHost> sk1net::discovered_hosts() {
    std::lock_guard lock{ discovery.mutex };
    std::vector<sk1net::DiscoveredHost> result;
    const auto now = clock_type::now();
    for (const auto& entry : discovery.hosts) {
        if (now - entry.seen < std::chrono::seconds(4)) {
            result.push_back(entry.host);
        }
    }
    return result;
}
