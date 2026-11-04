#include "tot_manager_title.h"

#include "common_functions.h"
#include "common_ui.h"
#include "patch.h"

#include <gc/OSTime.h>
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

// Hint for special save file names, debug mode cheat codes, and the debug
// menu's controls, shown on the title screen. Both new files and existing
// files renamed at the file select screen (Y) get these modes.
constexpr const char* kSpecialFileNamesInfo =
    "Special file names:\n"
    "RaceMode: Race mode\n"
    "YCNOTGBY: 100% mode\n"
    "xyzzy: Debug mode\n"
    "\n"
    "Debug mode only, in-game codes:\n"
    "L,Y,L,Y,L,Y,L,Y: Open debug menu\n"
    "R,L,Y,X,B,A,L,A: Get Bump Attack\n"
    "L,Z,L,Z,L,Z,L,Z: End countdown timer\n"
    "\n"
    "Debug menu controls:\n"
    "D-Pad: Move, Y: Select\n"
    "(L+Y on achievements: include secret)\n"
    "Enemies: Up/Down: Slot, Left/Right: Enemy\n"
    "  (hold L: +/-16), Y: Clear, L+Y: Done\n"
    "Floor: D-Pad: Change (hold L: +/-8), Y: Set";

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

// Address ranges of the title / logo screens' code, used to identify fades
// started by those screens (e.g. the white fade-in to the title screen).
constexpr const uintptr_t kSeqTitleTextStart    = 0x80008888;
constexpr const uintptr_t kSeqTitleTextEnd      = 0x80009e00;
constexpr const uintptr_t kSeqLogoTextStart     = 0x800859f8;
constexpr const uintptr_t kSeqLogoTextEnd       = 0x80087500;
// evt_fade_entry (and the function after it), which calls fadeEntry.
constexpr const uintptr_t kEvtFadeEntryStart    = 0x80135fdc;
constexpr const uintptr_t kEvtFadeEntryEnd      = 0x80136080;
// fadeEntry lies in the fade driver's code, after fadeIsFinish and before
// the next object file's code (hitdrv).
constexpr const uintptr_t kFadeEntrySearchStart = 0x80010d94;
constexpr const uintptr_t kFadeEntrySearchEnd   = 0x80013cec;

// Hook for fadeEntry(type, duration, color); extra argument registers are
// passed through as-is in case the function takes more than three.
void (*g_fadeEntry_trampoline)(
    int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t) = nullptr;

// --- Temporary diagnostics for title screen timing. -------------------------
// Shows a timeline of screen / title state changes and fades (with frame
// numbers and real time) on the logo and title screens, plus the live frame
// rate. Set to false to hide.
constexpr const bool kShowTitleDiagnostics = true;

struct FadeCall {
    int32_t type;
    int32_t duration;
    int32_t new_duration;
    uintptr_t caller;
};
constexpr const int32_t kMaxFadeCalls = 8;
FadeCall g_FadeCalls[kMaxFadeCalls];
volatile uint32_t g_NumFadeCalls = 0;
uint32_t g_NumFadeCallsLogged = 0;
uintptr_t g_FadeEntryAddress = 0;
bool g_FadeEntryHooked = false;

constexpr const int32_t kMaxDiagEvents = 22;
char g_DiagEvents[kMaxDiagEvents][48];
int32_t g_NumDiagEvents = 0;
uint32_t g_DiagFrame = 0;
uint64_t g_DiagStartTime = 0;
uint64_t g_DiagLastTime = 0;
float g_DiagFps = 0.0f;
int32_t g_DiagLastSeq = -1;
int32_t g_DiagLastNextSeq = -1;
int32_t g_DiagLastTitleState = -1;
int32_t g_DiagLastFadeTypes[5] = { -1, -1, -1, -1, -1 };
char g_DiagStatus[160];

uint32_t TicksToCentis(uint64_t ticks) {
    return DurationTicksToCentiseconds(static_cast<int64_t>(ticks));
}

void AddDiagEvent(const char* text) {
    // Once the log is full, keep the oldest entries and replace the last one,
    // so the start of the boot sequence stays visible.
    int32_t index = g_NumDiagEvents;
    if (index >= kMaxDiagEvents) index = kMaxDiagEvents - 1;
    else ++g_NumDiagEvents;
    const uint32_t centis =
        TicksToCentis(gc::OSTime::OSGetTime() - g_DiagStartTime);
    snprintf(
        g_DiagEvents[index], sizeof(g_DiagEvents[index]),
        "%4" PRIu32 " %3" PRIu32 ".%02" PRIu32 "s %s",
        g_DiagFrame, centis / 100, centis % 100, text);
}

