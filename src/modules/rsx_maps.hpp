#pragma once

#include "profiles.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rsx
{
using IoMap = profiles::IoMap;

std::vector<IoMap> parse_io_maps(const std::filesystem::path& path);
std::optional<std::vector<IoMap>> profile_io_maps(const std::wstring& profile);
std::optional<std::uint32_t> resolve_texture_ea(
    std::uint8_t location,
    std::uint32_t offset,
    const std::vector<IoMap>& maps);
std::optional<std::size_t> io_map_for_offset(
    std::uint32_t offset,
    const std::vector<IoMap>& maps);
std::optional<std::size_t> io_map_for_ea(
    std::uint32_t ea,
    const std::vector<IoMap>& maps);
bool ea_in_io_arena(std::uint32_t ea, const std::vector<IoMap>& maps);
}
