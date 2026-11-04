#pragma once

#include <cstdint>

namespace mod::tot {

// Handles renaming existing save files from the file select screen.
//
// While hovering an existing file on the file select screen, pressing Y opens
// an on-screen keyboard to choose a new name for that file. The new name is
// written to that file's slot in the memory card buffer, so it's displayed on
// the file select screen right away and used when the file is loaded.
//
// Special file names (e.g. "xyzzy" for debug mode) take effect when a renamed
// file is loaded, the same as when a new file is created with that name.
class FileRenameManager {
public:
    // Installs hooks; should be called once at mod initialization.
    static void Init();
    // Code that runs every frame.
    static void Update();
    // Code that runs drawing-related code every frame.
    static void Draw();
    // Called when the file select screen draws an existing file's info;
    // renaming is only offered for files that are actually being shown.
    static void OnFileInfoDrawn(int32_t file);
};

}
