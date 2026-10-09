#pragma once

// The table of a game's own PRX modules that were translated ahead of time
// (generated/<game>/modules_table.cpp, written by scripts/modules.py; games
// without such modules get the empty table in no_modules.cpp).

#include <cstdint>
#include <span>

namespace psprecomp {
class Runtime;
}

namespace pspweb {

struct GeneratedModule {
    const char *path;     // on the disc, e.g. "PSP_GAME/USRDIR/BIN/LEVEL.PRX"
    std::uint32_t base;   // load address the code was generated for
    void (*register_functions)(psprecomp::Runtime &);
};

std::span<const GeneratedModule> generated_modules();

} // namespace pspweb
