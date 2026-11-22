// Two-player lockstep netplay over UDP. See netplay.h for configuration.
//
// Every machine runs the full game. At the start of game tick t each machine samples its local controller (the
// game's port 0 on that machine) and schedules it for tick t + delay, then sends all inputs the peer has not yet
// acknowledged. Tick t only runs once the peer's input for tick t has arrived, so both machines feed identical inputs
// to identical game code (see the determinism harness results). Each packet repeats every unacknowledged input, so a
// lost packet needs no retransmission logic. Packets also carry a recent game-state hash for desync detection.
//
// Handshake: the joiner sends HELLO (with a bitmask of save chunks it already has) until the host has sent WELCOME
// and all 32 save chunks; it then sends READY until it sees START or the host's first INPUT packet.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
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

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "netplay.h"

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
void close_socket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
void close_socket(socket_t s) { close(s); }
#endif

using clock_type = std::chrono::steady_clock;

constexpr uint32_t packet_magic = 0x4E314B53; // "SK1N"
constexpr uint16_t protocol_version = 1;
constexpr uint32_t save_size = 0x8000;
constexpr uint32_t chunk_size = 1024;
constexpr uint32_t chunk_count = save_size / chunk_size;
constexpr uint32_t all_chunks = 0xFFFFFFFFu;
constexpr uint32_t port_count = 4;
constexpr uint32_t max_inputs_per_packet = 64;
constexpr uint32_t hash_history = 512;
constexpr uint32_t no_tick = 0xFFFFFFFFu;
constexpr auto peer_timeout = std::chrono::seconds(10);
constexpr auto resend_interval = std::chrono::milliseconds(15);
constexpr auto hello_interval = std::chrono::milliseconds(200);
static_assert(chunk_count == 32);

enum PacketType : uint8_t { Hello = 1, Welcome = 2, SaveChunk = 3, Ready = 4, Start = 5, Input = 6 };

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint8_t type;
    uint8_t player;
};
struct HelloPacket {
    Header header;
    uint32_t have_mask;
};
struct WelcomePacket {
    Header header;
    uint8_t delay;
    uint8_t padding[3];
};
struct SaveChunkPacket {
    Header header;
    uint8_t index;
    uint8_t padding[3];
    uint8_t data[chunk_size];
};
struct WireInput {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
};
struct InputPacket {
    Header header;
    uint32_t first_tick; // tick of inputs[0]
    uint32_t ack_tick;   // the sender holds every one of the receiver's inputs below this tick
    uint32_t hash_tick;  // tick of state_hash, or no_tick
    uint32_t state_hash;
    uint8_t count;
    uint8_t padding[3];
    WireInput inputs[max_inputs_per_packet];
};
#pragma pack(pop)

constexpr size_t input_packet_base = sizeof(InputPacket) - sizeof(WireInput) * max_inputs_per_packet;

struct Session {
    bool initialised = false;
    bool enabled = false;
    bool started = false;
    bool ended = false;
    bool is_host = false;
    uint8_t my_player = 0;
    uint8_t peer_player = 1;
    uint32_t delay = 3;

    socket_t sock = invalid_socket;
    sockaddr_in peer{};
    bool have_peer = false;
    clock_type::time_point last_receive{};

    std::mutex save_mutex;
    std::vector<uint8_t> save;
    uint32_t have_mask = 0;
    bool welcomed = false;

    std::vector<WireInput> local;  // indexed by tick
    std::vector<WireInput> remote; // indexed by tick
    std::vector<uint8_t> remote_have;
    uint32_t remote_next = 0; // every remote input below this tick has arrived
    uint32_t peer_ack = 0;    // the peer holds every local input below this tick
    uint32_t tick = 0;

    std::array<uint32_t, hash_history> my_hash_tick{};
    std::array<uint32_t, hash_history> my_hash{};
    std::array<uint32_t, hash_history> peer_hash_tick{};
    std::array<uint32_t, hash_history> peer_hash{};
    uint32_t last_hash_tick = no_tick;
    bool desync_reported = false;

