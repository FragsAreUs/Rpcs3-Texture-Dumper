#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace profiles
{
struct IoMap
{
    std::uint32_t io = 0;
    std::uint32_t ea = 0;
    std::uint32_t size = 0;
};

struct CommandBufferRange
{
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

enum class DeepCaptureKind
{
    none,
    socom_secondary_calls,
    renderer_ring,
    command_ring,
};

struct GameProfile
{
    std::wstring_view id;
    std::wstring_view game_name;
    std::wstring_view version;
    std::wstring_view display_name;
    std::array<std::wstring_view, 3> aliases{};
    std::uint32_t control_ea = 0;
    std::array<IoMap, 3> io_maps{};
    std::size_t io_map_count = 0;
    DeepCaptureKind deep_capture = DeepCaptureKind::none;
    std::array<CommandBufferRange, 6> deep_buffers{};
    std::size_t deep_buffer_count = 0;
    bool preview_flip_y = true;
};

std::span<const GameProfile> all();
const GameProfile& default_profile();
const GameProfile* find(std::wstring_view name);
bool default_preview_flip_y(std::wstring_view name);
std::wstring dump_folder_name(const GameProfile& profile);
}
