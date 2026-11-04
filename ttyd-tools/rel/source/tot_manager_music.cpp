#include "tot_manager_music.h"

#include "mod.h"
#include "tot_gsw.h"
#include "tot_state.h"

#include <gc/dvd.h>
#include <ttyd/npcdrv.h>
#include <ttyd/pmario_sound.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mod::tot {

namespace {

using ::ttyd::pmario_sound::PSndBgmListEntry;

// Vanilla entries the tower uses for battle music (see tot_generate_enemy).
constexpr const char* kBattleBgmNames[] = {
    "BGM_ZAKO_BATTLE1",
    "BGM_KOBOSS_BATTLE1",
    "BGM_BOSS_STG4_RUNPELL1",
    "BGM_BOSS_STG1_GONBABA1",
    "BGM_BOSS_STG8_KOOPA",
};
// Vanilla victory fanfares; which one plays depends on battle-end state.
constexpr const char* kVictoryBgmNames[] = {
    "BGM_BATTLE_WIN1",
    "BGM_BATTLE_WIN2",
    "BGM_BATTLE_WIN3",
};
constexpr int32_t kNumBattleNames =
    sizeof(kBattleBgmNames) / sizeof(kBattleBgmNames[0]);
constexpr int32_t kNumVictoryNames =
    sizeof(kVictoryBgmNames) / sizeof(kVictoryBgmNames[0]);
constexpr int32_t kNumRedirects = kNumBattleNames + kNumVictoryNames;

// Remake file stems for each battle kind (index = MusicManager::BattleKind).
constexpr const char* kBossStems[] = {
    nullptr, nullptr, "midboss", "boss_doopliss", "boss_hooktail",
    "boss_gloomtail", "boss_bonetail", "boss_bowser",
};

struct Redirect {
    PSndBgmListEntry* entry;
    const char* original_path;
    char path[48];
};
Redirect g_Redirects[kNumRedirects];
bool g_Initialized = false;
int32_t g_BattleKind = MusicManager::BATTLE_NONE;

PSndBgmListEntry* FindEntry(const char* name) {
    for (auto& entry : ttyd::pmario_sound::psbgmlist) {
        if (entry.name && !strcmp(entry.name, name)) return &entry;
    }
    return nullptr;
}

void Initialize() {
    if (g_Initialized) return;
    for (int32_t i = 0; i < kNumRedirects; ++i) {
        const char* name = i < kNumBattleNames
            ? kBattleBgmNames[i] : kVictoryBgmNames[i - kNumBattleNames];
        auto& r = g_Redirects[i];
        r.entry = FindEntry(name);
        r.original_path = r.entry ? r.entry->path : nullptr;
        r.path[0] = '\0';
    }
    g_Initialized = true;
}

void RestoreAll() {
    for (auto& r : g_Redirects) {
        if (r.entry) r.entry->path = r.original_path;
    }
}

// Whether "sound/stream/tot_<stem>_32k.stm" exists on the disc.
bool StreamExists(const char* stem) {
    char buf[64];
    sprintf(buf, "sound/stream/tot_%s_32k.stm", stem);
    return gc::dvd::DVDConvertPathToEntrynum(buf) >= 0;
}

// Points the redirect at the first stem that exists; else uses the original.
void Apply(Redirect& r, const char* stem, const char* fallback_stem) {
    if (!r.entry) return;
    const char* use = nullptr;
    if (stem && StreamExists(stem)) {
        use = stem;
    } else if (fallback_stem && StreamExists(fallback_stem)) {
        use = fallback_stem;
    }
    if (use) {
        sprintf(r.path, "sound/stream/tot_%s_32k", use);
        r.entry->path = r.path;
    } else {
        r.entry->path = r.original_path;
    }
}

// Which quarter of the tower the current floor is in (1-4).
// 32-floor runs switch every 8 floors, 64-floor runs every 16.
int32_t GetSection() {
    const auto& state = g_Mod->state_;
    int32_t num_floors = state.GetNumFloors();
    int32_t floor = state.floor_ < 1 ? 1 : state.floor_;
    int32_t section = (floor - 1) * 4 / num_floors + 1;
    return section < 1 ? 1 : section > 4 ? 4 : section;
}

}  // namespace

void MusicManager::SetBattleKind(int32_t kind) {
    g_BattleKind = kind;
}

bool MusicManager::IsRemakeEnabled() {
    return GetSWF(GSWF_BgmEnabled) && GetSWF(GSWF_RemakeBgmEnabled);
}

void MusicManager::OnBgmStart(const char* name) {
    // Names can also be raw table indices; those are never redirected.
    if (reinterpret_cast<uintptr_t>(name) < 0x8000'0000U) return;
    Initialize();

    int32_t battle_idx = -1;
    int32_t victory_idx = -1;
    for (int32_t i = 0; i < kNumBattleNames; ++i) {
        if (!strcmp(name, kBattleBgmNames[i])) battle_idx = i;
    }
    for (int32_t i = 0; i < kNumVictoryNames; ++i) {
        if (!strcmp(name, kVictoryBgmNames[i])) victory_idx = i;
    }
    if (battle_idx < 0 && victory_idx < 0) return;

    if (!IsRemakeEnabled() || g_BattleKind == BATTLE_NONE ||
        !g_Mod->state_.GetOption(OPT_RUN_STARTED)) {
        RestoreAll();
        return;
    }

    char stem[32];
    char fallback[32];
    const char* fallback_stem = nullptr;
    if (g_BattleKind == BATTLE_NORMAL) {
        int32_t section = GetSection();
        if (victory_idx >= 0) {
            sprintf(stem, "s%" PRId32 "_victory", section);
        } else {
            // 0 = no first strike, 1-8 = Mario / partner, 9+ = enemy.
            uint32_t first_attack =
                ttyd::npcdrv::fbatGetPointer()->battleInfo.wFirstAttack;
            const char* variant =
                first_attack == 0 ? "battle" :
                first_attack < 9  ? "mario_first" : "enemy_first";
            sprintf(stem, "s%" PRId32 "_%s", section, variant);
            sprintf(fallback, "s%" PRId32 "_battle", section);
            fallback_stem = fallback;
        }
    } else {
        sprintf(stem, victory_idx >= 0 ? "%s_victory" : "%s",
                kBossStems[g_BattleKind]);
    }

    auto& r = battle_idx >= 0
        ? g_Redirects[battle_idx]
        : g_Redirects[kNumBattleNames + victory_idx];
    Apply(r, stem, fallback_stem);
}

bool MusicManager::CycleMusicOption() {
    if (!GetSWF(GSWF_BgmEnabled)) {
        SetSWF(GSWF_BgmEnabled, 1);
        SetSWF(GSWF_RemakeBgmEnabled, 0);
        return true;
    }
    if (!GetSWF(GSWF_RemakeBgmEnabled)) {
        SetSWF(GSWF_RemakeBgmEnabled, 1);
        return true;
    }
    SetSWF(GSWF_BgmEnabled, 0);
    SetSWF(GSWF_RemakeBgmEnabled, 0);
    return false;
}

const char* MusicManager::GetToggleDescription() {
    const char* mode = !GetSWF(GSWF_BgmEnabled) ? "Off" :
        GetSWF(GSWF_RemakeBgmEnabled) ? "Remake" : "Original";
    static char buf[96];
    sprintf(buf, "Cycle music: Off, Original, or\n"
                 "Remake battles. Now: %s", mode);
    return buf;
}

}  // namespace mod::tot
