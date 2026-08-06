#pragma once

#include "options.hpp"
#include "rsx_maps.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace diagnostics
{
int scan_gcm_tile_info(
    HANDLE process,
    std::uintptr_t vm_base,
    const app::Options& options);
int scan_gcm_fifo_state(
    HANDLE process,
    std::uintptr_t vm_base,
    const app::Options& options,
    const std::vector<rsx::IoMap>& maps);
int scan_rsx_texture_commands(
    HANDLE process,
    std::uintptr_t vm_base,
    const app::Options& options,
    const std::vector<rsx::IoMap>& maps);
}
