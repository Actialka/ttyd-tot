#pragma once

#include <cstdint>

namespace ttyd::effdrv {

struct EffEntry;

}

namespace ttyd::eff_sheep {

// Per-sheep work for the Sleepy Sheep herd effect; entry 0 of the effect's
// work array holds the spawn position, entries 1 to (eff_count - 1) are sheep.
struct EffSheepWork {
    int32_t     bounces;        // 0 for the final (straggler) sheep
    float       x;              // Starts at -500, done once >= 500
    float       y;
    float       z;
    float       scale;
    int32_t     unk_14;
    float       unk_18;
    float       unk_1c;
    int32_t     pose_id;
    int32_t     state;
    int32_t     delay;          // Frames to wait before starting to run
    float       x_speed;        // Units moved per frame
    int32_t     sfx_id;
} ;

static_assert(sizeof(EffSheepWork) == 0x34);

extern "C" {

// effSheepMain
effdrv::EffEntry* effSheepEntry(int32_t type, float x, float y, float z);

}

}
