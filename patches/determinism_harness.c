#include "patches.h"

#include "game/audio/sound_manager.h"
#include "game/engine/controller_input.h"
#include "game/engine/game_task_scheduler.h"
#include "game/engine/system_runtime.h"
#include "game/menu/main_menu/controller_main_menu_flow.h"
#include "game/race/player/race_player_input.h"

// Determinism harness (netplay groundwork). Native side: src/game/determinism_harness.cpp.
// Each game tick, recomp_det_pre_tick records the four ports' inputs or replaces them with recorded ones, and
// recomp_det_post_tick hashes game state after the tick. Both are no-ops unless SK1_DET_RECORD or SK1_DET_REPLAY
// is set.
extern void recomp_det_pre_tick(ControllerInputState* inputs, u8* connectedMask);
extern void recomp_det_post_tick(RacePlayer* players, u16* mainRngIndex, u16* secondaryRngIndex, s16* frameCounter);
extern u16 gMainRngIndex;
extern u16 gSecondaryRngIndex;

// From game_task_scheduler.c.
#define RAW_STICK_LIMIT 0x2D
#define STICK_DIRECTION_PRESS_THRESHOLD 0x1B
#define STICK_DIRECTION_RELEASE_THRESHOLD 8
#define PLAYER_INPUT_REPEAT_DELAY 9
#define STICK_DIRECTION_MASK 0xFFFF0000

extern s8 gAnalogStickResponseCurve[];
extern s32 gPlayerInputHeld[];
extern s32 gPlayerInputPressed[];
extern s8 gPlayerStickX[];
extern s8 gPlayerStickY[];
void resetRenderScratchAllocator(void);
s32 updateFramebufferRenderScheduler(void);