    FILE* log = nullptr;
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

void fill_header(Header& h, PacketType type) {
    h.magic = packet_magic;
    h.version = protocol_version;
    h.type = type;
    h.player = session.my_player;
}

void send_raw(const void* data, size_t size) {
    if (!session.have_peer) {
        return;
    }
    sendto(session.sock, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
        reinterpret_cast<const sockaddr*>(&session.peer), sizeof(session.peer));
}

WireInput input_at(const std::vector<WireInput>& inputs, uint32_t t) {
    if (t < session.delay || t >= inputs.size()) {
        return WireInput{};
    }
    return inputs[t];
}

void send_inputs() {
    InputPacket p{};
    fill_header(p.header, Input);
    const uint32_t last = session.tick + session.delay; // newest scheduled local input
    uint32_t first = std::max(session.peer_ack, session.delay);
    uint32_t count = 0;
    if (last >= first) {
        count = std::min(last - first + 1, max_inputs_per_packet);
    }
    p.first_tick = first;
    p.count = static_cast<uint8_t>(count);
    for (uint32_t i = 0; i < count; i++) {
        p.inputs[i] = input_at(session.local, first + i);
    }
    p.ack_tick = session.remote_next;
    p.hash_tick = session.last_hash_tick;
    p.state_hash = session.last_hash_tick == no_tick ? 0 : session.my_hash[session.last_hash_tick % hash_history];
    send_raw(&p, input_packet_base + count * sizeof(WireInput));
}

void check_hash(uint32_t t) {
    const uint32_t slot = t % hash_history;
    if (session.my_hash_tick[slot] != t || session.peer_hash_tick[slot] != t) {
        return;
    }
    if (session.my_hash[slot] != session.peer_hash[slot] && !session.desync_reported) {
        session.desync_reported = true;
        log_msg("DESYNC: game state differs from the peer's at tick %u", t);
    }
}

void handle_input(const InputPacket& p, size_t size) {
    if (size < input_packet_base || size < input_packet_base + p.count * sizeof(WireInput)) {
        return;
    }
    for (uint32_t i = 0; i < p.count; i++) {
        const uint32_t t = p.first_tick + i;
        if (t >= session.remote.size()) {
            session.remote.resize(t + 1024);
            session.remote_have.resize(t + 1024, 0);
        }
        if (!session.remote_have[t]) {
            session.remote[t] = p.inputs[i];
            session.remote_have[t] = 1;
        }
    }
    while (session.remote_next < session.remote_have.size() && session.remote_have[session.remote_next]) {
        session.remote_next++;
    }
    session.peer_ack = std::max(session.peer_ack, p.ack_tick);
    if (p.hash_tick != no_tick) {
        const uint32_t slot = p.hash_tick % hash_history;
        session.peer_hash_tick[slot] = p.hash_tick;
        session.peer_hash[slot] = p.state_hash;
        check_hash(p.hash_tick);
    }
}

void send_save_chunks(uint32_t have_mask) {
    WelcomePacket w{};
    fill_header(w.header, Welcome);
    w.delay = static_cast<uint8_t>(session.delay);
    send_raw(&w, sizeof(w));
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (have_mask & (1u << i)) {
            continue;
        }
        SaveChunkPacket c{};
        fill_header(c.header, SaveChunk);
        c.index = static_cast<uint8_t>(i);
        std::memcpy(c.data, session.save.data() + i * chunk_size, chunk_size);
        send_raw(&c, sizeof(c));
    }
}

void process_packet(const uint8_t* data, size_t size, const sockaddr_in& from) {
    if (size < sizeof(Header)) {
        return;
    }
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != packet_magic || h.version != protocol_version || h.player != session.peer_player) {
        return;
    }
    if (session.is_host) {
        if (!session.have_peer) {
            session.peer = from;
            session.have_peer = true;
            char address[64];
            inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
            log_msg("player 2 connected from %s:%u", address, ntohs(from.sin_port));
        } else if (from.sin_addr.s_addr != session.peer.sin_addr.s_addr || from.sin_port != session.peer.sin_port) {
            return;
        }
    }
    session.last_receive = clock_type::now();

    switch (h.type) {
        case Hello:
            if (session.is_host && size >= sizeof(HelloPacket)) {
                HelloPacket p;
                std::memcpy(&p, data, sizeof(p));
                send_save_chunks(p.have_mask);
            }
            break;
        case Ready:
            if (session.is_host) {
                if (!session.started) {
                    session.started = true;
                    log_msg("session started (input delay %u ticks)", session.delay);
                }
                Header s;
                fill_header(s, Start);
                send_raw(&s, sizeof(s));
            }
            break;
        case Welcome:
            if (!session.is_host && size >= sizeof(WelcomePacket)) {
                WelcomePacket p;
                std::memcpy(&p, data, sizeof(p));
                session.delay = p.delay;
                session.welcomed = true;
            }
            break;
        case SaveChunk:
            if (!session.is_host && size >= sizeof(SaveChunkPacket)) {
                const SaveChunkPacket* p = reinterpret_cast<const SaveChunkPacket*>(data);
                if (p->index < chunk_count && !(session.have_mask & (1u << p->index))) {
                    std::lock_guard lock{ session.save_mutex };
                    std::memcpy(session.save.data() + p->index * chunk_size, p->data, chunk_size);
                    session.have_mask |= 1u << p->index;
                }
            }
            break;
        case Start:
            if (!session.is_host && session.welcomed && session.have_mask == all_chunks && !session.started) {
                session.started = true;
                log_msg("session started (input delay %u ticks)", session.delay);
            }
            break;
        case Input:
            if (!session.is_host && !session.started && session.welcomed && session.have_mask == all_chunks) {
                session.started = true;
                log_msg("session started (input delay %u ticks)", session.delay);
            }
            if (session.started) {
                InputPacket p{};
                std::memcpy(&p, data, std::min(size, sizeof(p)));
                handle_input(p, size);
            }
            break;
    }
}

