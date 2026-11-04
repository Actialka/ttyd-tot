#include "tot_manager_file_rename.h"

#include "common_types.h"
#include "common_ui.h"
#include "patch.h"
#include "tot_state.h"

#include <ttyd/cardmgr.h>
#include <ttyd/dispdrv.h>
#include <ttyd/pmario_sound.h>
#include <ttyd/seqdrv.h>
#include <ttyd/system.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace mod::tot {

namespace {

using ::ttyd::seqdrv::SeqIndex;

constexpr int32_t kNumSaveFiles = 4;
// Same as the maximum length allowed by the vanilla name entry screen.
constexpr int32_t kMaxNameLength = 8;
// Offset of the file select screen's work pointer from r13, and the offset of
// the currently hovered file / button index in that work.
constexpr int32_t kSeqLoadWorkR13Offset = -0x6550;
constexpr int32_t kSeqLoadWorkHoveredOffset = 0x20;

constexpr int32_t kNumCharRows = 5;
constexpr int32_t kNumCharCols = 13;
constexpr const char* kCharRows[kNumCharRows] = {
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXYZ",
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "0123456789.-!",
};
// The row after the character rows holds command buttons.
constexpr int32_t kCommandRow = kNumCharRows;
enum Command {
    CMD_SPACE = 0,
    CMD_DELETE,
    CMD_CANCEL,
    CMD_OK,
    CMD_MAX
};
constexpr const char* kCommandNames[CMD_MAX] = {
    "Space", "Delete", "Cancel", "OK",
};

// Layout constants for the on-screen keyboard.
constexpr float kCellWidth = 30.0f;
constexpr float kCellHeight = 30.0f;
constexpr float kGridTop = 50.0f;
constexpr float kCommandWidth = 96.0f;

constexpr uint32_t kWindowColor = 0x000000E5U;
constexpr uint32_t kHighlightColor = 0xC00000E5U;
constexpr uint32_t kTextColor = 0xFFFFFFFFU;
constexpr uint32_t kDimTextColor = 0xA0A0A0FFU;
constexpr uint32_t kErrorTextColor = 0xFF8080FFU;

// Trampolines for the game's input functions; while the rename keyboard is
// open, the hooked versions report no input so the vanilla file select menu
// doesn't react, and this manager reads input through these instead.
uint32_t (*g_keyGetButtonTrg_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetButtonRep_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetButton_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDirTrg_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDirRep_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetDir_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetStickX_trampoline)(uint32_t) = nullptr;
uint32_t (*g_keyGetStickY_trampoline)(uint32_t) = nullptr;

// Whether the rename keyboard is currently open.
bool g_Active = false;
// Set after closing the keyboard; keeps game input blocked until the buttons
// used to close it are released, so the vanilla menu doesn't react to them.
bool g_WaitForRelease = false;
// The existing save file currently hovered on the file select screen, or -1.
int32_t g_HoveredFile = -1;
// The file being renamed, and the name being entered.
int32_t g_File = -1;
char g_Name[kMaxNameLength + 1];
// Keyboard cursor position.
int32_t g_Row = 0;
int32_t g_Col = 0;
int32_t g_Cmd = CMD_OK;
// Message shown at the bottom of the keyboard (e.g. errors), and its timer.
const char* g_Message = nullptr;
int32_t g_MessageTimer = 0;

bool IsInputBlocked() {
    return g_Active || g_WaitForRelease;
}

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

// Gets buttons pressed / repeated this frame, with stick inputs converted to
// their respective D-Pad directions.
uint32_t GetPressedButtons() {
    uint32_t buttons = g_keyGetButtonTrg_trampoline(0);
    buttons |= g_keyGetButtonRep_trampoline(0) & (
        ButtonId::DPAD_UP | ButtonId::DPAD_DOWN |
        ButtonId::DPAD_LEFT | ButtonId::DPAD_RIGHT);

    const uint32_t dir = g_keyGetDirRep_trampoline(0);
    if (dir & (DirectionInputId::ANALOG_UP | DirectionInputId::CSTICK_UP))
        buttons |= ButtonId::DPAD_UP;
    if (dir & (DirectionInputId::ANALOG_DOWN | DirectionInputId::CSTICK_DOWN))
        buttons |= ButtonId::DPAD_DOWN;
    if (dir & (DirectionInputId::ANALOG_LEFT | DirectionInputId::CSTICK_LEFT))
        buttons |= ButtonId::DPAD_LEFT;
    if (dir & (DirectionInputId::ANALOG_RIGHT | DirectionInputId::CSTICK_RIGHT))
        buttons |= ButtonId::DPAD_RIGHT;
    return buttons;
}

void ShowMessage(const char* message) {
    g_Message = message;
    g_MessageTimer = 120;
}

void Open(int32_t file) {
    const TotSaveSlot* slot = GetSaveSlot(file);
    if (!slot) return;

    g_Active = true;
    g_File = file;
    // Start from the file's current name.
    memset(g_Name, 0, sizeof(g_Name));
    strncpy(g_Name, slot->data.global_data.saveFileName, kMaxNameLength);
    g_Row = 0;
    g_Col = 0;
    g_Cmd = CMD_OK;
    g_Message = nullptr;
    g_MessageTimer = 0;
    ttyd::pmario_sound::psndSFXOn("SFX_BTL_CURSOR_MOVE2");
}

void Close() {
    g_Active = false;
    g_File = -1;
    g_WaitForRelease = true;
}

void AppendChar(char c) {
    int32_t length = strlen(g_Name);
    if (length >= kMaxNameLength) {
        ShowMessage("The name can't be any longer.");
        return;
    }
    g_Name[length] = c;
    g_Name[length + 1] = '\0';
}

void DeleteChar() {
    int32_t length = strlen(g_Name);
    if (length > 0) g_Name[length - 1] = '\0';
}

void Confirm() {
    // Require at least one non-space character.
    bool has_visible_char = false;
    for (const char* c = g_Name; *c; ++c) {
        if (*c != ' ') has_visible_char = true;
    }
    if (!has_visible_char) {
        ShowMessage("Please enter a name.");
        return;
    }

    TotSaveSlot* slot = GetSaveSlot(g_File);
    if (!slot || !StateManager::IsSaveSlotValid(slot)) {
        // The file was removed out from under the keyboard somehow.
        Close();
        return;
    }
    if (ttyd::cardmgr::cardIsExec()) {
        ShowMessage("The Memory Card is busy; try again.");
        return;
    }

    char* file_name = slot->data.global_data.saveFileName;
    memset(file_name, 0, sizeof(slot->data.global_data.saveFileName));
    strcpy(file_name, g_Name);
    StateManager::ComputeSaveChecksum(slot);

    ttyd::pmario_sound::psndSFXOn("SFX_SYSTEM_LUCKY1");
    Close();
}

void UpdateKeyboard() {
    const uint32_t buttons = GetPressedButtons();

    if (g_MessageTimer > 0 && --g_MessageTimer == 0) g_Message = nullptr;

    // Cursor movement.
    bool moved = false;
    if (buttons & ButtonId::DPAD_UP) {
        g_Row = g_Row == 0 ? kCommandRow : g_Row - 1;
        moved = true;
    } else if (buttons & ButtonId::DPAD_DOWN) {
        g_Row = g_Row == kCommandRow ? 0 : g_Row + 1;
        moved = true;
    } else if (buttons & ButtonId::DPAD_LEFT) {
        if (g_Row == kCommandRow) {
            g_Cmd = g_Cmd == 0 ? CMD_MAX - 1 : g_Cmd - 1;
        } else {
            g_Col = g_Col == 0 ? kNumCharCols - 1 : g_Col - 1;
        }
        moved = true;
    } else if (buttons & ButtonId::DPAD_RIGHT) {
        if (g_Row == kCommandRow) {
            g_Cmd = g_Cmd == CMD_MAX - 1 ? 0 : g_Cmd + 1;
        } else {
            g_Col = g_Col == kNumCharCols - 1 ? 0 : g_Col + 1;
        }
        moved = true;
    }
    if (moved) {
        ttyd::pmario_sound::psndSFXOn("SFX_BTL_CURSOR_MOVE2");
        return;
    }

    if (buttons & ButtonId::START) {
        Confirm();
    } else if (buttons & ButtonId::A) {
        if (g_Row < kCommandRow) {
            AppendChar(kCharRows[g_Row][g_Col]);
        } else {
            switch (g_Cmd) {
                case CMD_SPACE:     AppendChar(' ');    break;
                case CMD_DELETE:    DeleteChar();       break;
                case CMD_CANCEL:    Close();            break;
                case CMD_OK:        Confirm();          break;
            }
        }
    } else if (buttons & ButtonId::B) {
        // Backspace, or cancel if the name is already empty.
        if (g_Name[0]) {
            DeleteChar();
        } else {
            Close();
        }
    }
}

void DrawKeyboard() {
    char buf[64];

    // Background window.
    const float window_width = kNumCharCols * kCellWidth + 40.0f;
    const float window_top = 150.0f;
    const float window_height = 330.0f;
    DrawWindow(
        kWindowColor, -window_width / 2, window_top,
        window_width, window_height, 10.0f);

    // Title + current name.
    sprintf(buf, "Rename File %" PRId32, g_File + 1);
    DrawText(buf, 0.0f, window_top - 22.0f, 0xFF, true, kTextColor, 0.75f, 4);

    char name_buf[kMaxNameLength + 2];
    strcpy(name_buf, g_Name);
    if (static_cast<int32_t>(strlen(g_Name)) < kMaxNameLength) {
        strcat(name_buf, "_");
    }
    DrawText(name_buf, 0.0f, window_top - 60.0f, 0xFF, true, kTextColor, 1.0f, 4);

    // Character grid.
    const float grid_left = -(kNumCharCols - 1) * kCellWidth / 2;
    for (int32_t row = 0; row < kNumCharRows; ++row) {
        for (int32_t col = 0; col < kNumCharCols; ++col) {
            const float x = grid_left + col * kCellWidth;
            const float y = kGridTop - row * kCellHeight;
            if (row == g_Row && col == g_Col) {
                DrawWindow(
                    kHighlightColor,
                    x - kCellWidth / 2 + 2.0f, y + kCellHeight / 2 - 2.0f,
                    kCellWidth - 4.0f, kCellHeight - 4.0f, 5.0f);
            }
            buf[0] = kCharRows[row][col];
            buf[1] = '\0';
            DrawText(buf, x, y, 0xFF, true, kTextColor, 0.8f, 4);
        }
    }

    // Command buttons.
    const float cmd_y = kGridTop - kCommandRow * kCellHeight - 8.0f;
    const float cmd_left = -(CMD_MAX - 1) * kCommandWidth / 2;
    for (int32_t cmd = 0; cmd < CMD_MAX; ++cmd) {
        const float x = cmd_left + cmd * kCommandWidth;
        if (g_Row == kCommandRow && cmd == g_Cmd) {
            DrawWindow(
                kHighlightColor,
                x - kCommandWidth / 2 + 4.0f, cmd_y + kCellHeight / 2 - 2.0f,
                kCommandWidth - 8.0f, kCellHeight - 4.0f, 5.0f);
        }
        DrawText(kCommandNames[cmd], x, cmd_y, 0xFF, true, kTextColor, 0.7f, 4);
    }

    // Controls help, or error message.
    const float help_y = window_top - window_height + 22.0f;
    if (g_Message) {
        DrawText(g_Message, 0.0f, help_y, 0xFF, true, kErrorTextColor, 0.6f, 4);
    } else {
        DrawText(
            "A: Enter   B: Delete   START: Confirm",
            0.0f, help_y, 0xFF, true, kDimTextColor, 0.6f, 4);
    }
}

}  // namespace