void UpdateDiagnostics() {
    if (!kShowTitleDiagnostics) return;
    const uint64_t now = gc::OSTime::OSGetTime();
    if (g_DiagFrame == 0) {
        g_DiagStartTime = now;
        g_DiagLastTime = now;
        AddDiagEvent(g_FadeEntryHooked ? "fade hook on" : "fade hook OFF");
    }
    ++g_DiagFrame;

    // Smoothed frame rate.
    const uint32_t frame_centis = TicksToCentis(now - g_DiagLastTime);
    g_DiagLastTime = now;
    if (frame_centis > 0) {
        const float fps = 100.0f / frame_centis;
        g_DiagFps = g_DiagFps * 0.9f + fps * 0.1f;
    }

    char buf[48];
    const int32_t seq = static_cast<int32_t>(ttyd::seqdrv::seqGetSeq());
    const int32_t next_seq =
        static_cast<int32_t>(ttyd::seqdrv::seqGetNextSeq());
    if (seq != g_DiagLastSeq || next_seq != g_DiagLastNextSeq) {
        snprintf(buf, sizeof(buf), "seq %" PRId32 " (next %" PRId32 ")",
                 seq, next_seq);
        AddDiagEvent(buf);
        g_DiagLastSeq = seq;
        g_DiagLastNextSeq = next_seq;
    }

    auto* title = reinterpret_cast<TitleWork*>(
        ttyd::seq_title::seqTitleWorkPointer2);
    int32_t title_state = -1;
    if (seq == static_cast<int32_t>(ttyd::seqdrv::SeqIndex::kTitle) && title) {
        title_state = title->state;
    }
    if (title_state != g_DiagLastTitleState) {
        snprintf(buf, sizeof(buf), "title state %" PRId32, title_state);
        AddDiagEvent(buf);
        g_DiagLastTitleState = title_state;
    }

    auto* fade_work = reinterpret_cast<FadeWork*>(
        ttyd::fadedrv::fadedrvWorkPointer);
    if (fade_work) {
        for (int32_t i = 0; i < 5; ++i) {
            const auto& entry = fade_work->entries[i];
            const int32_t type =
                (entry.flags & kFadeEntryActive) ? entry.type : -1;
            if (type != g_DiagLastFadeTypes[i]) {
                if (type >= 0) {
                    snprintf(buf, sizeof(buf),
                        "fade%" PRId32 " start type %" PRId32 " fl %" PRIx32,
                        i, type, static_cast<uint32_t>(entry.flags));
                } else {
                    snprintf(buf, sizeof(buf), "fade%" PRId32 " end", i);
                }
                AddDiagEvent(buf);
                g_DiagLastFadeTypes[i] = type;
            }
        }
    }

    while (g_NumFadeCallsLogged < g_NumFadeCalls) {
        const auto& call = g_FadeCalls[g_NumFadeCallsLogged % kMaxFadeCalls];
        snprintf(buf, sizeof(buf),
            "fadeEntry t%" PRId32 " d%" PRId32 "->%" PRId32 " %08" PRIx32,
            call.type, call.duration, call.new_duration,
            static_cast<uint32_t>(call.caller));
        AddDiagEvent(buf);
        ++g_NumFadeCallsLogged;
    }

    if (title) {
        snprintf(g_DiagStatus, sizeof(g_DiagStatus),
            "fps %.1f  seq %" PRId32 "/%" PRId32 "\n"
            "title st %" PRId32 " cnt %" PRId32 " tmr %" PRId32
            " a %.2f",
            g_DiagFps, seq, next_seq, title_state,
            title_state >= 0 ? title->counter : -1,
            title_state >= 0 ? title->timer : -1,
            title_state >= 0 ? title->fade_alpha : -1.0f);
    } else {
        snprintf(g_DiagStatus, sizeof(g_DiagStatus),
            "fps %.1f  seq %" PRId32 "/%" PRId32,
            g_DiagFps, seq, next_seq);
    }
}

void DrawDiagnostics() {
    if (!kShowTitleDiagnostics) return;
    const auto seq = ttyd::seqdrv::seqGetSeq();
    if (seq != ttyd::seqdrv::SeqIndex::kLogo &&
        seq != ttyd::seqdrv::SeqIndex::kTitle) {
        return;
    }
    const float scale = 0.42f;
    const float left = -296.0f;
    float y = 220.0f;
    const float line_height = 25.0f * scale;
    const int32_t num_lines = g_NumDiagEvents + 3;
    DrawWindow(
        0x000000C0u, left - 6.0f, y + 6.0f, 270.0f,
        num_lines * line_height + 12.0f, 6);
    DrawText(g_DiagStatus, left, y, 0xFF, true, 0xFFFF80FFu, scale, 0);
    y -= line_height * 2.5f;
    for (int32_t i = 0; i < g_NumDiagEvents; ++i) {
        DrawText(g_DiagEvents[i], left, y, 0xFF, true, 0xFFFFFFFFu, scale, 0);
        y -= line_height;
    }
}
// --- End of temporary diagnostics. -------------------------------------------

