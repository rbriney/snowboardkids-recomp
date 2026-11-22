#pragma once

#include <cstdint>

#include "recomp.h"

// Determinism harness (src/game/determinism_harness.cpp). Active when SK1_DET_RECORD or SK1_DET_REPLAY is set.
namespace sk1det {
    bool active();
    // Redirect game saves to the harness's in-memory copy while it is active. Return false when inactive.
    bool save_read(uint8_t* rdram, gpr dst, uint32_t offset, uint32_t size);
    bool save_write(uint8_t* rdram, gpr src, uint32_t offset, uint32_t size);
}

extern "C" void recomp_det_pre_tick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_det_post_tick(uint8_t* rdram, recomp_context* ctx);