void FileRenameManager::Init() {
    using namespace ::ttyd::system;

    g_keyGetButtonTrg_trampoline = mod::hookFunction(
        keyGetButtonTrg, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetButtonTrg_trampoline(pad);
        });
    g_keyGetButtonRep_trampoline = mod::hookFunction(
        keyGetButtonRep, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetButtonRep_trampoline(pad);
        });
    g_keyGetButton_trampoline = mod::hookFunction(
        keyGetButton, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetButton_trampoline(pad);
        });
    g_keyGetDirTrg_trampoline = mod::hookFunction(
        keyGetDirTrg, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetDirTrg_trampoline(pad);
        });
    g_keyGetDirRep_trampoline = mod::hookFunction(
        keyGetDirRep, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetDirRep_trampoline(pad);
        });
    g_keyGetDir_trampoline = mod::hookFunction(
        keyGetDir, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetDir_trampoline(pad);
        });
    g_keyGetStickX_trampoline = mod::hookFunction(
        keyGetStickX, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetStickX_trampoline(pad);
        });
    g_keyGetStickY_trampoline = mod::hookFunction(
        keyGetStickY, [](uint32_t pad) -> uint32_t {
            return IsInputBlocked() ? 0 : g_keyGetStickY_trampoline(pad);
        });
}

void FileRenameManager::Update() {
    // After closing the keyboard, keep blocking game input until the buttons
    // that might have closed it are released.
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
        UpdateKeyboard();
        return;
    }

    g_HoveredFile = g_WaitForRelease ? -1 : GetHoveredFile();
    if (g_HoveredFile >= 0 && !ttyd::cardmgr::cardIsExec() &&
        (g_keyGetButtonTrg_trampoline(0) & ButtonId::Y)) {
        Open(g_HoveredFile);
    }
}

void FileRenameManager::Draw() {
    if (ttyd::seqdrv::seqGetSeq() != SeqIndex::kLoad) return;

    if (g_Active) {
        DrawKeyboard();
    } else if (g_HoveredFile >= 0) {
        DrawText(
            "Y: Rename File", 0.0f, -212.0f, 0xFF, true, kTextColor, 0.6f, 4);
    }
}

}
