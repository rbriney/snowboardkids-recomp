#pragma once

// Netplay status overlay (src/game/netplay_overlay.cpp).
namespace sk1net_overlay {
    // Registers the overlay's per-frame UI hook. Safe to call from any thread, more than once.
    void install();
}
