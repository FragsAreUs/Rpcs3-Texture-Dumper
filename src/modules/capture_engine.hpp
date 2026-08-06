#pragma once

#include "options.hpp"
#include "rsx_maps.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace capture_engine
{
int run(
    HANDLE process,
    std::uintptr_t vm_base,
    app::Options options,
    const std::vector<rsx::IoMap>& maps);
}