bool IsTitleOrLogoCode(uintptr_t addr) {
    return (addr >= kSeqTitleTextStart && addr < kSeqTitleTextEnd) ||
        (addr >= kSeqLogoTextStart && addr < kSeqLogoTextEnd);
}

// Speeds up (non-curtain) fades started by the title / logo screens, such as
// the white fade-in to the title screen, by shortening their duration. The
// hook is branched to from fadeEntry's first instruction, so the return
// address is that of fadeEntry's caller.
__attribute__((noinline)) void fadeEntry_hook(
    int32_t type, int32_t duration, uint32_t a2, uint32_t a3, uint32_t a4,
    uint32_t a5, uint32_t a6, uint32_t a7) {
    const uintptr_t caller =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const int32_t original_duration = duration;
    if (IsTitleOrLogoCode(caller) && !IsCurtainFade(type) &&
        duration > kTitleAnimSpeed) {
        duration /= kTitleAnimSpeed;
    }
    // Record the call for the diagnostics overlay (no function calls here).
    auto& call = g_FadeCalls[g_NumFadeCalls % kMaxFadeCalls];
    call.type = type;
    call.duration = original_duration;
    call.new_duration = duration;
    call.caller = caller;
    ++g_NumFadeCalls;

    g_fadeEntry_trampoline(type, duration, a2, a3, a4, a5, a6, a7);
}

// Finds fadeEntry's address from the call to it in evt_fade_entry, since it
// isn't in the symbol map; returns 0 if it can't be found.
uintptr_t FindFadeEntry() {
    for (uintptr_t addr = kEvtFadeEntryStart; addr < kEvtFadeEntryEnd;
         addr += 4) {
        const uint32_t instr = *reinterpret_cast<const uint32_t*>(addr);
        // Look for "bl <relative address>" instructions.
        if ((instr & 0xfc000003U) != 0x48000001U) continue;
        int32_t offset = static_cast<int32_t>(instr & 0x03fffffcU);
        if (offset & 0x02000000) offset -= 0x04000000;
        const uintptr_t target = addr + offset;
        if (target > kFadeEntrySearchStart && target < kFadeEntrySearchEnd) {
            return target;
        }
    }
    return 0;
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
                // Wait before "Press Start" fades in and input is accepted;
                // skip almost all of it.
                if (work->counter < fps) work->counter = fps;
                break;
        }
    }
}

}

void TitleScreenManager::Init() {
    const uintptr_t fade_entry = FindFadeEntry();
    g_FadeEntryAddress = fade_entry;
    if (!fade_entry) return;
    // Don't hook if the first instruction is a relative branch, since it
    // couldn't be moved to the trampoline.
    const uint32_t first_instr = *reinterpret_cast<const uint32_t*>(fade_entry);
    const uint32_t opcode = first_instr >> 26;
    if (opcode == 16 || opcode == 18) return;

    g_fadeEntry_trampoline = mod::hookFunction(
        reinterpret_cast<decltype(g_fadeEntry_trampoline)>(fade_entry),
        fadeEntry_hook);
    g_FadeEntryHooked = true;
}

void TitleScreenManager::Update() {
    UpdateDiagnostics();
    if (!CheckSeq(ttyd::seqdrv::SeqIndex::kTitle)) return;
    SpeedUpTitleCurtains();
    SpeedUpTitleLogo();
}

void TitleScreenManager::Draw() {
    DrawDiagnostics();

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

            // Draw special file name / debug mode hints in the top-right.
            const float scale = 0.42f;
            const float pad = 8.0f;
            const float right = 296.0f;
            const float top = 220.0f;
            float width, height;
            GetTextDimensions(kSpecialFileNamesInfo, scale, &width, &height);
            const float left = right - width;
            DrawWindow(
                0x000000E5u, left - pad, top + pad,
                width + 2 * pad, height + 2 * pad, 8);
            DrawText(
                kSpecialFileNamesInfo, left, top, 0xFFu, true,
                0xFFFFFFFFu, scale, /* top-left */ 0);
        }
    }
}

const char* TitleScreenManager::GetVersionString() {
    return kVersion;
}

}