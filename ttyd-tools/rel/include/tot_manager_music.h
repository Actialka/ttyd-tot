#pragma once

#include <cstdint>

namespace mod::tot {

// Optional "remake" battle music, played from extra .stm streams added to the
// disc (sound/stream/tot_*_32k.stm; see ttyd-tools/remake_music).
//
// The game's BGM table is compiled into main.dol, so instead of adding new
// track names, the stream path of the vanilla battle / victory entries is
// redirected just before they are played. Any remake file that isn't present
// on the disc falls back to the original track.
class MusicManager {
public:
    // The kinds of tower battle that have their own remake themes.
    enum BattleKind {
        BATTLE_NONE = 0,    // Keeps the original music (e.g. Gold Fuzzy).
        BATTLE_NORMAL,
        BATTLE_MIDBOSS,
        BATTLE_DOOPLISS,
        BATTLE_HOOKTAIL,
        BATTLE_GLOOMTAIL,
        BATTLE_BONETAIL,
        BATTLE_BOWSER,
    };

    // Records which kind of battle the current floor's encounter is.
    static void SetBattleKind(int32_t kind);

    // Called right before a BGM starts; redirects battle / victory entries to
    // remake streams if enabled, or restores the original streams otherwise.
    static void OnBgmStart(const char* name);

    // Whether the remake battle music option is active.
    static bool IsRemakeEnabled();

    // Cycles the music option: Off -> Original -> Remake -> Off.
    // Returns whether music is on after the change.
    static bool CycleMusicOption();

    // Description for the music toggle key item, reflecting the current mode.
    static const char* GetToggleDescription();
};

}  // namespace mod::tot
