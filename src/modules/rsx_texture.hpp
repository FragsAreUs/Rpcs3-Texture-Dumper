#pragma once

#include "rsx_maps.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rsx_texture
{
struct TextureDesc
{
    std::uint32_t descriptor_ea = 0;
    std::uint8_t format = 0;
    std::uint8_t mipmap = 0;
    std::uint8_t dimension = 0;
    std::uint8_t cubemap = 0;
    std::uint32_t remap = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t depth = 0;
    std::uint8_t location = 0;
    std::uint8_t padding = 0;
    std::uint32_t pitch = 0;
    std::uint32_t offset = 0;
    std::uint32_t data_ea = 0;
    std::uint64_t estimated_size = 0;
};

struct RsxTexture
{
    std::uint32_t command_ea = 0;
    std::uint32_t io_offset = 0;
    std::uint32_t header = 0;
    std::uint32_t data_ea = 0;
    std::uint32_t offset = 0;
    std::uint32_t format_reg = 0;
    std::uint32_t address = 0;
    std::uint32_t control0 = 0;
    std::uint32_t control1 = 0;
    std::uint32_t filter = 0;
    std::uint32_t image_rect = 0;
    std::uint32_t border_color = 0;
    std::uint8_t unit = 0;
    std::uint8_t location = 0;
    std::uint8_t format = 0;
    std::uint8_t dimension = 0;
    std::uint8_t cubemap = 0;
    std::uint16_t mipmap = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t pitch = 0;
    std::uint64_t estimated_size = 0;
};

std::uint16_t read_be16(const std::uint8_t* data);
std::uint32_t read_be32(const std::uint8_t* data);
std::uint64_t estimate_size(const TextureDesc& texture);
std::uint32_t find_control3_pitch_for_texture(
    const std::uint8_t* data,
    std::size_t data_size,
    std::size_t position,
    std::uint8_t unit,
    std::uint8_t format,
    std::uint16_t width);
std::optional<TextureDesc> parse_candidate(
    const std::uint8_t* data,
    std::uint32_t descriptor_ea,
    const std::vector<rsx::IoMap>& maps);
std::string hex8(std::uint32_t value);
std::string hex2(std::uint8_t value);
bool dump_payload(
    HANDLE process,
    std::uintptr_t vm_base,
    const TextureDesc& texture,
    const std::filesystem::path& output_directory,
    std::size_t index,
    bool preview_variants);
std::optional<RsxTexture> parse_rsx_texture_block(
    const std::uint8_t* data,
    std::size_t available,
    std::uint32_t command_ea,
    std::uint32_t io_offset,
    const std::vector<rsx::IoMap>& maps);
std::vector<RsxTexture> parse_rsx_texture_state_stream(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t command_ea,
    std::uint32_t io_offset,
    const std::vector<rsx::IoMap>& maps,
    std::size_t segment_size);
bool dump_rsx_payload(
    HANDLE process,
    std::uintptr_t vm_base,
    const RsxTexture& texture,
    const std::filesystem::path& output_directory,
    std::size_t index,
    bool preview_variants);
}
