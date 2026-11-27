// See netplay_internet.h.
//
// UPnP: an SSDP search (UDP multicast) finds the router's description URL; the description names the control URL
// of its WANIPConnection (or WANPPPConnection) service, which takes SOAP requests over HTTP to add and delete port
// mappings. Only what that needs is implemented: a blocking HTTP/1.1 client with "Connection: close" and a naive
// tag search in the XML.

#include "netplay_internet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

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

void (*external_logger)(const char*) = nullptr;

void log_line(const std::string& text) {
    if (external_logger != nullptr) {
        external_logger(text.c_str());
        return;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "SK1net", "%s", text.c_str());
#else
    std::fprintf(stderr, "[netplay] %s\n", text.c_str());
#endif
}

void set_timeouts(socket_t s, int milliseconds) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval value{ milliseconds / 1000, (milliseconds % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// The text of the first <tag>...</tag> at or after `from` (tag names compared without case or namespace prefix).
std::string xml_value(const std::string& xml, const std::string& tag, size_t from = 0, size_t* end_out = nullptr) {
    const std::string low = lower(xml);
    const std::string want = lower(tag);
    size_t pos = from;
    while ((pos = low.find('<', pos)) != std::string::npos) {
        size_t name_start = pos + 1;
        size_t name_end = low.find_first_of("> \t\r\n/", name_start);
        if (name_end == std::string::npos) {
            break;
        }
        std::string name = low.substr(name_start, name_end - name_start);
        const size_t colon = name.find(':');
        if (colon != std::string::npos) {
            name = name.substr(colon + 1);
        }
        const size_t open_end = low.find('>', name_end);
        if (name == want && open_end != std::string::npos) {
            const size_t close = low.find("</", open_end);
            if (close == std::string::npos) {
                break;
            }
            if (end_out != nullptr) {
                *end_out = close;
            }
            std::string value = xml.substr(open_end + 1, close - open_end - 1);
            const size_t first = value.find_first_not_of(" \t\r\n");
            const size_t last = value.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
        }
        pos = name_end;
    }
    return {};
}

struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool parse_url(const std::string& text, Url& url) {
    if (lower(text.substr(0, 7)) != "http://") {
        return false;
    }
    const std::string rest = text.substr(7);
    const size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    url.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = authority.find(':');
    url.host = authority.substr(0, colon);
    url.port = colon == std::string::npos ? 80 : static_cast<uint16_t>(std::atoi(authority.c_str() + colon + 1));
    return !url.host.empty();
}

// A blocking HTTP request. Returns the status code (0 on a network failure) and the body. Also reports the local
// address used to reach the server (the address the router should forward to).
int http_request(const Url& url, const std::string& method, const std::string& extra_headers, const std::string& body,
                 std::string& response_body, std::string* local_address = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &result) != 0 || result == nullptr) {
        return 0;
    }
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == invalid_socket) {
        freeaddrinfo(result);
        return 0;
    }
    set_timeouts(s, 3000);
    const bool connected = connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen)) == 0;
    freeaddrinfo(result);
    if (!connected) {
        close_socket(s);
        return 0;
    }
    if (local_address != nullptr) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char text[64];
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
            *local_address = text;
        }
    }
    std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" +
        std::to_string(url.port) + "\r\nConnection: close\r\n" + extra_headers;
    if (!body.empty() || method == "POST") {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n" + body;
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (n <= 0) {
            close_socket(s);
            return 0;
        }
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        response.append(buffer, static_cast<size_t>(n));
        if (response.size() > 1 << 20) {
            break;
        }
    }
    close_socket(s);
    const size_t header_end = response.find("\r\n\r\n");
    if (response.compare(0, 5, "HTTP/") != 0 || header_end == std::string::npos) {
        return 0;
    }
    const size_t space = response.find(' ');
    const int status = std::atoi(response.c_str() + space + 1);
    response_body = response.substr(header_end + 4);
    // Chunked bodies: strip the chunk-size lines (enough for the small XML replies routers send).
    if (lower(response.substr(0, header_end)).find("transfer-encoding: chunked") != std::string::npos) {
        std::string joined;
        size_t pos = 0;
        while (pos < response_body.size()) {
            const size_t line_end = response_body.find("\r\n", pos);
            if (line_end == std::string::npos) {
                break;
            }
            const size_t chunk = std::strtoul(response_body.substr(pos, line_end - pos).c_str(), nullptr, 16);
            if (chunk == 0) {
                break;
            }
            joined += response_body.substr(line_end + 2, chunk);
            pos = line_end + 2 + chunk + 2;
        }
        response_body = joined;
    }
    return status;
}

struct Gateway {
    Url control;
    std::string service; // e.g. urn:schemas-upnp-org:service:WANIPConnection:1
    std::string local_address;
};

