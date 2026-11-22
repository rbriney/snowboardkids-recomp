// On-screen netplay status: shown while the session is paused (a player lagging or disconnected) or while this
// machine is waiting for the host. Built with RecompFrontend's UI and updated from its per-frame hook, on the UI
// thread, from a snapshot that netplay publishes (sk1net::overlay_status).

#include <array>
#include <string>

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "netplay.h"
#include "netplay_overlay.h"

namespace {

struct Overlay {
    bool created = false;
    bool shown = false;
    recompui::ContextId context;
    recompui::Label* title = nullptr;
    std::array<recompui::Label*, 4> players{};
    recompui::Label* footer = nullptr;
} overlay;

void create_overlay() {
    using namespace recompui;
    overlay.context = create_context();
    // Leave the controllers to the game: netplay reads A (ready) and Start (continue without) itself, and an
    // input-capturing context would hide those presses from it.
    overlay.context.set_captures_input(false);
    overlay.context.set_captures_mouse(false);
    overlay.context.open();
    Element* root = overlay.context.get_root_element();

    Element* page = overlay.context.create_element<Element>(root);
    page->set_position(Position::Absolute);
    page->set_top(0);
    page->set_right(0);
    page->set_bottom(0);
    page->set_left(0);
    page->set_display(Display::Flex);
    page->set_align_items(AlignItems::Center);
    page->set_justify_content(JustifyContent::Center);
    page->set_background_color(Color{ 0, 0, 0, 140 });

    Element* panel = overlay.context.create_element<Element>(page);
    panel->set_display(Display::Flex);
    panel->set_flex_direction(FlexDirection::Column);
    panel->set_gap(18.0f);
    panel->set_padding(40.0f);
    panel->set_border_radius(24.0f);
    panel->set_background_color(Color{ 20, 24, 40, 235 });
    panel->set_max_width(1100, Unit::Dp);

    overlay.title = overlay.context.create_element<Label>(panel, "", theme::Typography::Header2);
    for (auto& line : overlay.players) {
        line = overlay.context.create_element<Label>(panel, "", LabelStyle::Large);
    }
    overlay.footer = overlay.context.create_element<Label>(panel, "", LabelStyle::Normal);
    overlay.footer->set_white_space(WhiteSpace::Normal);
    overlay.context.close();
    overlay.created = true;
}

std::string player_line(const sk1net::OverlayStatus& status, uint32_t port) {
    const sk1net::PlayerStatus& p = status.players[port];
    std::string line = "Player " + std::to_string(port + 1);
    if (p.local) {
        line += " (you)";
    } else if (p.host) {
        line += " (host)";
    }
    line += ":  ";
    switch (p.state) {
        case sk1net::PlayerState::Connected:
            line += status.paused ? (p.ready ? "Ready" : "Press A when ready") : "Connected";
            break;
        case sk1net::PlayerState::Lagging:
            line += "Connection is slow...";
            break;
        case sk1net::PlayerState::Disconnected:
            line += "Disconnected - waiting for them to come back";
            break;
        case sk1net::PlayerState::Dropped:
            line += "Left the race (the game continues without them)";
            break;
    }
    return line;
}

void frame_hook() {
    const sk1net::OverlayStatus status = sk1net::overlay_status();
    if (!status.visible) {
        if (overlay.shown) {
            recompui::hide_context(overlay.context);
            overlay.shown = false;
        }
        return;
    }
    if (!overlay.created) {
        create_overlay();
    }

    overlay.context.open();
    overlay.title->set_text(status.paused ? "Netplay paused" : "Waiting for the host...");
    for (uint32_t port = 0; port < overlay.players.size(); port++) {
        if (port < status.total_ports) {
            overlay.players[port]->set_display(recompui::Display::Block);
            overlay.players[port]->set_text(player_line(status, port));
        } else {
            overlay.players[port]->set_display(recompui::Display::None);
        }
    }
    std::string footer;
    if (status.paused) {
        footer = "The race resumes once every connected player has pressed A.";
        if (status.am_host && status.can_continue_without) {
            footer += " Host: press Start to continue without the disconnected players.";
        }
    } else {
        footer = "The game will continue as soon as the host's connection recovers (up to a minute).";
    }
    overlay.footer->set_text(footer);
    overlay.context.close();

    if (!overlay.shown) {
        recompui::show_context(overlay.context, "");
        overlay.shown = true;
    }
}

} // namespace

void sk1net_overlay::install() {
    recompui::set_frame_hook(frame_hook);
}