// Unchanged from the decompilation apart from the two @recomp calls.
RECOMP_PATCH void updateGameTaskScheduler(void) {
    s32 playerIndex;
    s32* previousInput;
    ControllerInputState* controllerInput;
    s32 previousHeldInput;
    s32 heldInput;
    s32 mappedStickX;
    s32 mappedStickY;
    s8 rawStickX;
    s8 rawStickY;

    // @recomp Record or inject this tick's controller inputs.
    recomp_det_pre_tick(gControllerInputState, &gConnectedControllerBitmask);

    gFrameCounter = (gFrameCounter + 1) & 0xFFF;
    resetRenderScratchAllocator();
    resetRenderCallbackQueues();
    clearPendingPositionalSoundRequests();
    for (playerIndex = 0; playerIndex < PLAYER_INPUT_COUNT; playerIndex++) {
        previousInput = &gPlayerInputPrevious[playerIndex];
        previousHeldInput = gPlayerInputHeld[playerIndex];
        *previousInput = previousHeldInput;
        controllerInput = &gControllerInputState[playerIndex];
        heldInput = previousHeldInput & STICK_DIRECTION_MASK;
        *(u32*)&gPlayerInputHeld[playerIndex] = heldInput;
        gPlayerInputHeld[playerIndex] |= controllerInput->buttons;
        rawStickX = controllerInput->stickX;
        if (rawStickX > RAW_STICK_LIMIT) {
            controllerInput->stickX = RAW_STICK_LIMIT;
            rawStickX = controllerInput->stickX;
        }
        if (rawStickX < -RAW_STICK_LIMIT) {
            controllerInput->stickX = -RAW_STICK_LIMIT;
        }
        rawStickY = controllerInput->stickY;
        if (rawStickY > RAW_STICK_LIMIT) {
            controllerInput->stickY = RAW_STICK_LIMIT;
            rawStickY = controllerInput->stickY;
        }
        if (rawStickY < -RAW_STICK_LIMIT) {
            controllerInput->stickY = -RAW_STICK_LIMIT;
            rawStickY = controllerInput->stickY;
        }
        rawStickX = controllerInput->stickX;
        if (rawStickX >= 0) {
            gPlayerStickX[playerIndex] = gAnalogStickResponseCurve[rawStickX];
        } else {
            gPlayerStickX[playerIndex] = -gAnalogStickResponseCurve[-rawStickX];
        }
        if (rawStickY >= 0) {
            gPlayerStickY[playerIndex] = gAnalogStickResponseCurve[rawStickY];
        } else {
            gPlayerStickY[playerIndex] = -gAnalogStickResponseCurve[-rawStickY];
        }
        mappedStickX = gPlayerStickX[playerIndex];
        if (mappedStickX >= STICK_DIRECTION_PRESS_THRESHOLD) {
            gPlayerInputHeld[playerIndex] |= STICK_RIGHT;
        }
        if (mappedStickX <= -STICK_DIRECTION_PRESS_THRESHOLD) {
            gPlayerInputHeld[playerIndex] |= STICK_LEFT;
        }
        mappedStickY = gPlayerStickY[playerIndex];
        if (mappedStickY >= STICK_DIRECTION_PRESS_THRESHOLD) {
            gPlayerInputHeld[playerIndex] |= STICK_UP;
        }
        if (mappedStickY <= -STICK_DIRECTION_PRESS_THRESHOLD) {
            gPlayerInputHeld[playerIndex] |= STICK_DOWN;
        }
        if (mappedStickX < STICK_DIRECTION_RELEASE_THRESHOLD) {
            gPlayerInputHeld[playerIndex] &= ~STICK_RIGHT;
        }
        if (mappedStickX > -STICK_DIRECTION_RELEASE_THRESHOLD) {
            gPlayerInputHeld[playerIndex] &= ~STICK_LEFT;
        }
        if (mappedStickY < STICK_DIRECTION_RELEASE_THRESHOLD) {
            gPlayerInputHeld[playerIndex] &= ~STICK_UP;
        }
        if (mappedStickY > -STICK_DIRECTION_RELEASE_THRESHOLD) {
            gPlayerInputHeld[playerIndex] &= ~STICK_DOWN;
        }
        heldInput = gPlayerInputHeld[playerIndex];
        gPlayerInputPressed[playerIndex] = ~*previousInput & heldInput;
        if (heldInput == 0) {
            gPlayerInputRepeatTimer[playerIndex] = 0;
            gPlayerInputRepeat[playerIndex] = heldInput;
        } else {
            if (gPlayerInputRepeatTimer[playerIndex] >= PLAYER_INPUT_REPEAT_DELAY) {
                gPlayerInputRepeat[playerIndex] = heldInput;
            } else {
                gPlayerInputRepeatTimer[playerIndex]++;
                gPlayerInputRepeat[playerIndex] = gPlayerInputPressed[playerIndex];
            }
        }
    }

    for (gCurrentGameTask = gActiveGameTaskList.next; gCurrentGameTask != NULL;
         gCurrentGameTask = gCurrentGameTask->next) {
        if (gCurrentGameTask->state == GAME_TASK_STATE_PENDING) {
            gCurrentGameTask->state = GAME_TASK_STATE_ACTIVE;
        }
    }

    for (gCurrentGameTask = gActiveGameTaskList.next; gCurrentGameTask != NULL;
         gCurrentGameTask = gCurrentGameTask->next) {
        if (gCurrentGameTask->state == GAME_TASK_STATE_ACTIVE) {
            if (gCurrentGameTask->callbacks[0] != NULL) {
                gCurrentGameTask->callbacks[0]();
            }
            if (gCurrentGameTask->callbacks[1] != NULL) {
                gCurrentGameTask->callbacks[1]();
            }
            if (gCurrentGameTask->callbacks[2] != NULL) {
                gCurrentGameTask->callbacks[2]();
            }
        }
    }

    // @recomp Hash game state now that all game logic for this tick has run (before render submission).
    recomp_det_post_tick(gRacePlayers, &gMainRngIndex, &gSecondaryRngIndex, &gFrameCounter);

    updateFramebufferRenderScheduler();
    playPendingPositionalSoundRequests();
}