// SSDP search for an Internet Gateway Device; returns the description URLs heard within the timeout.
std::vector<std::string> ssdp_search() {
    std::vector<std::string> locations;
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == invalid_socket) {
        return locations;
    }
    set_timeouts(s, 500);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);
    const char* targets[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                              "urn:schemas-upnp-org:service:WANIPConnection:1",
                              "urn:schemas-upnp-org:service:WANIPConnection:2",
                              "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    for (int attempt = 0; attempt < 2; attempt++) {
        for (const char* st : targets) {
            const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                                   "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
            sendto(s, search.data(), static_cast<int>(search.size()), 0, reinterpret_cast<const sockaddr*>(&target),
                sizeof(target));
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        while (std::chrono::steady_clock::now() < until) {
            char buffer[2048];
            const int n = recv(s, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) {
                continue;
            }
            const std::string reply(buffer, static_cast<size_t>(n));
            const std::string low = lower(reply);
            const size_t at = low.find("\nlocation:");
            if (at == std::string::npos) {
                continue;
            }
            const size_t start = reply.find_first_not_of(" \t", at + 10);
            const size_t end = reply.find_first_of("\r\n", start);
            const std::string location = reply.substr(start, end - start);
            if (std::find(locations.begin(), locations.end(), location) == locations.end()) {
                locations.push_back(location);
            }
        }
        if (!locations.empty()) {
            break;
        }
    }
    close_socket(s);
    return locations;
}

bool find_gateway(Gateway& gateway, std::string& why) {
    const std::vector<std::string> locations = ssdp_search();
    if (locations.empty()) {
        why = "no UPnP router answered";
        return false;
    }
    for (const std::string& location : locations) {
        Url url;
        if (!parse_url(location, url)) {
            continue;
        }
        std::string description;
        std::string local;
        if (http_request(url, "GET", "", "", description, &local) != 200) {
            continue;
        }
        // Find a WAN connection service and its control URL.
        size_t pos = 0;
        size_t end = 0;
        std::string type;
        while (!(type = xml_value(description, "serviceType", pos, &end)).empty()) {
            if (type.find("WANIPConnection") != std::string::npos ||
                type.find("WANPPPConnection") != std::string::npos) {
                const std::string control = xml_value(description, "controlURL", end);
                if (control.empty()) {
                    break;
                }
                gateway.service = type;
                gateway.local_address = local;
                if (lower(control.substr(0, 7)) == "http://") {
                    parse_url(control, gateway.control);
                } else {
                    std::string base = xml_value(description, "URLBase");
                    Url base_url = url;
                    if (!base.empty()) {
                        parse_url(base, base_url);
                    }
                    gateway.control = base_url;
                    gateway.control.path = control[0] == '/' ? control : "/" + control;
                }
                return true;
            }
            pos = end;
        }
    }
    why = "the router does not offer UPnP port forwarding";
    return false;
}

int soap(const Gateway& gateway, const std::string& action, const std::string& arguments, std::string& reply) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" +
        gateway.service + "\">" + arguments + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gateway.service +
        "#" + action + "\"\r\n";
    return http_request(gateway.control, "POST", headers, body, reply);
}

struct State {
    std::mutex mutex;
    std::thread thread;
    std::atomic<int> state{ static_cast<int>(sk1net_internet::PortState::Idle) };
    std::string detail;
    std::string router_address;
    Gateway gateway;
    uint16_t port = 0;
} upnp;

void open_port_thread(uint16_t port) {
    Gateway gateway;
    std::string why;
    auto fail = [&](const std::string& reason) {
        std::lock_guard lock{ upnp.mutex };
        upnp.detail = reason;
        upnp.state = static_cast<int>(sk1net_internet::PortState::Failed);
        log_line("UPnP: could not open UDP port " + std::to_string(port) + ": " + reason);
    };
    if (!find_gateway(gateway, why)) {
        fail(why);
        return;
    }
    std::string reply;
    std::string external;
    if (soap(gateway, "GetExternalIPAddress", "", reply) == 200) {
        external = xml_value(reply, "NewExternalIPAddress");
    }
    const std::string port_text = std::to_string(port);
    auto add = [&](int lease) {
        return soap(gateway, "AddPortMapping",
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + port_text +
            "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + port_text +
            "</NewInternalPort><NewInternalClient>" + gateway.local_address +
            "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Snowboard Kids netplay"
            "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>",
            reply);
    };
    int status = add(7200);
    if (status != 200 && xml_value(reply, "errorCode") == "725") {
        status = add(0); // OnlyPermanentLeasesSupported
    }
    if (status != 200) {
        const std::string code = xml_value(reply, "errorCode");
        const std::string description = xml_value(reply, "errorDescription");
        fail(code == "718" ? "the port is already forwarded to another device"
             : status == 0 ? "the router did not answer"
                           : "the router refused (" + (code.empty() ? std::to_string(status) : code) +
                    (description.empty() ? "" : " " + description) + ")");
        return;
    }
    std::lock_guard lock{ upnp.mutex };
    upnp.gateway = gateway;
    upnp.port = port;
    upnp.router_address = external;
    upnp.detail.clear();
    upnp.state = static_cast<int>(sk1net_internet::PortState::Open);
    log_line("UPnP: router forwards UDP port " + port_text + " to " + gateway.local_address +
        (external.empty() ? "" : "; public address " + external));
}

