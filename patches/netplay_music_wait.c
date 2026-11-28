// @recomp Netplay: make the race results' wait for the results music deterministic.
//
// The results flow (updateRaceResultsFlow, and updateRaceResultsMusicFlow for one player) waits until no music
// sequence is playing before it moves on. Music plays in real time on each device's audio thread, so it stops on a
// different game tick on each device (and much later in a fast-forwarded replay), which puts the devices' game state
// a tick or more apart. During netplay every device uses the host's answer instead, which the host sends with its
// inputs; replays of netplay recordings use the recorded answer (recomp_netplay_music_wait in
// src/game/determinism_harness.cpp).
#include "patches.h"

#include "game/audio/audio_engine.h"
#include "game/audio/sound_manager.h"

// Whether to treat music as still playing: this device's own answer outside netplay, otherwise the host's.
extern s32 recomp_netplay_music_wait(s32 localActive);

RECOMP_PATCH s32 countActiveMusicSequences(void) {
    return recomp_netplay_music_wait(MusAsk(2));
}
