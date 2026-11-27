// @recomp Netplay: fix the race setup player count to the number of players in the netplay session.
#include "patches.h"

#include "game/audio/sound_manager.h"
#include "game/engine/callback_task_scheduler.h"
#include "game/engine/controller_input.h"
#include "game/engine/game_task_scheduler.h"
#include "game/menu/main_menu/controller_main_menu_flow.h"
#include "game/menu/main_menu/controller_subsystem.h"
#include "game/menu/race_setup/race_setup_menu.h"
#include "game/race/player/race_player_input.h"

// The number of players in a running netplay session, or 0 without netplay (src/game/netplay.cpp).
extern s32 recomp_get_netplay_player_count(void);

RECOMP_PATCH void updateRaceSetupPlayerCountMenu(void) {
    s32 confirmationNext;
    s32 pressedInput;
    s32 substateNext;
    s32 temp_v0;
    u8 temp_v1;
    u32 confirmationValue;
    u16 repeatTimer;
    s32 one;
    s32 sessionPlayers;

    one = 1;
    // @recomp Netplay: the session already knows how many players there are, so the count is fixed to it (every
    // device does the same, so the game stays in step). A smaller count would leave players watching; a larger one
    // would wait forever for players who do not exist. Options above it show as unavailable.
    sessionPlayers = recomp_get_netplay_player_count();
    if (sessionPlayers != 0) {
        gConnectedControllerCount = sessionPlayers;
        gPlayerCount = sessionPlayers;
    }
    substateNext = 2;
    if ((gRaceSetupMenuSubState.state == one) &&
        ((gPlayerInputPressed[0] & A_BUTTON) || (gPlayerInputPressed[0] & START_BUTTON))) {
        enqueueSoundEffect(one, SFX_PRIORITY_NORMAL);
        gRaceSetupMenuSubState.state = substateNext;
        gRaceSetupMenuSubState.timer = 0;
    }
    if (gCurrentGameTask->callbackData1 == one) {
        temp_v1 = gMenuSelectionConfirmTimer;
        confirmationValue = temp_v1;
        temp_v0 = temp_v1;
        if (temp_v1 == 0) {
            pressedInput = gPlayerInputPressed[0];
            temp_v0 = gPlayerInputHeld[0] & (STICK_UP | U_JPAD);
            if ((temp_v0 == 0) && !(gPlayerInputHeld[0] & (STICK_DOWN | D_JPAD))) {
                gMenuInputRepeatTimers[0] = 0;
            }
            if ((pressedInput & (STICK_UP | U_JPAD)) || ((temp_v0 != 0) && ((s32)gMenuInputRepeatTimers[0] >= 9) &&
                                                         (((s32)gMenuInputRepeatTimers[0] % 3) == 0))) {
                if (gMenuInputRepeatTimers[0] == 0) {
                    gMenuInputRepeatTimers[0] += 1;
                }
                if (sessionPlayers == 0 && gPlayerCount != one) {
                    gPlayerCount -= 1;
                    enqueueSoundEffect(SFX_MENU_CURSOR, SFX_PRIORITY_NORMAL);
                }
            } else if (
                (pressedInput & (STICK_DOWN | D_JPAD)) ||
                ((gPlayerInputHeld[0] & (STICK_DOWN | D_JPAD)) && ((s32)gMenuInputRepeatTimers[0] >= 9) &&
                 (((s32)gMenuInputRepeatTimers[0] % 3) == 0))
            ) {
                if (gMenuInputRepeatTimers[0] == 0) {
                    gMenuInputRepeatTimers[0] += 1;
                }
                if (sessionPlayers == 0 && gPlayerCount != gConnectedControllerCount) {
                    gPlayerCount += 1;
                    enqueueSoundEffect(SFX_MENU_CURSOR, SFX_PRIORITY_NORMAL);
                }
            }
            repeatTimer = gMenuInputRepeatTimers[0];
            if (repeatTimer != 0) {
                gMenuInputRepeatTimers[0] = repeatTimer + 1;
                if (gMenuInputRepeatTimers[0] == 0xFFFF) {
                    gMenuInputRepeatTimers[0] = 0xA;
                }
            }
            if ((pressedInput & A_BUTTON) || (pressedInput & START_BUTTON)) {
                gMenuSelectionConfirmTimer = 1;
                enqueueSoundEffect(SFX_CONFIRM, SFX_PRIORITY_NORMAL);
            }
        } else if (temp_v0 < 0x13) {
            gMenuSelectionConfirmTimer = (confirmationNext = confirmationValue + 1);
        }
    }
    if (gRaceSetupMenuSubState.state == RACE_SETUP_PROMPT_CHECKING) {
        setCurrentGameTaskCallback(initRaceSetupSaveMenu, 0);
        gCurrentGameTask->callbackData0 = 0;
        gMenuSelectionConfirmTimer = 0;
    }
    updateCallbackTasks();
}