std::array<uint8_t, 12> stun_transaction{};
constexpr uint32_t stun_cookie = 0x2112A442;

} // namespace

void sk1net_internet::set_logger(void (*logger)(const char* message)) {
    external_logger = logger;
}

void sk1net_internet::open_port(uint16_t port) {
    if (upnp.thread.joinable()) {
        upnp.thread.join();
    }
    upnp.state = static_cast<int>(PortState::Working);
    upnp.thread = std::thread(open_port_thread, port);
}

void sk1net_internet::close_port() {
    if (upnp.thread.joinable()) {
        upnp.thread.join();
    }
    Gateway gateway;
    uint16_t port;
    {
        std::lock_guard lock{ upnp.mutex };
        if (upnp.state != static_cast<int>(PortState::Open)) {
            upnp.state = static_cast<int>(PortState::Idle);
            return;
        }
        gateway = upnp.gateway;
        port = upnp.port;
        upnp.state = static_cast<int>(PortState::Idle);
    }
    std::string reply;
    soap(gateway, "DeletePortMapping",
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
        "</NewExternalPort><NewProtocol>UDP</NewProtocol>", reply);
    log_line("UPnP: closed UDP port " + std::to_string(port));
}

sk1net_internet::PortState sk1net_internet::port_state() {
    return static_cast<PortState>(upnp.state.load());
}

std::string sk1net_internet::port_detail() {
    std::lock_guard lock{ upnp.mutex };
    return upnp.detail;
}

std::string sk1net_internet::router_address() {
    std::lock_guard lock{ upnp.mutex };
    return upnp.router_address;
}

bool sk1net_internet::resolve_stun_server(sockaddr_in& out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(stun_host, std::to_string(stun_port).c_str(), &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    std::memcpy(&out, result->ai_addr, sizeof(out));
    freeaddrinfo(result);
    return true;
}

size_t sk1net_internet::make_stun_request(uint8_t (&buffer)[20]) {
    static std::mt19937 rng{ std::random_device{}() };
    for (uint8_t& b : stun_transaction) {
        b = static_cast<uint8_t>(rng());
    }
    buffer[0] = 0x00; // Binding request
    buffer[1] = 0x01;
    buffer[2] = 0x00; // no attributes
    buffer[3] = 0x00;
    buffer[4] = uint8_t(stun_cookie >> 24);
    buffer[5] = uint8_t(stun_cookie >> 16);
    buffer[6] = uint8_t(stun_cookie >> 8);
    buffer[7] = uint8_t(stun_cookie);
    std::memcpy(buffer + 8, stun_transaction.data(), 12);
    return 20;
}

bool sk1net_internet::is_stun_packet(const uint8_t* data, size_t size) {
    return size >= 20 && (data[0] & 0xC0) == 0 && data[4] == uint8_t(stun_cookie >> 24) &&
        data[5] == uint8_t(stun_cookie >> 16) && data[6] == uint8_t(stun_cookie >> 8) && data[7] == uint8_t(stun_cookie);
}

std::string sk1net_internet::parse_stun_response(const uint8_t* data, size_t size) {
    if (!is_stun_packet(data, size) || data[0] != 0x01 || data[1] != 0x01 ||
        std::memcmp(data + 8, stun_transaction.data(), 12) != 0) {
        return {};
    }
    const size_t length = std::min<size_t>(size - 20, (size_t(data[2]) << 8) | data[3]);
    size_t pos = 20;
    std::string mapped;
    while (pos + 4 <= 20 + length) {
        const uint16_t type = uint16_t((data[pos] << 8) | data[pos + 1]);
        const uint16_t attr_length = uint16_t((data[pos + 2] << 8) | data[pos + 3]);
        const uint8_t* value = data + pos + 4;
        if (pos + 4 + attr_length > size) {
            break;
        }
        if ((type == 0x0020 || type == 0x0001) && attr_length >= 8 && value[1] == 0x01) { // IPv4
            uint16_t port = uint16_t((value[2] << 8) | value[3]);
            uint8_t ip[4] = { value[4], value[5], value[6], value[7] };
            if (type == 0x0020) { // XOR-MAPPED-ADDRESS
                port ^= uint16_t(stun_cookie >> 16);
                for (int i = 0; i < 4; i++) {
                    ip[i] ^= uint8_t(stun_cookie >> (24 - 8 * i));
                }
            }
            mapped = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
                std::to_string(ip[3]) + ":" + std::to_string(port);
            if (type == 0x0020) {
                break; // preferred
            }
        }
        pos += 4 + ((attr_length + 3) & ~size_t(3));
    }
    return mapped;
}
