#include "tot_manager_file_rename.h"

#include "common_types.h"
#include "common_ui.h"
#include "evt_cmd.h"
#include "patch.h"
#include "tot_state.h"

#include <ttyd/cardmgr.h>
#include <ttyd/evt_img.h>
#include <ttyd/evt_paper.h>
#include <ttyd/evt_window.h>
#include <ttyd/fadedrv.h>
#include <ttyd/evtmgr.h>
#include <ttyd/pmario_sound.h>
#include <ttyd/seqdrv.h>
#include <ttyd/system.h>

#include <cstring>

namespace mod::tot {

namespace {

using ::ttyd::evtmgr::EvtEntry;
using ::ttyd::seqdrv::SeqIndex;

constexpr int32_t kNumSaveFiles = 4;
// Offset of the file select screen's work pointer from r13, and the offset of
// the currently hovered file / button index in that work.
constexpr int32_t kSeqLoadWorkR13Offset = -0x6550;
constexpr int32_t kSeqLoadWorkHoveredOffset = 0x20;
// Address range of the vanilla file select code (seq_load.o's .text).
constexpr uintptr_t kSeqLoadTextStart = 0x800f39dc;
constexpr uintptr_t kSeqLoadTextEnd   = 0x800f74b4;
// Type of name entry window to open (the same one used for Bub-ulb's seeds).
constexpr int32_t kNameEntryType = 3;

// Value returned by evt user functions once they've finished.
constexpr int32_t kEvtReturnDone = 2;

// Trampolines for the game's input functions. While renaming, the hooked
// versions report no input to the vanilla file select menu's code, so it
// doesn't react to buttons pressed in the name entry window.
uint32_t (*g_keyGetButtonTrg_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetButtonRep_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetButton_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDirTrg_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDirRep_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDir_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetStickX_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetStickY_trampoline)(uint32_t) = nullptr;

// Whether a file is currently being renamed (including the animations).
bool g_Active = false;
// Set after closing the window; keeps the file select menu's input blocked
// until the buttons that might have closed it are released.
bool g_WaitForRelease = false;
// The existing save file currently hovered on the file select screen, or -1.
int32_t g_HoveredFile = -1;
// How many frames the file select screen has been fully faded in for.
int32_t g_FadedInFrames = 0;
// The file being renamed.
int32_t g_File = -1;

// Stand-in evt context for calling evt functions directly, since evts aren't
// otherwise run on the file select screen.
EvtEntry g_Evt;

// A step in a sequence of evt function calls, run like an evt script would:
// each function is called until it reports it's done, or waits some frames.
using EvtFunc = int32_t (*)(EvtEntry*, bool);
struct EvtStep {
    EvtFunc func;
    int32_t args[8];
    int32_t wait_frames;
};

// Phases of renaming a file.
enum class Phase {
    OPENING,    // Screen capture rolls up, name entry window opens.
    ENTERING,   // Waiting for the player to finish entering a name.
    CLOSING,    // Paper rolls back down, everything is cleaned up.
};
Phase g_Phase = Phase::OPENING;

// Currently running sequence of steps.
const EvtStep* g_Steps = nullptr;
int32_t g_NumSteps = 0;
int32_t g_StepIndex = 0;
int32_t g_StepFrames = 0;
bool g_StepFirstCall = true;
// Countdown to play a sound effect partway through an animation (-1 = none).
int32_t g_SfxTimer = -1;
const char* g_SfxName = nullptr;
// Which resources have been set up, for cleanup if renaming is cut short.
bool g_PaperEntered = false;
bool g_ImgEntered = false;
bool g_NameEntryOpen = false;

// Max frames to wait on any one step, so a stuck animation can't soft-lock.
constexpr int32_t kStepTimeoutFrames = 300;
// Delay (300 ms) before the name entry sounds, matching Bub-ulb's script.
constexpr int32_t kSfxDelayFrames = 18;
// Frames to wait after the file select screen fades in before allowing
// renaming / showing the hint.
constexpr int32_t kFadedInDelayFrames = 20;

#define EVT_STR(str) reinterpret_cast<int32_t>(str)

// Same sequence of calls as the paper animation in Bub-ulb's name entry.
EvtStep g_OpenSteps[] = {
    { ttyd::evt_paper::evt_paper_entry,         { 0 }, 0 },
    { ttyd::evt_img::evt_img_entry,             { 0 }, 0 },
    { ttyd::evt_img::evt_img_set_paper,         { 0 }, 0 },
    { ttyd::evt_img::evt_img_set_paper_anim,    { 0 }, 0 },
    { ttyd::evt_img::evt_img_alloc_capture,     { 0 }, 0 },
    { ttyd::evt_img::evt_img_clear_virtual_point, { 0 }, 0 },
    { ttyd::evt_img::evt_img_onoff,             { 0 }, 0 },
    { nullptr,                                  { 0 }, 1 },
    { ttyd::evt_window::evt_win_nameent_on,     { 0 }, 0 },
    { ttyd::evt_img::evt_img_set_paper_anim,    { 0 }, 0 },
    { ttyd::evt_img::evt_img_wait_animend,      { 0 }, 0 },
};
EvtStep g_CloseSteps[] = {
    { ttyd::evt_img::evt_img_set_paper_anim,    { 0 }, 0 },
    { ttyd::evt_img::evt_img_wait_animend,      { 0 }, 0 },
    { ttyd::evt_window::evt_win_nameent_off,    { 0 }, 0 },
    { ttyd::evt_img::evt_img_release,           { 0 }, 0 },
    { ttyd::evt_paper::evt_paper_delete,        { 0 }, 0 },
};

void SetArgs(
    EvtStep& step, int32_t a0, int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0,
    int32_t a4 = 0, int32_t a5 = 0, int32_t a6 = 0, int32_t a7 = 0) {
    const int32_t args[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
    memcpy(step.args, args, sizeof(args));
}

void InitSteps() {
    const int32_t paper = EVT_STR("OFF_d_roll");
    const int32_t img = EVT_STR("img");
    SetArgs(g_OpenSteps[0], paper);
    SetArgs(g_OpenSteps[1], img);
    SetArgs(g_OpenSteps[2], img, paper);
    SetArgs(g_OpenSteps[3], img, EVT_STR("Z_1"));
    SetArgs(g_OpenSteps[4], img, 0, 0, 1, 0, 0, 608, 480);
    SetArgs(g_OpenSteps[5], img);
    SetArgs(g_OpenSteps[6], img, 1);
    SetArgs(g_OpenSteps[8], kNameEntryType);
    SetArgs(g_OpenSteps[9], img, EVT_STR("A_2"));
    SetArgs(g_OpenSteps[10], img);

    SetArgs(g_CloseSteps[0], img, EVT_STR("A_1"));
    SetArgs(g_CloseSteps[1], img);
    SetArgs(g_CloseSteps[3], img);
    SetArgs(g_CloseSteps[4], paper);
}

#undef EVT_STR

void StartSteps(const EvtStep* steps, int32_t num_steps) {
    g_Steps = steps;
    g_NumSteps = num_steps;
    g_StepIndex = 0;
    g_StepFrames = 0;
    g_StepFirstCall = true;
}

void StartSfx(const char* name) {
    g_SfxName = name;
    g_SfxTimer = kSfxDelayFrames;
}

// Tracks which resources a just-completed step set up / tore down.
void OnStepDone(EvtFunc func) {
    if (func == ttyd::evt_paper::evt_paper_entry) g_PaperEntered = true;
    if (func == ttyd::evt_paper::evt_paper_delete) g_PaperEntered = false;
    if (func == ttyd::evt_img::evt_img_entry) g_ImgEntered = true;
    if (func == ttyd::evt_img::evt_img_release) g_ImgEntered = false;
    if (func == ttyd::evt_window::evt_win_nameent_off) g_NameEntryOpen = false;
    if (func == ttyd::evt_window::evt_win_nameent_on) {
        g_NameEntryOpen = true;
        StartSfx("SFX_OFF4_NAME_ENTRY1");
    }
}

// Runs steps until one needs to wait; returns true once all are finished.
bool RunSteps() {
    while (g_StepIndex < g_NumSteps) {
        const EvtStep& step = g_Steps[g_StepIndex];
        bool done;
        if (step.func) {
            g_Evt.evtArguments = const_cast<int32_t*>(step.args);
            done = step.func(&g_Evt, g_StepFirstCall) == kEvtReturnDone;
            if (done) OnStepDone(step.func);
        } else {
            done = g_StepFrames >= step.wait_frames;
        }
        // Give up waiting on a step that never finishes.
        if (!done && g_StepFrames >= kStepTimeoutFrames) done = true;

        if (!done) {
            g_StepFirstCall = false;
            ++g_StepFrames;
            return false;
        }
        ++g_StepIndex;
        g_StepFrames = 0;
        g_StepFirstCall = true;
    }
    return true;
}

void Open(int32_t file) {
    memset(&g_Evt, 0, sizeof(g_Evt));
    InitSteps();
    StartSteps(g_OpenSteps, sizeof(g_OpenSteps) / sizeof(EvtStep));

    g_Active = true;
    g_Phase = Phase::OPENING;
    g_File = file;
    g_SfxTimer = -1;
}

void Finish() {
    g_Active = false;
    g_File = -1;
    g_WaitForRelease = true;
}

// Immediately cleans up anything still set up (e.g. on leaving the screen).
void Abort() {
    int32_t args[1];
    g_Evt.evtArguments = args;
    if (g_NameEntryOpen) {
        ttyd::evt_window::evt_win_nameent_off(&g_Evt, true);
        g_NameEntryOpen = false;
    }
    if (g_ImgEntered) {
        args[0] = reinterpret_cast<int32_t>("img");
        ttyd::evt_img::evt_img_release(&g_Evt, true);
        g_ImgEntered = false;
    }
    if (g_PaperEntered) {
        args[0] = reinterpret_cast<int32_t>("OFF_d_roll");
        ttyd::evt_paper::evt_paper_delete(&g_Evt, true);
        g_PaperEntered = false;
    }
    g_SfxTimer = -1;
    Finish();
}

// Returns whether input reads from the given caller should be suppressed.
bool ShouldBlockInput(void* caller) {
    if (!g_Active && !g_WaitForRelease) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(caller);
    return addr >= kSeqLoadTextStart && addr < kSeqLoadTextEnd;
}

// The hooks are branched to from the first instruction of the original
// functions, so the return address is still that of the original caller.
#define DEFINE_INPUT_HOOK(name)                                         \
    __attribute__((noinline)) uint32_t name##_hook(uint32_t pad) {      \
        if (ShouldBlockInput(__builtin_return_address(0))) return 0;    \
        return g_##name##_trampoline(pad);                              \
    }
DEFINE_INPUT_HOOK(keyGetButtonTrg)
DEFINE_INPUT_HOOK(keyGetButtonRep)
DEFINE_INPUT_HOOK(keyGetButton)
DEFINE_INPUT_HOOK(keyGetDirTrg)
DEFINE_INPUT_HOOK(keyGetDirRep)
DEFINE_INPUT_HOOK(keyGetDir)
DEFINE_INPUT_HOOK(keyGetStickX)
DEFINE_INPUT_HOOK(keyGetStickY)
#undef DEFINE_INPUT_HOOK

TotSaveSlot* GetSaveSlot(int32_t file) {
    uintptr_t file_ptr =
        reinterpret_cast<uintptr_t>(ttyd::cardmgr::cardGetFilePtr());
    if (!file_ptr) return nullptr;
    return reinterpret_cast<TotSaveSlot*>(
        file_ptr + file * sizeof(TotSaveSlot) + 0x2000);
}

// Returns the index of the existing save file hovered on the file select
// screen, or -1 if not on the file select screen / not hovering a valid file.
int32_t GetHoveredFile() {
    if (ttyd::seqdrv::seqGetSeq() != SeqIndex::kLoad) return -1;

    uintptr_t r13;
    __asm__ volatile("mr %0, 13" : "=r"(r13));
    uintptr_t work = *reinterpret_cast<uintptr_t*>(r13 + kSeqLoadWorkR13Offset);
    if (!work) return -1;

    int32_t file =
        *reinterpret_cast<int32_t*>(work + kSeqLoadWorkHoveredOffset);
    if (file < 0 || file >= kNumSaveFiles) return -1;

    const TotSaveSlot* slot = GetSaveSlot(file);
    if (!slot || !StateManager::IsSaveSlotValid(slot)) return -1;
    return file;
}

// Writes the new name to the file's slot in the memory card buffer.
void ApplyName(const char* name) {
    // Treat an empty / all-space name as cancelling.
    bool has_visible_char = false;
    for (const char* c = name; c && *c; ++c) {
        if (*c != ' ') has_visible_char = true;
    }
    if (!has_visible_char) return;

    TotSaveSlot* slot = GetSaveSlot(g_File);
    if (!slot || !StateManager::IsSaveSlotValid(slot)) return;

    char* file_name = slot->data.global_data.saveFileName;
    const int32_t max_length =
        static_cast<int32_t>(sizeof(slot->data.global_data.saveFileName)) - 1;
    memset(file_name, 0, max_length + 1);
    strncpy(file_name, name, max_length);
    StateManager::ComputeSaveChecksum(slot);
}

void UpdateRename() {
    if (g_SfxTimer >= 0 && g_SfxTimer-- == 0) {
        ttyd::pmario_sound::psndSFXOn(g_SfxName);
    }

    switch (g_Phase) {
        case Phase::OPENING: {
            if (RunSteps()) g_Phase = Phase::ENTERING;
            break;
        }
        case Phase::ENTERING: {
            int32_t args[1] = { 0 };
            g_Evt.evtArguments = args;
            if (ttyd::evt_window::evt_win_nameent_wait(&g_Evt, g_StepFirstCall)
                != kEvtReturnDone) {
                g_StepFirstCall = false;
                break;
            }
            // Name entry finished; fetch the entered name.
            args[0] = LW(0);
            ttyd::evt_window::evt_win_nameent_name(&g_Evt, true);
            ApplyName(reinterpret_cast<const char*>(g_Evt.lwData[0]));

            StartSfx("SFX_OFF4_NAME_ENTRY2");
            StartSteps(g_CloseSteps, sizeof(g_CloseSteps) / sizeof(EvtStep));
            g_Phase = Phase::CLOSING;
            break;
        }
        case Phase::CLOSING: {
            // Let the second sound effect play out before finishing.
            if (RunSteps() && g_SfxTimer < 0) Finish();
            break;
        }
    }
}

// Returns whether the file select screen is fully shown (faded in), so the
// option to rename files should be available.
bool IsFileSelectReady() {
    if (ttyd::fadedrv::fadeIsFinish()) {
        if (g_FadedInFrames < kFadedInDelayFrames) ++g_FadedInFrames;
    } else {
        g_FadedInFrames = 0;
    }
    return g_FadedInFrames >= kFadedInDelayFrames;
}

}  // namespace

void FileRenameManager::Init() {
    using namespace ::ttyd::system;

    g_keyGetButtonTrg_trampoline =
        mod::hookFunction(keyGetButtonTrg, keyGetButtonTrg_hook);
    g_keyGetButtonRep_trampoline =
        mod::hookFunction(keyGetButtonRep, keyGetButtonRep_hook);
    g_keyGetButton_trampoline =
        mod::hookFunction(keyGetButton, keyGetButton_hook);
    g_keyGetDirTrg_trampoline =
        mod::hookFunction(keyGetDirTrg, keyGetDirTrg_hook);
    g_keyGetDirRep_trampoline =
        mod::hookFunction(keyGetDirRep, keyGetDirRep_hook);
    g_keyGetDir_trampoline =
        mod::hookFunction(keyGetDir, keyGetDir_hook);
    g_keyGetStickX_trampoline =
        mod::hookFunction(keyGetStickX, keyGetStickX_hook);
    g_keyGetStickY_trampoline =
        mod::hookFunction(keyGetStickY, keyGetStickY_hook);
}

void FileRenameManager::Update() {
    // After closing the window, keep blocking the file select menu's input
    // until the buttons that might have closed it are released.
    if (g_WaitForRelease) {
        const uint32_t held = g_keyGetButton_trampoline(0);
        if (!(held & (ButtonId::A | ButtonId::B | ButtonId::START))) {
            g_WaitForRelease = false;
        }
    }

    if (ttyd::seqdrv::seqGetSeq() != SeqIndex::kLoad) {
        // Left the file select screen; discard any rename in progress.
        if (g_Active) Abort();
        g_WaitForRelease = false;
        g_HoveredFile = -1;
        g_FadedInFrames = 0;
        return;
    }

    if (g_Active) {
        UpdateRename();
        return;
    }

    const bool ready = IsFileSelectReady();
    g_HoveredFile = (g_WaitForRelease || !ready) ? -1 : GetHoveredFile();
    if (g_HoveredFile >= 0 && !ttyd::cardmgr::cardIsExec() &&
        (g_keyGetButtonTrg_trampoline(0) & ButtonId::Y)) {
        Open(g_HoveredFile);
    }
}

void FileRenameManager::Draw() {
    if (ttyd::seqdrv::seqGetSeq() != SeqIndex::kLoad) return;

    if (!g_Active && g_HoveredFile >= 0) {
        DrawText(
            "Y: Rename File", 0.0f, -212.0f, 0xFF, true, 0xFFFFFFFFU, 0.6f, 4);
    }
}

}
