#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace app
{
struct Options
{
    std::wstring process = L"rpcs3.exe";
    std::wstring profile;
    std::filesystem::path log_path;
    std::filesystem::path out_dir = L"rpcs3_texture_dump";
    std::uint32_t guest_start = 0x00010000;
    std::uint32_t guest_end = 0x80000000;
    std::uintptr_t vm_base_override = 0;
    std::size_t max_textures = 2000;
    std::uint64_t dump_budget_bytes = 1024ull * 1024 * 1024;
    std::uint32_t tile_offset_filter = 0xffffffffu;
    std::uint32_t fifo_sample_ms = 250;
    std::uint32_t fifo_samples = 8;
    std::uint64_t fifo_history_bytes = 2ull * 1024 * 1024;
    std::uint32_t control_ea = 0;
    bool list_only = true;
    bool rsx_scan = false;
    bool tile_scan = false;
    bool fifo_scan = false;
    bool fifo_capture = false;
    bool fifo_follow_calls = false;
    bool renderer_ring = false;
    bool preview_variants = false;
    bool auto_tune = false;
};

void usage();
bool parse_args(int argc, wchar_t** argv, Options& options);
void apply_automatic_capture_tuning(Options& options);
}