// Receive and process packets, waiting up to timeout for the first one.
void pump(std::chrono::milliseconds timeout) {
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

void open_log() {
    std::filesystem::path folder = std::filesystem::current_path();
    if (const char* app = std::getenv("APP_FOLDER_PATH"); app != nullptr && *app != '\0') {
        folder = std::filesystem::u8path(app);
    }
    session.log = std::fopen((folder / "netplay.log").string().c_str(), "w");
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
    open_log();

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
    if (hosting) {
        session.is_host = true;
        session.my_player = 0;
        session.peer_player = 1;
        local.sin_port = htons(static_cast<uint16_t>(std::atoi(host)));
        if (const char* d = std::getenv("SK1_NET_DELAY"); d != nullptr && *d != '\0') {
            session.delay = static_cast<uint32_t>(std::clamp(std::atoi(d), 1, 30));
        }
    } else {
        session.is_host = false;
        session.my_player = 1;
        session.peer_player = 0;
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
        std::memcpy(&session.peer, result->ai_addr, sizeof(session.peer));
        freeaddrinfo(result);
        session.have_peer = true;
    }
    if (bind(session.sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        log_msg("cannot bind UDP port %u; netplay disabled", ntohs(local.sin_port));
        return;
    }

    session.save.assign(save_size, 0);
    session.remote_next = session.delay;
    session.peer_ack = session.delay;
    session.my_hash_tick.fill(no_tick);
    session.peer_hash_tick.fill(no_tick);
    session.enabled = true;
    if (hosting) {
        log_msg("hosting on UDP port %u as player 1; waiting for player 2", ntohs(local.sin_port));
    } else {
        log_msg("joining %s as player 2", join);
    }
}

void end_session(const char* reason) {
    if (!session.ended) {
        session.ended = true;
        log_msg("session ended: %s", reason);
    }
}

// Blocks the game thread until the handshake completes (at the first game tick).
void wait_for_start() {
    if (session.is_host) {
        // The host's save is the session save.
        std::ifstream in(ultramodern::get_save_file_path(), std::ios::binary);
        if (in) {
            std::lock_guard lock{ session.save_mutex };
            in.read(reinterpret_cast<char*>(session.save.data()), save_size);
        }
        session.have_mask = all_chunks;
    }
    auto last_send = clock_type::time_point{};
    auto last_log = clock_type::now();
    while (!session.started) {
        const auto now = clock_type::now();
        if (!session.is_host && now - last_send >= hello_interval) {
            last_send = now;
            if (session.welcomed && session.have_mask == all_chunks) {
                Header r;
                fill_header(r, Ready);
                send_raw(&r, sizeof(r));
            } else {
                HelloPacket p{};
                fill_header(p.header, Hello);
                p.have_mask = session.have_mask;
                send_raw(&p, sizeof(p));
            }
        }
        if (now - last_log >= std::chrono::seconds(5)) {
            last_log = now;
            log_msg(session.is_host ? "still waiting for player 2" : "still waiting for the host");
        }
        pump(std::chrono::milliseconds(20));
    }
    session.last_receive = clock_type::now();
}

} // namespace

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
    }

    // Sample this machine's controller (the game's port 0 locally) and schedule it.
    WireInput mine{};
    mine.buttons = MEM_HU(0, inputs);
    mine.stick_x = MEM_B(2, inputs);
    mine.stick_y = MEM_B(3, inputs);
    const uint32_t scheduled = session.tick + session.delay;
    if (scheduled >= session.local.size()) {
        session.local.resize(scheduled + 1024);
    }
    session.local[scheduled] = mine;

    send_inputs();
    auto last_send = clock_type::now();
    while (session.remote_next <= session.tick) {
        pump(std::chrono::milliseconds(5));
        const auto now = clock_type::now();
        if (now - session.last_receive > peer_timeout) {
            end_session("no packets from the other player for 10 seconds");
            return;
        }
        if (now - last_send >= resend_interval) {
            last_send = now;
            send_inputs();
        }
    }
    // Drain anything else that has arrived without waiting.
    pump(std::chrono::milliseconds(0));

    for (uint32_t port = 0; port < port_count; port++) {
        WireInput in{};
        if (port == session.my_player) {
            in = input_at(session.local, session.tick);
        } else if (port == session.peer_player) {
            in = input_at(session.remote, session.tick);
        }
        gpr addr = inputs + port * 6;
        MEM_H(0, addr) = in.buttons;
        MEM_B(2, addr) = in.stick_x;
        MEM_B(3, addr) = in.stick_y;
    }
}

void sk1net::post_tick(uint32_t state_hash) {
    if (!active()) {
        return;
    }
    const uint32_t slot = session.tick % hash_history;
    session.my_hash_tick[slot] = session.tick;
    session.my_hash[slot] = state_hash;
    session.last_hash_tick = session.tick;
    check_hash(session.tick);
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
