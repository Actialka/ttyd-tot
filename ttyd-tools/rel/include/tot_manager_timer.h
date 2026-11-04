#pragma once

#include <ttyd/evtmgr.h>

#include <cstdint>

namespace mod::tot {
    
class TimerManager {
public:
    // Function that updates ToT timer tracking functions every frame.
    static void Update();
    // Function that draws the current time to the screen.
    static void Draw();

    // Useful wrapper functions.
    static int32_t GetCurrentRunTotalTimeCentis();
    static int32_t GetCurrentRunTotalBattleTimeCentis();
    static int32_t GetNumberOfBattles();
    // Returns the countdown timer's time per set of floors, in seconds,
    // based on the current run's options.
    static int32_t GetCountdownSecondsPerSet();
};
// Toggles the in-game timer on or off.
EVT_DECLARE_USER_FUNC(evtTot_ToggleIGT, 1)
// Tracks the completion.
EVT_DECLARE_USER_FUNC(evtTot_TrackCompletedRun, 0)
// Tracks a run being lost to the countdown timer running out.
EVT_DECLARE_USER_FUNC(evtTot_TrackCountdownTimeUp, 0)

}