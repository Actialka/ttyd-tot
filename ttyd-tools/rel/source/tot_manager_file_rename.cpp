#include "tot_manager_file_rename.h"

#include "common_types.h"
#include "common_ui.h"
#include "evt_cmd.h"
#include "patch.h"
#include "tot_state.h"

#include <ttyd/cardmgr.h>
#include <ttyd/evt_window.h>
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

// Whether the name entry window is currently open for renaming a file.
bool g_Active = false;
// Set after closing the window; keeps the file select menu's input blocked
// until the buttons that might have closed it are released.
bool g_WaitForRelease = false;
// The existing save file currently hovered on the file select screen, or -1.
int32_t g_HoveredFile = -1;
// The file being renamed.
int32_t g_File = -1;
// Whether evt_win_nameent_wait has been called yet for this rename.
bool g_WaitStarted = false;

// Stand-in evt context for calling the name entry window's evt functions
// directly, since evts aren't otherwise run on the file select screen.
EvtEntry g_Evt;
int32_t g_EvtArgs[1];

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

void Open(int32_t file) {
    memset(&g_Evt, 0, sizeof(g_Evt));
    g_Evt.evtArguments = g_EvtArgs;
    g_EvtArgs[0] = kNameEntryType;
    ttyd::evt_window::evt_win_nameent_on(&g_Evt, true);

    g_Active = true;
    g_File = file;
    g_WaitStarted = false;
}

void Close() {
    ttyd::evt_window::evt_win_nameent_off(&g_Evt, true);
    g_Active = false;
    g_File = -1;
    g_WaitForRelease = true;
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

void UpdateNameEntry() {
    if (ttyd::evt_window::evt_win_nameent_wait(&g_Evt, !g_WaitStarted)
        != kEvtReturnDone) {
        g_WaitStarted = true;
        return;
    }

    // Name entry finished; fetch the entered name.
    g_EvtArgs[0] = LW(0);
    ttyd::evt_window::evt_win_nameent_name(&g_Evt, true);
    ApplyName(reinterpret_cast<const char*>(g_Evt.lwData[0]));
    Close();
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
        if (g_Active) Close();
        g_WaitForRelease = false;
        g_HoveredFile = -1;
        return;
    }

    if (g_Active) {
        UpdateNameEntry();
        return;
    }

    g_HoveredFile = g_WaitForRelease ? -1 : GetHoveredFile();
    if (g_HoveredFile >= 0 && !ttyd::cardmgr::cardIsExec() &&
        (g_keyGetButtonTrg_trampoline(0) & ButtonId::Y)) {
        ttyd::pmario_sound::psndSFXOn("SFX_BTL_CURSOR_MOVE2");
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
