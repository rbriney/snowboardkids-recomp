// Netplay lobby screen. See netplay_lobby.h.
//
// The lobby only chooses settings: it writes them to the SK1_NET_* environment variables that netplay reads when the
// game starts (src/game/netplay.h), then starts the game. Hosts are found on the local network by broadcasting
// discovery requests (sk1net::start_discovery); a host answers while it is waiting for players.

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_button.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_text_input.h"
#include "elements/ui_toggle.h"
#include "util/file.h"
#include "librecomp/game.hpp"
#include "netplay.h"
#include "netplay_lobby.h"
#include "netplay_overlay.h"

namespace {

constexpr uint16_t lobby_port = 7777;
constexpr size_t max_listed_hosts = 4;

struct Settings {
    uint32_t total_players = 2; // host
    uint32_t local_players = 1;
    std::string join_address;   // last typed address
    bool own_view = false;      // show only this device's player, full screen (one player on this device)
};

std::filesystem::path settings_path() {
    return recompui::file::get_app_folder_path() / "netplay.cfg";
}

Settings load_settings() {
    Settings settings;
    std::ifstream in(settings_path());
    std::string line;
    while (std::getline(in, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "players") {
            settings.total_players = std::clamp(std::atoi(value.c_str()), 2, 4);
        } else if (key == "local") {
            settings.local_players = std::clamp(std::atoi(value.c_str()), 1, 3);
        } else if (key == "address") {
            settings.join_address = value;
        } else if (key == "ownview") {
            settings.own_view = value == "1";
        }
    }
    return settings;
}

void save_settings(const Settings& settings) {
    std::ofstream out(settings_path(), std::ios::trunc);
    out << "players=" << settings.total_players << "\n";
    out << "local=" << settings.local_players << "\n";
    out << "address=" << settings.join_address << "\n";
    out << "ownview=" << (settings.own_view ? 1 : 0) << "\n";
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

enum class Page { Choose, Host, Join, Waiting };

struct Lobby {
    bool created = false;
    bool open = false;
    recompui::ContextId context;
    std::function<void()> start_game;
    Settings settings;
    Page page = Page::Choose;

    recompui::Element* choose_page = nullptr;
    recompui::Element* host_page = nullptr;
    recompui::Element* join_page = nullptr;
    recompui::Button* host_choice = nullptr;
    recompui::Radio* host_local_players = nullptr;
    recompui::Label* host_address = nullptr;
    recompui::Button* host_button = nullptr;
    recompui::Label* search_status = nullptr;
    std::array<recompui::Button*, max_listed_hosts> host_buttons{};
    std::array<std::string, max_listed_hosts> listed_addresses{};
    recompui::TextInput* address_input = nullptr;
    recompui::Radio* join_local_players = nullptr;
    recompui::Toggle* host_own_view = nullptr;
    recompui::Toggle* join_own_view = nullptr;
    recompui::Label* join_message = nullptr;  // why the last attempt to join ended
    std::u8string game_id;

    // Waiting room (a session in progress, before the game is launched).
    Page return_page = Page::Choose;
    recompui::Element* waiting_page = nullptr;
    recompui::Label* waiting_title = nullptr;
    recompui::Label* waiting_players = nullptr;
    recompui::Label* waiting_info = nullptr;
    recompui::Button* waiting_start = nullptr;
    recompui::Button* waiting_cancel = nullptr;
    std::string shown_players; // last texts set, to avoid re-setting them every frame
    std::string shown_info;
    int shown_can_start = -1;
} lobby;

void show_page(Page page);

// Applies the settings and opens the waiting room; the game is launched once the host starts it.
void start_with_env(Page from) {
    // Own view only applies with a single player on this device (netplay ignores it otherwise).
    set_env("SK1_NET_OWN_VIEW", lobby.settings.own_view ? "1" : "");
    save_settings(lobby.settings);
    sk1net::stop_discovery();
    const std::filesystem::path save = recomp::get_config_path() / "saves" / (lobby.game_id + u8".bin");
    lobby.return_page = from;
    lobby.join_message->set_text("");
    const std::u8string save_text = save.u8string();
    if (!sk1net::begin_lobby_session(std::string(save_text.begin(), save_text.end()))) {
        const std::string reason = sk1net::finish_lobby_session();
        lobby.join_message->set_text(reason);
        lobby.join_message->set_display(recompui::Display::Block);
        show_page(Page::Join);
        return;
    }
    show_page(Page::Waiting);
}

void host_game() {
    lobby.settings.local_players = lobby.host_local_players->get_index() + 1;
    set_env("SK1_NET_JOIN", "");
    set_env("SK1_NET_HOST", std::to_string(lobby_port));
    set_env("SK1_NET_PLAYERS", ""); // the host starts the game when ready
    set_env("SK1_NET_LOCAL_PLAYERS", std::to_string(lobby.settings.local_players));
    start_with_env(Page::Host);
}

void join_game(const std::string& address) {
    std::string target = address;
    if (target.empty()) {
        return;
    }
    if (target.find(':') == std::string::npos) {
        target += ":" + std::to_string(lobby_port);
    }
    lobby.settings.local_players = lobby.join_local_players->get_index() + 1;
    set_env("SK1_NET_HOST", "");
    set_env("SK1_NET_PLAYERS", "");
    set_env("SK1_NET_JOIN", target);
    set_env("SK1_NET_LOCAL_PLAYERS", std::to_string(lobby.settings.local_players));
    start_with_env(Page::Join);
}

void show_page(Page page) {
    lobby.page = page;
    lobby.host_own_view->set_checked(lobby.settings.own_view);
    lobby.join_own_view->set_checked(lobby.settings.own_view);
    lobby.choose_page->set_display(page == Page::Choose ? recompui::Display::Flex : recompui::Display::None);
    lobby.host_page->set_display(page == Page::Host ? recompui::Display::Flex : recompui::Display::None);
    lobby.join_page->set_display(page == Page::Join ? recompui::Display::Flex : recompui::Display::None);
    lobby.waiting_page->set_display(page == Page::Waiting ? recompui::Display::Flex : recompui::Display::None);
    if (page == Page::Waiting) {
        lobby.shown_players.clear();
        lobby.shown_info.clear();
        lobby.shown_can_start = -1;
        lobby.waiting_cancel->focus();
    }
    if (page == Page::Join) {
        sk1net::start_discovery(lobby_port);
    } else {
        sk1net::stop_discovery();
    }
    if (page == Page::Host) {
        const std::string address = sk1net::local_address();
        lobby.host_address->set_text(address.empty()
                ? "This device is not connected to a network."
                : "This device's address: " + address + "  (others can also find it automatically)");
        lobby.host_button->focus();
    } else if (page == Page::Choose) {
        lobby.host_choice->focus();
    }
}

recompui::Element* make_page(recompui::Element* parent) {
    using namespace recompui;
    Element* page = lobby.context.create_element<Element>(parent);
    page->set_display(Display::Flex);
    page->set_flex_direction(FlexDirection::Column);
    page->set_gap(16.0f);
    page->set_as_navigation_container(NavigationType::Vertical);
    return page;
}

recompui::Element* make_row(recompui::Element* parent) {
    using namespace recompui;
    Element* row = lobby.context.create_element<Element>(parent);
    row->set_display(Display::Flex);
    row->set_flex_direction(FlexDirection::Row);
    row->set_gap(16.0f);
    row->set_as_navigation_container(NavigationType::Horizontal);
    return row;
}

// "Show only my player" toggle; the host and join pages each have one, kept in step through the settings.
recompui::Toggle* make_own_view_row(recompui::Element* parent) {
    using namespace recompui;
    Element* row = make_row(parent);
    row->set_align_items(AlignItems::Center);
    Toggle* toggle = lobby.context.create_element<Toggle>(row);
    toggle->set_checked(lobby.settings.own_view);
    toggle->add_checked_callback([](bool checked) { lobby.settings.own_view = checked; });
    Label* label = lobby.context.create_element<Label>(row,
        "Show only my player, full screen (with one player on this device)", LabelStyle::Normal);
    label->set_white_space(WhiteSpace::Normal);
    return toggle;
}

recompui::Radio* make_radio(recompui::Element* parent, const std::vector<std::string>& options, uint32_t index) {
    recompui::Radio* radio = lobby.context.create_element<recompui::Radio>(parent);
    for (const std::string& option : options) {
        radio->add_option(option);
    }
    radio->set_index(index);
    return radio;
}

void create_lobby() {
    using namespace recompui;
    lobby.context = create_context();
    lobby.context.open();
    Element* root = lobby.context.get_root_element();

    Element* backdrop = lobby.context.create_element<Element>(root);
    backdrop->set_position(Position::Absolute);
    backdrop->set_top(0);
    backdrop->set_right(0);
    backdrop->set_bottom(0);
    backdrop->set_left(0);
    backdrop->set_display(Display::Flex);
    backdrop->set_align_items(AlignItems::Center);
    backdrop->set_justify_content(JustifyContent::Center);
    backdrop->set_background_color(Color{ 0, 0, 0, 170 });

    Element* panel = lobby.context.create_element<Element>(backdrop);
    panel->set_display(Display::Flex);
    panel->set_flex_direction(FlexDirection::Column);
    panel->set_gap(20.0f);
    panel->set_padding(40.0f);
    panel->set_border_radius(24.0f);
    panel->set_background_color(Color{ 20, 24, 40, 245 });
    panel->set_width(1000, Unit::Dp);
    panel->set_as_navigation_container(NavigationType::Vertical);

    lobby.context.create_element<Label>(panel, "Netplay", theme::Typography::Header2);

    // Choose: host or join.
    lobby.choose_page = make_page(panel);
    Label* intro = lobby.context.create_element<Label>(lobby.choose_page,
        "Play with up to four players on other phones or PCs on the same network. Everyone needs this version of "
        "the game and the same ROM.", LabelStyle::Normal);
    intro->set_white_space(WhiteSpace::Normal);
    Element* choices = make_row(lobby.choose_page);
    lobby.host_choice = lobby.context.create_element<Button>(choices, "Host a game", ButtonStyle::Primary);
    lobby.host_choice->add_pressed_callback([]() { show_page(Page::Host); });
    Button* join_choice = lobby.context.create_element<Button>(choices, "Join a game", ButtonStyle::Secondary);
    join_choice->add_pressed_callback([]() { show_page(Page::Join); });
    Button* close_button = lobby.context.create_element<Button>(choices, "Back", ButtonStyle::Tertiary);
    close_button->add_pressed_callback([]() { sk1net_lobby::close(); });

    // Host.
    lobby.host_page = make_page(panel);
    Label* host_intro = lobby.context.create_element<Label>(lobby.host_page,
        "Up to four players can join. You start the game when everyone you want is in.", LabelStyle::Normal);
    host_intro->set_white_space(WhiteSpace::Normal);
    lobby.context.create_element<Label>(lobby.host_page, "Players on this device", LabelStyle::Large);
    lobby.host_local_players = make_radio(lobby.host_page, { "1", "2", "3" }, lobby.settings.local_players - 1);
    lobby.host_own_view = make_own_view_row(lobby.host_page);
    lobby.host_address = lobby.context.create_element<Label>(lobby.host_page, "", LabelStyle::Normal);
    lobby.host_address->set_white_space(WhiteSpace::Normal);
    Element* host_actions = make_row(lobby.host_page);
    lobby.host_button = lobby.context.create_element<Button>(host_actions, "Host game", ButtonStyle::Primary);
    lobby.host_button->add_pressed_callback(host_game);
    Button* host_back = lobby.context.create_element<Button>(host_actions, "Back", ButtonStyle::Tertiary);
    host_back->add_pressed_callback([]() { show_page(Page::Choose); });

    // Join.
    lobby.join_page = make_page(panel);
    lobby.context.create_element<Label>(lobby.join_page, "Games on your network", LabelStyle::Large);
    lobby.join_message = lobby.context.create_element<Label>(lobby.join_page, "", LabelStyle::Normal);
    lobby.join_message->set_white_space(WhiteSpace::Normal);
    lobby.join_message->set_color(Color{ 255, 190, 120, 255 });
    lobby.search_status = lobby.context.create_element<Label>(lobby.join_page, "Searching...", LabelStyle::Normal);
    for (size_t i = 0; i < max_listed_hosts; i++) {
        lobby.host_buttons[i] = lobby.context.create_element<Button>(lobby.join_page, "", ButtonStyle::Primary);
        lobby.host_buttons[i]->set_display(Display::None);
        lobby.host_buttons[i]->add_pressed_callback([i]() { join_game(lobby.listed_addresses[i]); });
    }
    lobby.context.create_element<Label>(lobby.join_page, "Or type the host's address", LabelStyle::Large);
    Element* address_row = make_row(lobby.join_page);
    lobby.address_input = lobby.context.create_element<TextInput>(address_row);
    lobby.address_input->set_width(500, Unit::Dp);
    lobby.address_input->set_text(lobby.settings.join_address);
    lobby.address_input->add_text_changed_callback(
        [](const std::string& text) { lobby.settings.join_address = text; });
    // Bring up the on-screen keyboard on phones while the address field has focus.
    lobby.address_input->set_focus_callback([](bool focused) {
        if (focused) {
            SDL_StartTextInput();
        } else {
            SDL_StopTextInput();
        }
    });
    Button* join_address = lobby.context.create_element<Button>(address_row, "Join", ButtonStyle::Secondary);
    join_address->add_pressed_callback([]() { join_game(lobby.settings.join_address); });
    lobby.context.create_element<Label>(lobby.join_page, "Players on this device", LabelStyle::Large);
    lobby.join_local_players = make_radio(lobby.join_page, { "1", "2", "3" }, lobby.settings.local_players - 1);
    lobby.join_own_view = make_own_view_row(lobby.join_page);
    Button* join_back = lobby.context.create_element<Button>(lobby.join_page, "Back", ButtonStyle::Tertiary);
    join_back->add_pressed_callback([]() { show_page(Page::Choose); });

    // Waiting room.
    lobby.waiting_page = make_page(panel);
    lobby.waiting_title = lobby.context.create_element<Label>(lobby.waiting_page, "", LabelStyle::Large);
    lobby.waiting_players = lobby.context.create_element<Label>(lobby.waiting_page, "", LabelStyle::Normal);
    lobby.waiting_info = lobby.context.create_element<Label>(lobby.waiting_page, "", LabelStyle::Normal);
    lobby.waiting_info->set_white_space(WhiteSpace::Normal);
    Element* waiting_actions = make_row(lobby.waiting_page);
    lobby.waiting_start = lobby.context.create_element<Button>(waiting_actions, "Start game", ButtonStyle::Primary);
    lobby.waiting_start->add_pressed_callback([]() { sk1net::lobby_request_start(); });
    lobby.waiting_cancel = lobby.context.create_element<Button>(waiting_actions, "Cancel", ButtonStyle::Tertiary);
    lobby.waiting_cancel->add_pressed_callback([]() { sk1net::lobby_request_cancel(); });

    lobby.context.close();
    lobby.created = true;
}

} // namespace

void sk1net_lobby::open(const std::u8string& game_id, std::function<void()> start_game) {
    lobby.start_game = std::move(start_game);
    lobby.game_id = game_id;
    lobby.settings = load_settings();
    // Called from a launcher button, whose context is open on this thread: close it before touching the lobby's
    // own context (building or editing it), and reopen it afterwards.
    recompui::ContextId previous = recompui::try_close_current_context();
    if (!lobby.created) {
        create_lobby();
    }
    lobby.context.open();
    lobby.join_message->set_text("");
    lobby.context.close();
    sk1net_overlay::install(); // its frame hook also refreshes the lobby
    lobby.context.open();
    show_page(Page::Choose);
    lobby.context.close();
    recompui::show_context(lobby.context, "");
    lobby.open = true;
    if (previous != recompui::ContextId::null()) {
        previous.open();
    }
}

void sk1net_lobby::close() {
    if (!lobby.open) {
        return;
    }
    if (lobby.page == Page::Waiting) {
        sk1net::lobby_request_cancel();
        sk1net::finish_lobby_session();
    }
    lobby.open = false;
    sk1net::stop_discovery();
    recompui::hide_context(lobby.context);
}

bool sk1net_lobby::is_open() {
    return lobby.open;
}

namespace {
void update_waiting_room() {
    using namespace recompui;
    const sk1net::LobbyState state = sk1net::lobby_state();
    if (state == sk1net::LobbyState::Started) {
        // Everyone is in: launch the game (netplay continues from its first tick).
        sk1net::finish_lobby_session(); // joins the finished waiting thread; the session itself carries on
        lobby.page = Page::Choose;
        sk1net_lobby::close();
        if (lobby.start_game) {
            lobby.start_game();
        }
        return;
    }
    if (state == sk1net::LobbyState::Ended) {
        const std::string reason = sk1net::finish_lobby_session();
        lobby.context.open();
        // Cancelling or leaving yourself needs no explanation; anything else is shown on the join page.
        const bool own_choice = reason == "hosting cancelled" || reason == "you left the game";
        if (!own_choice && !reason.empty()) {
            std::string text = reason;
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            lobby.join_message->set_text(text);
        }
        show_page(lobby.return_page);
        lobby.context.close();
        return;
    }
    const sk1net::OverlayStatus status = sk1net::overlay_status();
    std::string players;
    std::string info;
    if (status.am_host) {
        players = "Players joined: " + std::to_string(status.players_joined) +
            (status.players_joined >= 4 ? " (full)" : "");
        info = "Others can find this game in Netplay > Join a game, or type " + status.address + ". ";
        info += status.can_start ? "Start the game when everyone you want is in."
              : status.players_joined >= 2 ? "Sending the save to the new player..."
                                           : "Waiting for someone to join...";
    } else {
        players = "Connected to " + status.address;
        info = "Waiting for the host to start the game.";
    }
    const int can_start = status.am_host && status.can_start ? 1 : 0;
    if (players == lobby.shown_players && info == lobby.shown_info && can_start == lobby.shown_can_start) {
        return;
    }
    lobby.context.open();
    if (lobby.shown_can_start < 0) {
        lobby.waiting_title->set_text(status.am_host ? "Hosting a game" : "Joining a game");
        lobby.waiting_cancel->set_text(status.am_host ? "Cancel" : "Leave");
        lobby.waiting_start->set_display(status.am_host ? Display::Flex : Display::None);
    }
    lobby.waiting_players->set_text(players);
    lobby.waiting_info->set_text(info);
    lobby.waiting_start->set_enabled(can_start == 1);
    if (can_start == 1 && lobby.shown_can_start != 1) {
        lobby.waiting_start->focus();
    }
    lobby.context.close();
    lobby.shown_players = players;
    lobby.shown_info = info;
    lobby.shown_can_start = can_start;
}
} // namespace

void sk1net_lobby::frame_update() {
    if (lobby.open && lobby.page == Page::Waiting) {
        update_waiting_room();
        return;
    }
    if (!lobby.open || lobby.page != Page::Join) {
        return;
    }
    const std::vector<sk1net::DiscoveredHost> hosts = sk1net::discovered_hosts();
    lobby.context.open();
    lobby.search_status->set_text(hosts.empty() ? "Searching for games on your network..." : "Tap a game to join:");
    for (size_t i = 0; i < max_listed_hosts; i++) {
        if (i < hosts.size()) {
            const sk1net::DiscoveredHost& host = hosts[i];
            const std::string text = host.address + "   (" + std::to_string(host.players_joined) + " of " +
                std::to_string(host.total_players) + " players)";
            if (lobby.listed_addresses[i] != host.address) {
                lobby.listed_addresses[i] = host.address;
            }
            lobby.host_buttons[i]->set_text(text);
            lobby.host_buttons[i]->set_display(recompui::Display::Flex);
        } else {
            lobby.listed_addresses[i].clear();
            lobby.host_buttons[i]->set_display(recompui::Display::None);
        }
    }
    lobby.context.close();
}
