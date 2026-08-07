#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace preview
{
// Writes the normal human-facing BC preview in the profile-selected orientation.
// When variants is true, the other three orientation diagnostics are written too.
bool write_bc_previews(const std::vector<std::uint8_t>& data,
                       std::uint8_t format,
                       std::uint16_t width,
                       std::uint16_t height,
                       std::uint32_t pitch,
                       const std::filesystem::path& raw_path,
                       bool variants,
                       bool default_flip_y);
}
