#include "tot_manager_title.h"

#include "common_functions.h"
#include "common_ui.h"

#include <ttyd/animdrv.h>
#include <ttyd/fadedrv.h>
#include <ttyd/mariost.h>
#include <ttyd/seqdrv.h>
#include <ttyd/seq_title.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace mod::tot {

namespace {

constexpr const char* kVersion = "v4.01";
constexpr const char* kTitleInfo = "v4.01 r62" "\n" "2026-03-11";

// How many times faster than vanilla the title screen's curtain and logo
// animations play (must be an integer >= 1).
constexpr int32_t kTitleAnimSpeed = 2;

// Partial layout of the vanilla title screen's work (seqTitleWorkPointer2).
struct TitleWork {
    void*       texture;        // 0x00
    uint8_t     unk_0x04[4];    // 0x04
    int32_t     state;          // 0x08
    float       top_pos_x;      // 0x0c
    float       top_pos_y;      // 0x10
    float       bottom_pos_x;   // 0x14
    float       bottom_pos_y;   // 0x18
    float       fade_alpha;     // 0x1c
    int32_t     counter;        // 0x20
    int32_t     timer;          // 0x24
    int32_t     idle;           // 0x28
};
static_assert(sizeof(TitleWork) == 0x2c);

// Partial layout of the fade driver's work (fadedrvWorkPointer).
struct FadeEntry {
    uint16_t    flags;          // 0x00
    uint8_t     unk_0x02[2];    // 0x02
    int32_t     type;           // 0x04
    int32_t     state;          // 0x08
    uint8_t     unk_0x0c[0x6c - 0x0c];
    int32_t     pose_id;        // 0x6c
    uint8_t     unk_0x70[0x94 - 0x70];
    int32_t     scale_timer;    // 0x94
    uint8_t     unk_0x98[0xa8 - 0x98];
};
static_assert(sizeof(FadeEntry) == 0xa8);

struct FadeWork {
    uint16_t    flags;          // 0x00
    uint8_t     unk_0x02[6];    // 0x02
    FadeEntry   entries[5];     // 0x08
};

constexpr const uint16_t kFadeEntryActive = 0x1;
constexpr const uint16_t kFadeEntryFast   = 0x8;

// Fade types used by the title screen.
constexpr const int32_t kFadeCurtainFirst       = 21;   // VERTICAL_A1
constexpr const int32_t kFadeCurtainLast        = 39;   // ENTER (2D)
constexpr const int32_t kFadeCurtainEnter3d     = 33;
constexpr const int32_t kFadeCurtainEnter       = 39;
constexpr const int32_t kFadeCurtainSidesA1Top  = 62;
constexpr const int32_t kFadeCurtainSidesA2Top  = 63;

bool IsCurtainFade(int32_t type) {
    return (type >= kFadeCurtainFirst && type <= kFadeCurtainLast) ||
        type == kFadeCurtainSidesA1Top || type == kFadeCurtainSidesA2Top;
}

// Speeds up the curtain animations, which are run by the fade driver.
void SpeedUpTitleCurtains() {
    auto* fade_work = reinterpret_cast<FadeWork*>(
        ttyd::fadedrv::fadedrvWorkPointer);
    if (!fade_work) return;

    const int32_t fps =
        static_cast<int32_t>(ttyd::mariost::g_MarioSt->fps);

    for (auto& entry : fade_work->entries) {
        if (!(entry.flags & kFadeEntryActive) || !IsCurtainFade(entry.type))
            continue;

        // Play the curtain model's animation faster.
        if (entry.pose_id >= 0) {
            ttyd::animdrv::animPoseSetLocalTimeRate(
                entry.pose_id, static_cast<float>(kTitleAnimSpeed));
        }

        // The curtain zooming in (when booting / returning to the title
        // screen) is driven by a frame counter that has to land exactly on
        // its end value, so advance it extra, but never past the end.
        if ((entry.type == kFadeCurtainEnter3d ||
             entry.type == kFadeCurtainEnter) && entry.state == 1) {
            const int32_t length =
                (entry.flags & kFadeEntryFast ? 90 : 180) * fps / 60;
            for (int32_t i = 1; i < kTitleAnimSpeed; ++i) {
                if (entry.scale_timer + 1 < length) ++entry.scale_timer;
            }
        }
    }
}

// Speeds up the title logo sliding in and the delay before "Press Start".
void SpeedUpTitleLogo() {
    auto* work = reinterpret_cast<TitleWork*>(
        ttyd::seq_title::seqTitleWorkPointer2);
    if (!work) return;

    const int32_t fps =
        static_cast<int32_t>(ttyd::mariost::g_MarioSt->fps);

    for (int32_t i = 1; i < kTitleAnimSpeed; ++i) {
        switch (work->state) {
            case 1:
            case 2:
                // Countdown to unpausing the BGM; must land exactly on 0.
                if (work->timer > 1) --work->timer;
                // Logo halves slide in over 'fps' frames, ending once the
                // counter exceeds 'fps'; don't skip past the final position.
                if (work->state == 2 && work->counter + 1 < fps) {
                    ++work->counter;
                }
                break;
            case 3:
                // Wait before "Press Start" fades in and input is accepted.
                if (work->counter <= fps) ++work->counter;
                break;
        }
    }
}

}

void TitleScreenManager::Update() {
    if (!CheckSeq(ttyd::seqdrv::SeqIndex::kTitle)) return;
    SpeedUpTitleCurtains();
    SpeedUpTitleLogo();
}

void TitleScreenManager::Draw() {
    // If on the title screen and the curtain is not fully down.
    if (CheckSeq(ttyd::seqdrv::SeqIndex::kTitle)) {
        const uint32_t curtain_state = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(ttyd::seq_title::seqTitleWorkPointer2)
            + 0x8);
        if (curtain_state >= 2 && curtain_state < 12) {
            // Draw title screen information.
            DrawCenteredTextWindow(
                kTitleInfo, 230, -180, 0xFFu, true, 0xFFFFFFFFu, 0.52f, 
                0x000000E5u, 8, 8);
        }
    }
}

const char* TitleScreenManager::GetVersionString() {
    return kVersion;
}

}