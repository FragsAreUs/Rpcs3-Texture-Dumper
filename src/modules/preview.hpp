#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace preview
{
// Writes the normal human-facing SOCOM 4 BC preview using the confirmed Y flip.
// When variants is true, neutral/X/XY diagnostic versions are written too.
bool write_bc_previews(const std::vector<std::uint8_t>& data,
                       std::uint8_t format,
                       std::uint16_t width,
                       std::uint16_t height,
                       const std::filesystem::path& raw_path,
                       bool variants);
}
