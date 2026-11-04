#pragma once

#include <cstdint>

namespace gc::dvd {

extern "C" {

// Returns the disc file-system entry number for a path, or -1 if not found.
int32_t DVDConvertPathToEntrynum(const char* path);

}

}
