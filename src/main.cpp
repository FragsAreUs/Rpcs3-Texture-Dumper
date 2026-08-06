#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "cli.hpp"
#include "modules/profiles.hpp"
#include "modules/preview.hpp"

namespace fs = std::filesystem;

struct UniqueHandle
{
    HANDLE value = nullptr;
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : value(h) {}
    ~UniqueHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct ModuleInfo
{
    std::uintptr_t base = 0;
    std::uint32_t size = 0;
    fs::path path;
};

using IoMap = profiles::IoMap;

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

struct Options
{
    std::wstring process = L"rpcs3.exe";
    std::wstring profile;
    fs::path log_path;
    fs::path out_dir = L"rpcs3_texture_dump";
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

static bool is_socom4_profile(const std::wstring& profile)
{
    const auto* p = profiles::find(profile);
    return p && p->deep_capture == profiles::DeepCaptureKind::socom_secondary_calls;
}

static bool is_renderer_ring_profile(const std::wstring& profile)
{
    const auto* p = profiles::find(profile);
    return p && p->deep_capture == profiles::DeepCaptureKind::renderer_ring;
}

static void apply_automatic_capture_tuning(Options& o)
{
    if (!o.auto_tune) return;

    // The GUI deliberately exposes no numeric capture knobs. These values are
    // bounded by the same limits as the diagnostic CLI and favor reliability:
    // capture exits as soon as it obtains a useful FIFO sample, so 100 samples
    // is a retry ceiling rather than a mandatory 10-second wait.
    o.dump_budget_bytes = 1024ull * 1024 * 1024;
    o.max_textures = 4096;
    o.fifo_sample_ms = 100;
    o.fifo_samples = 100;

    // SOCOM 4's primary FIFO calls the confirmed secondary command buffers.
    // Keeping a wider recent window makes it much less timing-sensitive than
    // asking the user to guess an exact history size for each scene.
    o.fifo_history_bytes = (is_socom4_profile(o.profile) ? 8ull : 4ull) * 1024 * 1024;
}

struct GcmTileCandidate
{
    std::uint32_t entry_ea = 0;
    std::uint32_t tile = 0;
    std::uint32_t limit = 0;
    std::uint32_t pitch = 0;
    std::uint32_t format = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint8_t location = 0;
    std::uint8_t bank = 0;
};

struct GcmControlCandidate
{
    std::uint32_t entry_ea = 0;
    std::uint32_t put = 0;
    std::uint32_t get = 0;
    std::uint32_t ref = 0;
    std::uint32_t put_after = 0;
    std::uint32_t get_after = 0;
    std::uint32_t ref_after = 0;
    std::size_t map_index = 0;
    bool put_changed = false;
    bool get_changed = false;
    bool ref_changed = false;
};

struct GcmContextCandidate
{
    std::uint32_t entry_ea = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t current = 0;
    std::uint32_t callback = 0;
    std::uint32_t current_after = 0;
    std::size_t map_index = 0;
    bool current_changed = false;
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

static std::uint16_t read_be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

static std::uint32_t read_be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

static std::optional<std::uint64_t> parse_u64(std::wstring_view text)
{
    std::wstring s(text);
    int base = 10;
    if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
    {
        s.erase(0, 2);
        base = 16;
    }
    try
    {
        std::size_t used = 0;
        const auto v = std::stoull(s, &used, base);
        if (used != s.size()) return std::nullopt;
        return v;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

static bool readable_protection(DWORD protect)
{
    if (protect & PAGE_GUARD) return false;
    const DWORD p = protect & 0xff;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static std::optional<DWORD> find_process_id(const std::wstring& name)
{
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return std::nullopt;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snapshot.value, &pe)) return std::nullopt;
    do
    {
        if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) return pe.th32ProcessID;
    } while (Process32NextW(snapshot.value, &pe));
    return std::nullopt;
}

static std::optional<ModuleInfo> get_main_module(DWORD pid)
{
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) return std::nullopt;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (!Module32FirstW(snapshot.value, &me)) return std::nullopt;
    return ModuleInfo{reinterpret_cast<std::uintptr_t>(me.modBaseAddr), me.modBaseSize, me.szExePath};
}

static bool read_remote(HANDLE process, std::uintptr_t address, void* out, std::size_t size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &got) && got == size;
}

static std::optional<std::uintptr_t> scan_pattern(HANDLE process, const ModuleInfo& mod, const std::vector<int>& pattern)
{
    constexpr std::size_t chunk_size = 4 * 1024 * 1024;
    if (pattern.empty()) return std::nullopt;
    std::vector<std::uint8_t> buffer(chunk_size + pattern.size());

    std::size_t pos = 0;
    while (pos < mod.size)
    {
        const std::size_t want = std::min<std::size_t>(chunk_size, mod.size - pos);
        SIZE_T got = 0;
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(mod.base + pos), buffer.data(), want, &got) && got >= pattern.size())
        {
            for (std::size_t i = 0; i + pattern.size() <= got; ++i)
            {
                bool match = true;
                for (std::size_t j = 0; j < pattern.size(); ++j)
                {
                    if (pattern[j] >= 0 && buffer[i + j] != static_cast<std::uint8_t>(pattern[j]))
                    {
                        match = false;
                        break;
                    }
                }
                if (match) return mod.base + pos + i;
            }
        }
        if (want < chunk_size) break;
        pos += chunk_size - (pattern.size() - 1);
    }
    return std::nullopt;
}

static std::optional<std::uintptr_t> resolve_rip_pointer(HANDLE process, std::uintptr_t instruction, std::size_t disp_offset, std::size_t instruction_size)
{
    std::int32_t rel = 0;
    if (!read_remote(process, instruction + disp_offset, &rel, sizeof(rel))) return std::nullopt;
    const std::uintptr_t slot = instruction + instruction_size + rel;
    std::uintptr_t value = 0;
    if (!read_remote(process, slot, &value, sizeof(value))) return std::nullopt;
    return value;
}

static std::optional<std::uintptr_t> find_vm_base_signature(HANDLE process, const ModuleInfo& mod)
{
    // Supplied RPCS3 RE guide: sub rdx, [rip+vm::g_base_addr] followed by mov rax, imm64.
    const std::vector<int> pattern{0x48, 0x2B, 0x15, -1, -1, -1, -1, 0x48, 0xB8};
    const auto hit = scan_pattern(process, mod, pattern);
    if (!hit) return std::nullopt;
    return resolve_rip_pointer(process, *hit, 3, 7);
}

static bool verify_guest_elf(HANDLE process, std::uintptr_t vm_base)
{
    // PS3 PPU executables are ELF64, big-endian, PowerPC64 (EM_PPC64 = 0x15).
    // Checking these fields avoids mistaking an unrelated embedded ELF for the guest image.
    std::array<std::uint8_t, 20> header{};
    if (!read_remote(process, vm_base + 0x10000, header.data(), header.size())) return false;
    return header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
           header[4] == 2 && header[5] == 2 && header[6] == 1 &&
           header[18] == 0x00 && header[19] == 0x15;
}

static std::optional<std::uintptr_t> find_vm_base_from_guest_elf(HANDLE process)
{
    constexpr std::uintptr_t guest_elf_ea = 0x10000;
    constexpr std::uintptr_t vm_stride = 0x100000000ull; // RPCS3 reserves the VM on 4 GiB boundaries.

    // This is the usual RPCS3 placement documented by the supplied RE guide. Probe it first.
    constexpr std::uintptr_t common_vm_base = 0x300000000ull;
    if (verify_guest_elf(process, common_vm_base)) return common_vm_base;

    // Do not scan gigabytes of process bytes. Walk Windows memory regions and only probe
    // addresses whose low 32 bits are guest EA 0x10000. A valid hit gives VM = hit - 0x10000.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const auto max_address = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    std::uintptr_t cursor = 0;

    while (cursor < max_address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;

        const auto region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize == 0 || region_base >= max_address) break;
        if (mbi.RegionSize > max_address - region_base) break;
        const std::uintptr_t region_end = region_base + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && readable_protection(mbi.Protect))
        {
            std::uintptr_t block = region_base & ~(vm_stride - 1);
            std::uintptr_t probe = block + guest_elf_ea;
            if (probe < region_base)
            {
                if (probe > max_address - vm_stride) probe = max_address;
                else probe += vm_stride;
            }

            while (probe < region_end && probe < max_address)
            {
                const std::uintptr_t candidate = probe - guest_elf_ea;
                if (verify_guest_elf(process, candidate)) return candidate;
                if (probe > max_address - vm_stride) break;
                probe += vm_stride;
            }
        }

        if (region_end <= cursor) break;
        cursor = region_end;
    }

    return std::nullopt;
}

static std::vector<IoMap> parse_io_maps(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) return {};

    // Keep only the newest mapping for a given IO base. RPCS3.log can contain boot/VSH mappings before game mappings.
    std::map<std::uint32_t, IoMap> latest;
    const std::regex re(R"(sys_rsx_context_iomap\([^\n]*?io=0x([0-9a-fA-F]+),\s*ea=0x([0-9a-fA-F]+),\s*size=0x([0-9a-fA-F]+))");
    std::string line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (!std::regex_search(line, m, re)) continue;
        IoMap x{};
        x.io = static_cast<std::uint32_t>(std::stoull(m[1].str(), nullptr, 16));
        x.ea = static_cast<std::uint32_t>(std::stoull(m[2].str(), nullptr, 16));
        x.size = static_cast<std::uint32_t>(std::stoull(m[3].str(), nullptr, 16));
        latest[x.io] = x;
    }

    std::vector<IoMap> result;
    for (const auto& [_, x] : latest) result.push_back(x);
    return result;
}

static std::optional<std::vector<IoMap>> profile_io_maps(const std::wstring& profile)
{
    const auto* p = profiles::find(profile);
    if (!p) return std::nullopt;
    return std::vector<IoMap>(p->io_maps.begin(), p->io_maps.begin() + p->io_map_count);
}

static std::optional<std::uint32_t> resolve_texture_ea(std::uint8_t location, std::uint32_t offset, const std::vector<IoMap>& maps)
{
    if (location == 0) // CELL_GCM_LOCATION_LOCAL
    {
        if (offset >= 0x10000000) return std::nullopt;
        return 0xC0000000u + offset;
    }
    if (location == 1) // CELL_GCM_LOCATION_MAIN
    {
        for (const auto& m : maps)
        {
            const std::uint64_t begin = m.io;
            const std::uint64_t end = begin + m.size;
            if (offset >= begin && offset < end)
                return m.ea + (offset - m.io);
        }
    }
    return std::nullopt;
}

static std::optional<std::size_t> io_map_for_offset(std::uint32_t offset, const std::vector<IoMap>& maps)
{
    for (std::size_t i = 0; i < maps.size(); ++i)
    {
        const std::uint64_t begin = maps[i].io;
        const std::uint64_t end = begin + maps[i].size;
        if (offset >= begin && offset < end) return i;
    }
    return std::nullopt;
}

static std::optional<std::size_t> io_map_for_ea(std::uint32_t ea, const std::vector<IoMap>& maps)
{
    for (std::size_t i = 0; i < maps.size(); ++i)
    {
        const std::uint64_t begin = maps[i].ea;
        const std::uint64_t end = begin + maps[i].size;
        if (ea >= begin && ea < end) return i;
    }
    return std::nullopt;
}

static bool ea_in_io_arena(std::uint32_t ea, const std::vector<IoMap>& maps)
{
    return io_map_for_ea(ea, maps).has_value();
}

static bool known_base_format(std::uint8_t format)
{
    // CELL_GCM_TEXTURE_LN (0x20) and UN (0x40) are modifier bits.
    const std::uint8_t base = static_cast<std::uint8_t>(format & ~0x60u);
    switch (base)
    {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88:
    case 0x8b: case 0x8d: case 0x8e: case 0x8f: case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: case 0x9b:
    case 0x9c: case 0x9d: case 0x9e:
        return true;
    default:
        return false;
    }
}

static std::uint64_t estimate_size(const TextureDesc& t)
{
    const std::uint8_t base = static_cast<std::uint8_t>(t.format & ~0x60u);
    const std::uint64_t w = t.width;
    const std::uint64_t h = t.height;
    const std::uint64_t d = std::max<std::uint16_t>(t.depth, 1);

    if (base == 0x86) // DXT1
    {
        if ((t.format & 0x20u) && t.pitch && t.pitch <= 0x100000)
            return static_cast<std::uint64_t>(t.pitch) * ((h + 3) / 4) * d;
        return ((w + 3) / 4) * ((h + 3) / 4) * 8 * d;
    }
    if (base == 0x87 || base == 0x88) // DXT23/45
    {
        if ((t.format & 0x20u) && t.pitch && t.pitch <= 0x100000)
            return static_cast<std::uint64_t>(t.pitch) * ((h + 3) / 4) * d;
        return ((w + 3) / 4) * ((h + 3) / 4) * 16 * d;
    }

    if ((t.format & 0x20u) && t.pitch && t.pitch <= 0x100000)
        return static_cast<std::uint64_t>(t.pitch) * h * d;

    std::uint32_t bpp = 0;
    switch (base)
    {
    case 0x81: bpp = 1; break;
    case 0x82: case 0x83: case 0x84: case 0x8b: bpp = 2; break;
    case 0x85: bpp = 4; break;
    default:
        if (t.pitch && t.pitch <= 0x100000) return static_cast<std::uint64_t>(t.pitch) * h * d;
        return 0;
    }
    return w * h * d * bpp;
}

static std::uint32_t packed_pitch_for(std::uint8_t format, std::uint16_t width)
{
    const std::uint8_t base = static_cast<std::uint8_t>(format & ~0x60u);
    if (base == 0x86) return ((static_cast<std::uint32_t>(width) + 3) / 4) * 8;
    if (base == 0x87 || base == 0x88) return ((static_cast<std::uint32_t>(width) + 3) / 4) * 16;
    switch (base)
    {
    case 0x81: return width;
    case 0x82: case 0x83: case 0x84: case 0x8b: return static_cast<std::uint32_t>(width) * 2;
    case 0x85: return static_cast<std::uint32_t>(width) * 4;
    default: return 0;
    }
}

static std::uint32_t find_control3_pitch_for_texture(const std::uint8_t* data, std::size_t data_size,
                                                     std::size_t position, std::uint8_t unit,
                                                     std::uint8_t format, std::uint16_t width)
{
    // NV4097_SET_TEXTURE_CONTROL3 is method 0x1840. The 16 fragment units
    // occupy consecutive 32-bit methods here (0x1840 + unit*4). RPCS3 reads
    // the low 20 bits of this register as the texture pitch.
    if (!(format & 0x20u) || position + 4 > data_size) return 0; // Pitch is meaningful for linear textures.

    const std::uint32_t target = 0x1840u + static_cast<std::uint32_t>(unit) * 4u;
    const std::uint32_t texture_offset = 0x1a00u + static_cast<std::uint32_t>(unit) * 0x20u;
    const std::uint32_t minimum = packed_pitch_for(format, width);
    constexpr std::uint32_t non_increment = 0x40000000u;

    const auto valid_pitch = [minimum](std::uint32_t value) -> std::uint32_t
    {
        const std::uint32_t pitch = value & 0xfffffu;
        return pitch && pitch <= 0x100000u && (!minimum || pitch >= minimum) ? pitch : 0u;
    };

    // SOCOM 4 writes OFFSET..IMAGE_RECT first, then writes CONTROL3. The
    // following CONTROL3 belongs to the texture being bound, so prefer it over
    // older register state. Walk only real packet boundaries, and stop if the
    // same texture unit is rebound before CONTROL3 appears.
    const std::uint32_t texture_cmd = read_be32(data + position);
    const std::uint32_t texture_count = (texture_cmd >> 18) & 0x7ffu;
    const std::uint64_t texture_step64 = 4ull * (static_cast<std::uint64_t>(texture_count) + 1ull);
    if (texture_count && texture_step64 <= data_size - position)
    {
        std::size_t at = position + static_cast<std::size_t>(texture_step64);
        while (at + 4 <= data_size)
        {
            const std::uint32_t cmd = read_be32(data + at);
            if (cmd == 0x00020000u) break;
            if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) != 0)
            {
                at += 4;
                continue;
            }

            const std::uint32_t method = cmd & 0xfffcu;
            const std::uint32_t count = (cmd >> 18) & 0x7ffu;
            if (!count)
            {
                at += 4;
                continue;
            }
            const std::uint64_t step64 = 4ull * (static_cast<std::uint64_t>(count) + 1ull);
            if (step64 > data_size - at) break;
            const bool is_non_increment = (cmd & non_increment) != 0;

            for (std::uint32_t arg = 0; arg < count; ++arg)
            {
                const std::uint64_t effective_method = is_non_increment
                    ? method
                    : static_cast<std::uint64_t>(method) + static_cast<std::uint64_t>(arg) * 4ull;

                if (effective_method == target)
                    return valid_pitch(read_be32(data + at + static_cast<std::size_t>(arg + 1u) * 4u));
                if (effective_method == texture_offset)
                    return 0; // A new binding superseded this one before we saw its CONTROL3.
            }

            at += static_cast<std::size_t>(step64);
        }
    }

    // Fallback for streams that establish CONTROL3 before the texture block.
    // Unlike the old word-at-a-time search, this forward walk can only consume
    // values that are arguments of genuine FIFO method packets.
    std::uint32_t last_pitch = 0;

    for (std::size_t at = 0; at + 4 <= position;)
    {
        const std::uint32_t cmd = read_be32(data + at);

        // RETURN, JUMP and CALL are single control-flow words, not method
        // packets. Keep walking the captured linear buffer; the caller already
        // bounds live secondary buffers at their first packet-boundary RETURN.
        if (cmd == 0x00020000u ||
            (cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) != 0)
        {
            at += 4;
            continue;
        }

        const std::uint32_t method = cmd & 0xfffcu;
        const std::uint32_t count = (cmd >> 18) & 0x7ffu;
        if (!count)
        {
            at += 4;
            continue;
        }

        const std::uint64_t step64 = 4ull * (static_cast<std::uint64_t>(count) + 1ull);
        if (step64 > position - at) break; // Texture packet begins before this packet could end.
        const std::size_t step = static_cast<std::size_t>(step64);
        const bool is_non_increment = (cmd & non_increment) != 0;

        for (std::uint32_t arg = 0; arg < count; ++arg)
        {
            const std::uint64_t effective_method = is_non_increment
                ? method
                : static_cast<std::uint64_t>(method) + static_cast<std::uint64_t>(arg) * 4ull;
            if (effective_method != target) continue;

            const std::size_t value_at = at + static_cast<std::size_t>(arg + 1u) * 4u;
            last_pitch = valid_pitch(read_be32(data + value_at));
        }

        at += step;
    }

    return last_pitch;
}

static std::string hex8(std::uint32_t v);

static std::optional<GcmTileCandidate> parse_gcm_tile_info(const std::uint8_t* p, std::uint32_t entry_ea)
{
    // RPCS3's CellGcmTileInfo is four big-endian u32 values: tile, limit,
    // pitch and format. GcmTileInfo::pack() leaves a very distinctive bit
    // pattern, so this is much less permissive than the legacy texture-
    // descriptor scan.
    GcmTileCandidate t{};
    t.entry_ea = entry_ea;
    t.tile = read_be32(p + 0);
    t.limit = read_be32(p + 4);
    t.pitch = read_be32(p + 8);
    t.format = read_be32(p + 12);

    const std::uint32_t loc_tag = t.tile & 0x3u;
    if (loc_tag != 1u && loc_tag != 2u) return std::nullopt;
    t.location = static_cast<std::uint8_t>(loc_tag - 1u);
    if (((t.tile >> 31) & 1u) != t.location || ((t.limit >> 31) & 1u) != t.location)
        return std::nullopt;

    // tile low bits contain only location+1 and bank[1:0] at bits 4..5.
    if ((t.tile & 0x0000ffccu) != 0) return std::nullopt;
    if ((t.limit & 0x0000ffffu) != 0) return std::nullopt;
    if ((t.pitch & 0xffu) != 0 || t.pitch < 0x100u || t.pitch > 0x20000u)
        return std::nullopt;
    if (!(t.format & (1u << 30))) return std::nullopt; // Bound/valid bit.

    const std::uint32_t begin_segment = (t.tile >> 16) & 0x7fffu;
    const std::uint32_t end_segment = (t.limit >> 16) & 0x7fffu;
    if (end_segment < begin_segment) return std::nullopt;
    const std::uint64_t size = (static_cast<std::uint64_t>(end_segment) - begin_segment + 1u) * 0x10000ull;
    if (!size || size > 0x10000000ull) return std::nullopt;

    t.offset = begin_segment * 0x10000u;
    t.size = static_cast<std::uint32_t>(size);
    t.bank = static_cast<std::uint8_t>((t.tile >> 4) & 0x3u);
    return t;
}

static int scan_gcm_tile_info(HANDLE process, std::uintptr_t vm_base, const Options& opt)
{
    std::ofstream csv(opt.out_dir / L"rsx_tiles.csv");
    if (!csv)
    {
        std::wcerr << L"[!] Could not create rsx_tiles.csv.\n";
        return 10;
    }
    csv << "entry_ea,location,offset,size,pitch,bank,tile,limit,format,covers_filter\n";

    const std::uintptr_t host_begin = vm_base + opt.guest_start;
    const std::uintptr_t host_end = vm_base + opt.guest_end;
    std::uintptr_t cursor = host_begin;
    constexpr std::size_t max_chunk = 8 * 1024 * 1024;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> seen;
    std::size_t count = 0;
    std::size_t covering = 0;

    std::wcout << L"[+] Scanning guest VM for packed CellGcmTileInfo state...\n";
    while (cursor < host_end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (!mbi.RegionSize || region_base > UINTPTR_MAX - mbi.RegionSize) break;
        const auto region_end = region_base + mbi.RegionSize;
        const auto scan_begin = std::max(cursor, region_base);
        const auto scan_end = std::min(host_end, region_end);

        if (mbi.State == MEM_COMMIT && readable_protection(mbi.Protect) && scan_end > scan_begin)
        {
            std::uintptr_t chunk_begin = scan_begin;
            while (chunk_begin < scan_end)
            {
                const auto want = static_cast<std::size_t>(std::min<std::uintptr_t>(max_chunk, scan_end - chunk_begin));
                std::vector<std::uint8_t> data(want);
                SIZE_T got = 0;
                if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(chunk_begin), data.data(), want, &got) && got >= 16)
                {
                    const std::uint32_t guest_base = static_cast<std::uint32_t>(chunk_begin - vm_base);
                    // CellGcmTileInfo itself is 4-byte aligned (not alignas(16)); entries in
                    // its arrays are 16 bytes apart, but the array base need not be mod-16.
                    std::size_t i = (4u - (guest_base & 3u)) & 3u;
                    for (; i + 16 <= got; i += 4)
                    {
                        auto t = parse_gcm_tile_info(data.data() + i, guest_base + static_cast<std::uint32_t>(i));
                        if (!t) continue;
                        const auto key = std::make_tuple(t->tile, t->limit, t->pitch, t->format);
                        if (!seen.insert(key).second) continue;

                        const bool covers = opt.tile_offset_filter != 0xffffffffu &&
                            opt.tile_offset_filter >= t->offset &&
                            static_cast<std::uint64_t>(opt.tile_offset_filter) < static_cast<std::uint64_t>(t->offset) + t->size;
                        if (covers) ++covering;
                        ++count;
                        csv << "0x" << hex8(t->entry_ea)
                            << ',' << static_cast<unsigned>(t->location)
                            << ",0x" << hex8(t->offset)
                            << ",0x" << hex8(t->size)
                            << ",0x" << hex8(t->pitch)
                            << ',' << static_cast<unsigned>(t->bank)
                            << ",0x" << hex8(t->tile)
                            << ",0x" << hex8(t->limit)
                            << ",0x" << hex8(t->format)
                            << ',' << (covers ? "yes" : "no") << '\n';

                        if (covers)
                        {
                            std::wcout << L"    [MATCH] entry=0x" << std::hex << t->entry_ea
                                       << L" loc=" << std::dec << static_cast<unsigned>(t->location)
                                       << L" offset=0x" << std::hex << t->offset
                                       << L" size=0x" << t->size << L" pitch=0x" << t->pitch
                                       << L" bank=" << std::dec << static_cast<unsigned>(t->bank) << L"\n";
                        }
                    }
                }
                chunk_begin += want;
            }
        }
        if (region_end <= cursor) break;
        cursor = region_end;
    }

    std::wcout << L"[+] Unique packed tile-state candidates: " << count << L"\n";
    if (opt.tile_offset_filter != 0xffffffffu)
        std::wcout << L"[+] Tile regions covering offset 0x" << std::hex << opt.tile_offset_filter
                   << L": " << std::dec << covering << L"\n";
    std::wcout << L"[+] Tile manifest: " << (opt.out_dir / L"rsx_tiles.csv") << L"\n";
    return 0;
}

static int scan_gcm_fifo_state(HANDLE process, std::uintptr_t vm_base, const Options& opt,
                               const std::vector<IoMap>& maps)
{
    // CellGcmControl is { be32 put, get, ref }. CellGcmContextData is four
    // big-endian guest pointers: begin, end, current, callback.  Unlike the
    // RSX packet scan, these structures live outside the command arenas and
    // describe the FIFO that is active now.
    std::vector<GcmControlCandidate> controls;
    std::vector<GcmContextCandidate> contexts;
    constexpr std::size_t candidate_cap = 16384;
    constexpr std::size_t max_chunk = 8 * 1024 * 1024;

    const std::uintptr_t host_begin = vm_base + opt.guest_start;
    const std::uintptr_t host_end = vm_base + opt.guest_end;
    std::uintptr_t cursor = host_begin;

    std::wcout << L"[+] Scanning guest VM for live CellGcmControl/CellGcmContextData candidates...\n";
    while (cursor < host_end && (controls.size() < candidate_cap || contexts.size() < candidate_cap))
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (!mbi.RegionSize || region_base > UINTPTR_MAX - mbi.RegionSize) break;
        const auto region_end = region_base + mbi.RegionSize;
        const auto scan_begin = std::max(cursor, region_base);
        const auto scan_end = std::min(host_end, region_end);

        if (mbi.State == MEM_COMMIT && readable_protection(mbi.Protect) && scan_end > scan_begin)
        {
            std::uintptr_t chunk_begin = scan_begin;
            while (chunk_begin < scan_end && (controls.size() < candidate_cap || contexts.size() < candidate_cap))
            {
                const auto want = static_cast<std::size_t>(std::min<std::uintptr_t>(max_chunk, scan_end - chunk_begin));
                std::vector<std::uint8_t> data(want);
                SIZE_T got = 0;
                if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(chunk_begin), data.data(), want, &got) && got >= 16)
                {
                    const std::uint32_t guest_base = static_cast<std::uint32_t>(chunk_begin - vm_base);
                    std::size_t i = (4u - (guest_base & 3u)) & 3u;
                    for (; i + 16 <= got; i += 4)
                    {
                        const std::uint32_t entry_ea = guest_base + static_cast<std::uint32_t>(i);

                        // Real control/context structs are state, not words inside the mapped
                        // FIFO itself. Excluding command arenas eliminates most packet-shaped
                        // false positives while leaving the GCM state pages searchable.
                        if (ea_in_io_arena(entry_ea, maps)) continue;

                        const std::uint32_t a = read_be32(data.data() + i + 0);
                        const std::uint32_t b = read_be32(data.data() + i + 4);
                        const std::uint32_t c = read_be32(data.data() + i + 8);
                        const std::uint32_t d = read_be32(data.data() + i + 12);

                        // The numeric IO ranges overlap ordinary PPU code/data addresses below
                        // 0x04000000, which filled the old candidate cap with ELF pointers before
                        // the scan reached RPCS3's RSX control mapping at 0x50100000. The hardware
                        // control block is a high guest mapping, so only accept control-shaped
                        // triples from high non-command memory.
                        if (controls.size() < candidate_cap && entry_ea >= 0x40000000u &&
                            !(a & 3u) && !(b & 3u) && (a || b))
                        {
                            const auto ma = io_map_for_offset(a, maps);
                            const auto mb = io_map_for_offset(b, maps);
                            if (ma && mb && *ma == *mb)
                            {
                                const auto& map = maps[*ma];
                                const std::uint64_t delta = a > b ? static_cast<std::uint64_t>(a - b) : static_cast<std::uint64_t>(b - a);
                                const std::uint64_t circular = map.size > delta ? std::min(delta, static_cast<std::uint64_t>(map.size) - delta) : delta;
                                if (circular <= 0x01000000ull)
                                    controls.push_back({entry_ea, a, b, c, a, b, c, *ma});
                            }
                        }

                        if (contexts.size() < candidate_cap && !(a & 3u) && !(b & 3u) && !(c & 3u) && !(d & 3u))
                        {
                            const auto ma = io_map_for_ea(a, maps);
                            const auto mb = io_map_for_ea(b ? b - 4u : b, maps);
                            const auto mc = io_map_for_ea(c, maps);
                            if (ma && mb && mc && *ma == *mb && *ma == *mc && a < b && c >= a && c <= b)
                            {
                                const std::uint64_t span = static_cast<std::uint64_t>(b) - a;
                                // PPU callback addresses are executable guest addresses, not
                                // RSX IO pointers. SOCOM 4's game code is well below 0x10000000.
                                if (span >= 0x1000 && span <= 0x04000000 && d >= 0x00010000u && d < 0x10000000u)
                                    contexts.push_back({entry_ea, a, b, c, d, c, *ma});
                            }
                        }
                    }
                }
                chunk_begin += want;
            }
        }
        if (region_end <= cursor) break;
        cursor = region_end;
    }

    std::wcout << L"[+] Snapshot candidates: " << controls.size() << L" control, " << contexts.size()
               << L" context. Taking " << opt.fifo_samples << L" samples at "
               << opt.fifo_sample_ms << L" ms intervals...\n";

    for (std::uint32_t sample = 1; sample < opt.fifo_samples; ++sample)
    {
        Sleep(opt.fifo_sample_ms);
        for (auto& x : controls)
        {
            std::array<std::uint8_t, 12> p{};
            if (!read_remote(process, vm_base + x.entry_ea, p.data(), p.size())) continue;
            const std::uint32_t put = read_be32(p.data() + 0);
            const std::uint32_t get = read_be32(p.data() + 4);
            const std::uint32_t ref = read_be32(p.data() + 8);
            x.put_changed = x.put_changed || put != x.put;
            x.get_changed = x.get_changed || get != x.get;
            x.ref_changed = x.ref_changed || ref != x.ref;
            x.put_after = put;
            x.get_after = get;
            x.ref_after = ref;
        }
        for (auto& x : contexts)
        {
            std::array<std::uint8_t, 16> p{};
            if (!read_remote(process, vm_base + x.entry_ea, p.data(), p.size())) continue;
            if (read_be32(p.data() + 0) == x.begin && read_be32(p.data() + 4) == x.end)
            {
                const std::uint32_t current = read_be32(p.data() + 8);
                x.current_changed = x.current_changed || current != x.current;
                x.current_after = current;
            }
        }
    }

    std::stable_sort(controls.begin(), controls.end(), [](const auto& x, const auto& y)
    {
        const int xs = (x.put_changed ? 2 : 0) + (x.get_changed ? 4 : 0) + (x.ref_changed ? 1 : 0);
        const int ys = (y.put_changed ? 2 : 0) + (y.get_changed ? 4 : 0) + (y.ref_changed ? 1 : 0);
        return xs > ys;
    });
    std::stable_sort(contexts.begin(), contexts.end(), [](const auto& x, const auto& y)
    {
        return x.current_changed > y.current_changed;
    });

    std::ofstream control_csv(opt.out_dir / L"gcm_controls.csv");
    std::ofstream context_csv(opt.out_dir / L"gcm_contexts.csv");
    if (!control_csv || !context_csv)
    {
        std::wcerr << L"[!] Could not create FIFO diagnostic CSV files.\n";
        return 10;
    }
    control_csv << "entry_ea,map_io,put,get,ref,put_after,get_after,ref_after,put_changed,get_changed,ref_changed\n";
    for (const auto& x : controls)
    {
        control_csv << "0x" << hex8(x.entry_ea)
                    << ",0x" << hex8(maps[x.map_index].io)
                    << ",0x" << hex8(x.put) << ",0x" << hex8(x.get) << ",0x" << hex8(x.ref)
                    << ",0x" << hex8(x.put_after) << ",0x" << hex8(x.get_after) << ",0x" << hex8(x.ref_after)
                    << ',' << (x.put_changed ? "yes" : "no")
                    << ',' << (x.get_changed ? "yes" : "no")
                    << ',' << (x.ref_changed ? "yes" : "no") << '\n';
    }
    context_csv << "entry_ea,map_ea,begin,end,current,callback,current_after,current_changed\n";
    for (const auto& x : contexts)
    {
        context_csv << "0x" << hex8(x.entry_ea)
                    << ",0x" << hex8(maps[x.map_index].ea)
                    << ",0x" << hex8(x.begin) << ",0x" << hex8(x.end)
                    << ",0x" << hex8(x.current) << ",0x" << hex8(x.callback)
                    << ",0x" << hex8(x.current_after)
                    << ',' << (x.current_changed ? "yes" : "no") << '\n';
    }

    const auto active_controls = std::count_if(controls.begin(), controls.end(), [](const auto& x)
    {
        return x.put_changed || x.get_changed;
    });
    const auto active_contexts = std::count_if(contexts.begin(), contexts.end(), [](const auto& x)
    {
        return x.current_changed;
    });
    std::wcout << L"[+] Moving FIFO candidates: " << active_controls << L" control, " << active_contexts << L" context.\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(controls.size(), 8); ++i)
    {
        const auto& x = controls[i];
        if (!x.put_changed && !x.get_changed) break;
        std::wcout << L"    [CONTROL] EA=0x" << std::hex << x.entry_ea
                   << L" PUT 0x" << x.put << L"->0x" << x.put_after
                   << L" GET 0x" << x.get << L"->0x" << x.get_after << std::dec << L"\n";
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(contexts.size(), 8); ++i)
    {
        const auto& x = contexts[i];
        if (!x.current_changed) break;
        std::wcout << L"    [CONTEXT] EA=0x" << std::hex << x.entry_ea
                   << L" CURRENT 0x" << x.current << L"->0x" << x.current_after
                   << L" range 0x" << x.begin << L"..0x" << x.end << std::dec << L"\n";
    }
    std::wcout << L"[+] Control manifest: " << (opt.out_dir / L"gcm_controls.csv") << L"\n";
    std::wcout << L"[+] Context manifest: " << (opt.out_dir / L"gcm_contexts.csv") << L"\n";
    return 0;
}

static std::optional<TextureDesc> parse_candidate(const std::uint8_t* p, std::uint32_t descriptor_ea, const std::vector<IoMap>& maps)
{
    TextureDesc t{};
    t.descriptor_ea = descriptor_ea;
    t.format = p[0];
    t.mipmap = p[1];
    t.dimension = p[2];
    t.cubemap = p[3];
    t.remap = read_be32(p + 4);
    t.width = read_be16(p + 8);
    t.height = read_be16(p + 10);
    t.depth = read_be16(p + 12);
    t.location = p[14];
    t.padding = p[15];
    t.pitch = read_be32(p + 16);
    t.offset = read_be32(p + 20);

    if (!known_base_format(t.format)) return std::nullopt;
    if (t.mipmap == 0 || t.mipmap > 16) return std::nullopt;
    if (t.dimension < 1 || t.dimension > 3) return std::nullopt;
    if (t.cubemap > 1 || t.location > 1) return std::nullopt;
    if (t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192) return std::nullopt;
    if (t.depth > 2048) return std::nullopt;
    if (t.padding != 0) return std::nullopt;
    if (t.offset & 0x0f) return std::nullopt;
    if (t.pitch > 0x100000) return std::nullopt;

    const auto data_ea = resolve_texture_ea(t.location, t.offset, maps);
    if (!data_ea) return std::nullopt;
    t.data_ea = *data_ea;
    t.estimated_size = estimate_size(t);
    if (t.estimated_size == 0 || t.estimated_size > (128ull * 1024 * 1024)) return std::nullopt;
    return t;
}

static std::string hex8(std::uint32_t v)
{
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
    return ss.str();
}

static std::string hex2(std::uint8_t v)
{
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(v);
    return ss.str();
}

static bool dump_payload(HANDLE process, std::uintptr_t vm_base, const TextureDesc& t, const fs::path& out_dir,
                         std::size_t index, bool preview_variants)
{
    std::vector<std::uint8_t> data(static_cast<std::size_t>(t.estimated_size));
    if (!read_remote(process, vm_base + t.data_ea, data.data(), data.size())) return false;

    std::ostringstream name;
    name << "tex_" << std::setw(4) << std::setfill('0') << index
         << "_desc_" << hex8(t.descriptor_ea)
         << "_data_" << hex8(t.data_ea)
         << "_fmt_" << hex2(t.format)
         << "_" << t.width << "x" << t.height << ".bin";
    const fs::path raw_path = out_dir / name.str();
    std::ofstream out(raw_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return false;

    preview::write_bc_previews(data, t.format, t.width, t.height, t.pitch, raw_path, preview_variants);
    return true;
}

static std::optional<RsxTexture> parse_rsx_texture_block(const std::uint8_t* p, std::size_t available,
                                                          std::uint32_t command_ea, std::uint32_t io_offset,
                                                          const std::vector<IoMap>& maps)
{
    // NV4097_SET_TEXTURE_OFFSET = 0x1A00. Each of the 16 fragment texture units
    // occupies an 0x20-byte method block through SET_TEXTURE_BORDER_COLOR.
    // We intentionally require one incremental FIFO packet containing at least
    // OFFSET..IMAGE_RECT. That makes this much less prone to matching arbitrary
    // game data than the old CellGcmTexture-structure heuristic.
    if (available < 8 * sizeof(std::uint32_t)) return std::nullopt;

    const std::uint32_t cmd = read_be32(p);
    const std::uint32_t method = cmd & 0xfffcu;
    const std::uint32_t count = (cmd >> 18) & 0x7ffu;
    constexpr std::uint32_t non_increment = 0x40000000u;
    if (cmd & non_increment) return std::nullopt;
    if (method < 0x1a00u || method >= 0x1c00u) return std::nullopt;

    const std::uint32_t rel = method - 0x1a00u;
    if ((rel & 0x1fu) != 0) return std::nullopt; // Must begin at SET_TEXTURE_OFFSET.
    const std::uint32_t unit = rel >> 5;
    if (unit >= 16 || count < 7 || count > 64) return std::nullopt;
    if (available < static_cast<std::size_t>(count + 1) * 4) return std::nullopt;

    RsxTexture r{};
    r.command_ea = command_ea;
    r.io_offset = io_offset;
    r.header = cmd;
    r.unit = static_cast<std::uint8_t>(unit);
    r.offset = read_be32(p + 4);
    r.format_reg = read_be32(p + 8);
    r.address = read_be32(p + 12);
    r.control0 = read_be32(p + 16);
    r.control1 = read_be32(p + 20);
    r.filter = read_be32(p + 24);
    r.image_rect = read_be32(p + 28);
    if (count >= 8) r.border_color = read_be32(p + 32);

    // Matches RPCS3's documented NV4097 texture format bitfield.
    r.location = static_cast<std::uint8_t>((r.format_reg >> 1) & 1u);
    r.cubemap = static_cast<std::uint8_t>((r.format_reg >> 2) & 1u);
    r.dimension = static_cast<std::uint8_t>((r.format_reg >> 4) & 0x0fu);
    r.format = static_cast<std::uint8_t>((r.format_reg >> 8) & 0xffu);
    r.mipmap = static_cast<std::uint16_t>((r.format_reg >> 16) & 0xffffu);
    r.width = static_cast<std::uint16_t>(r.image_rect >> 16);
    r.height = static_cast<std::uint16_t>(r.image_rect & 0xffffu);

    if (!known_base_format(r.format)) return std::nullopt;
    if (r.dimension < 1 || r.dimension > 3) return std::nullopt;
    if (r.mipmap == 0 || r.mipmap > 16) return std::nullopt;
    if (r.width == 0 || r.height == 0 || r.width > 8192 || r.height > 8192) return std::nullopt;
    const auto data_ea = resolve_texture_ea(r.location, r.offset, maps);
    if (!data_ea) return std::nullopt;
    r.data_ea = *data_ea;

    TextureDesc t{};
    t.format = r.format;
    t.mipmap = static_cast<std::uint8_t>(r.mipmap);
    t.dimension = r.dimension;
    t.cubemap = r.cubemap;
    t.location = r.location;
    t.width = r.width;
    t.height = r.height;
    t.depth = 1;
    t.offset = r.offset;
    t.data_ea = r.data_ea;
    r.estimated_size = estimate_size(t);
    if (r.estimated_size == 0 || r.estimated_size > 128ull * 1024 * 1024) return std::nullopt;
    return r;
}

static bool dump_rsx_payload(HANDLE process, std::uintptr_t vm_base, const RsxTexture& r,
                             const fs::path& out_dir, std::size_t index, bool preview_variants)
{
    TextureDesc t{};
    t.descriptor_ea = r.command_ea;
    t.format = r.format;
    t.mipmap = static_cast<std::uint8_t>(r.mipmap);
    t.dimension = r.dimension;
    t.cubemap = r.cubemap;
    t.width = r.width;
    t.height = r.height;
    t.depth = 1;
    t.location = r.location;
    t.pitch = r.pitch;
    t.offset = r.offset;
    t.data_ea = r.data_ea;
    t.estimated_size = r.estimated_size;
    return dump_payload(process, vm_base, t, out_dir, index, preview_variants);
}

static int capture_renderer_ring(HANDLE process, std::uintptr_t vm_base, const Options& opt,
                                 const std::vector<IoMap>& maps)
{
    const auto* profile = profiles::find(opt.profile);
    if (!profile || profile->deep_capture != profiles::DeepCaptureKind::renderer_ring)
    {
        std::wcerr << L"[!] The selected profile does not define a RendererRing Deep Capture.\n";
        return 9;
    }
    const auto ring = std::span(profile->deep_buffers.data(), profile->deep_buffer_count);

    std::ofstream csv(opt.out_dir / L"renderer_ring_textures.csv");
    if (!csv)
    {
        std::wcerr << L"[!] Could not create renderer_ring_textures.csv.\n";
        return 16;
    }
    csv << "index,buffer,command_ea,io_offset,unit,data_ea,location,offset,format,mipmaps,dimension,cubemap,width,height,pitch,"
           "header,format_reg,address,control0,control1,filter,image_rect,border_color,estimated_size,dumped\n";

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint16_t, std::uint16_t>> seen;
    std::size_t texture_count = 0;
    std::size_t dumped_count = 0;
    std::uint64_t dumped_bytes = 0;
    std::size_t buffers_read = 0;

    std::wcout << L"[+] " << profile->game_name << L" deep capture: scanning all "
               << ring.size() << L" configured RendererRing command buffers...\n";
    for (std::size_t buffer_index = 0; buffer_index < ring.size() && texture_count < opt.max_textures; ++buffer_index)
    {
        const auto& range = ring[buffer_index];
        const auto begin_map = io_map_for_offset(range.begin, maps);
        const auto end_map = io_map_for_offset(range.end - 1u, maps);
        if (!begin_map || !end_map || *begin_map != *end_map)
        {
            std::wcerr << L"[!] " << profile->game_name << L" RendererRing buffer " << buffer_index
                       << L" does not fit a confirmed RSX IO mapping; skipping it.\n";
            continue;
        }

        const auto& map = maps[*begin_map];
        const std::uint32_t begin_ea = map.ea + (range.begin - map.io);
        const std::size_t size = static_cast<std::size_t>(range.end - range.begin);
        std::vector<std::uint8_t> bytes(size);
        if (!read_remote(process, vm_base + begin_ea, bytes.data(), bytes.size()))
        {
            std::wcerr << L"[!] Could not read " << profile->game_name << L" RendererRing buffer " << buffer_index
                       << L" at EA 0x" << std::hex << begin_ea << std::dec << L"; skipping it.\n";
            continue;
        }
        ++buffers_read;

        std::size_t buffer_textures = 0;
        for (std::size_t pos = 0; pos + 32 <= bytes.size() && texture_count < opt.max_textures; pos += 4)
        {
            auto r = parse_rsx_texture_block(bytes.data() + pos, bytes.size() - pos,
                                             begin_ea + static_cast<std::uint32_t>(pos),
                                             range.begin + static_cast<std::uint32_t>(pos), maps);
            if (!r) continue;

            r->pitch = find_control3_pitch_for_texture(bytes.data(), bytes.size(), pos,
                                                       r->unit, r->format, r->width);
            if (r->pitch)
            {
                TextureDesc sized{};
                sized.format = r->format;
                sized.width = r->width;
                sized.height = r->height;
                sized.depth = 1;
                sized.pitch = r->pitch;
                r->estimated_size = estimate_size(sized);
            }

            std::array<std::uint8_t, 16> probe{};
            if (!read_remote(process, vm_base + r->data_ea, probe.data(), probe.size())) continue;
            const auto key = std::make_tuple(r->location, r->offset, r->format, r->width, r->height);
            if (!seen.insert(key).second) continue;

            const std::size_t idx = texture_count++;
            ++buffer_textures;
            bool dumped = false;
            const std::uint64_t remaining = opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, dumped_bytes);
            if (!opt.list_only && r->estimated_size <= remaining)
            {
                dumped = dump_rsx_payload(process, vm_base, *r, opt.out_dir, idx, opt.preview_variants);
                if (dumped)
                {
                    ++dumped_count;
                    dumped_bytes += r->estimated_size;
                }
            }

            csv << idx
                << ',' << buffer_index
                << ",0x" << hex8(r->command_ea)
                << ",0x" << hex8(r->io_offset)
                << ',' << static_cast<unsigned>(r->unit)
                << ",0x" << hex8(r->data_ea)
                << ',' << static_cast<unsigned>(r->location)
                << ",0x" << hex8(r->offset)
                << ",0x" << hex2(r->format)
                << ',' << r->mipmap
                << ',' << static_cast<unsigned>(r->dimension)
                << ',' << static_cast<unsigned>(r->cubemap)
                << ',' << r->width << ',' << r->height << ',' << r->pitch
                << ",0x" << hex8(r->header)
                << ",0x" << hex8(r->format_reg)
                << ",0x" << hex8(r->address)
                << ",0x" << hex8(r->control0)
                << ",0x" << hex8(r->control1)
                << ",0x" << hex8(r->filter)
                << ",0x" << hex8(r->image_rect)
                << ",0x" << hex8(r->border_color)
                << ',' << r->estimated_size
                << ',' << (dumped ? "yes" : "no") << '\n';
        }

        std::wcout << L"    RendererRing[" << buffer_index << L"] IO 0x" << std::hex << range.begin
                   << L"..0x" << range.end << std::dec << L": " << buffer_textures
                   << L" unique texture binding(s)\n";
    }

    std::wcout << L"[+] " << profile->game_name << L" RendererRing buffers read: " << buffers_read << L"/" << ring.size() << L"\n";
    std::wcout << L"[+] " << profile->game_name << L" RendererRing unique texture bindings: " << texture_count << L"\n";
    if (!opt.list_only)
        std::wcout << L"[+] " << profile->game_name << L" RendererRing payloads dumped: " << dumped_count << L" ("
                   << (dumped_bytes / (1024 * 1024)) << L" MB)\n";
    std::wcout << L"[+] RendererRing manifest: " << (opt.out_dir / L"renderer_ring_textures.csv") << L"\n";
    return 0;
}

static bool recent_primary_has_socom_call(HANDLE process, std::uintptr_t vm_base, const IoMap& map,
                                          std::uint32_t put, std::uint64_t history_bytes)
{
    if (put <= map.io) return false;
    const std::uint64_t available = static_cast<std::uint64_t>(put) - map.io;
    const std::uint64_t back = std::min(history_bytes, available);
    if (back < 4) return false;
    const std::uint32_t start_io = put - static_cast<std::uint32_t>(back);
    const std::uint32_t start_ea = map.ea + (start_io - map.io);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(back));
    if (!read_remote(process, vm_base + start_ea, bytes.data(), bytes.size())) return false;

    constexpr std::array<std::uint32_t, 6> call_words = {
        0x0216C582u, 0x02194602u, 0x021BC682u,
        0x021E4702u, 0x021EC782u, 0x021F4802u
    };
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
    {
        const std::uint32_t word = read_be32(bytes.data() + i);
        if (std::find(call_words.begin(), call_words.end(), word) != call_words.end()) return true;
    }
    return false;
}

static int capture_active_fifo(HANDLE process, std::uintptr_t vm_base, const Options& opt,
                               const std::vector<IoMap>& maps)
{
    // MAG's Deep Capture is intentionally independent of the instantaneous
    // primary FIFO GET/PUT window. Scan the complete RendererRing first so a
    // wrapped primary FIFO cannot prevent the useful texture inventory pass.
    const bool renderer_ring_deep = opt.renderer_ring && is_renderer_ring_profile(opt.profile);
    if (renderer_ring_deep)
    {
        const int ring_result = capture_renderer_ring(process, vm_base, opt, maps);
        if (ring_result != 0) return ring_result;
    }

    std::array<std::uint8_t, 12> control{};
    std::uint32_t put = 0;
    std::uint32_t get = 0;
    std::uint32_t ref = 0;
    std::size_t map_index = 0;
    std::uint64_t span64 = 0;
    std::uint32_t wrapped_samples = 0;
    bool have_linear_sample = false;
    std::uint32_t linear_put = 0;
    std::uint32_t linear_get = 0;
    std::uint32_t linear_ref = 0;
    std::size_t linear_map_index = 0;
    std::uint64_t linear_span64 = 0;
    constexpr std::uint64_t max_fifo_capture = 64ull * 1024 * 1024;

    for (std::uint32_t sample = 0; sample < opt.fifo_samples; ++sample)
    {
        if (!read_remote(process, vm_base + opt.control_ea, control.data(), control.size()))
        {
            std::wcerr << L"[!] Could not read CellGcmControl at EA 0x" << std::hex << opt.control_ea << std::dec << L".\n";
            return 10;
        }

        put = read_be32(control.data());
        get = read_be32(control.data() + 4);
        ref = read_be32(control.data() + 8);
        const auto get_map = io_map_for_offset(get, maps);
        const auto put_map = io_map_for_offset(put, maps);
        if (!get_map || !put_map || *get_map != *put_map)
        {
            std::wcerr << L"[!] GET/PUT do not resolve through the same known RSX IO map: GET=0x"
                       << std::hex << get << L" PUT=0x" << put << std::dec << L".\n";
            return 11;
        }
        map_index = *get_map;

        if (get > put)
        {
            ++wrapped_samples;
            if (sample + 1 < opt.fifo_samples)
            {
                std::wcout << L"[*] FIFO wrap observed at sample " << (sample + 1) << L"/" << opt.fifo_samples
                           << L" (GET=0x" << std::hex << get << L", PUT=0x" << put << std::dec
                           << L"); waiting for a linear window and retrying in " << opt.fifo_sample_ms << L" ms...\n";
                Sleep(opt.fifo_sample_ms);
                continue;
            }

            if (have_linear_sample)
            {
                put = linear_put;
                get = linear_get;
                ref = linear_ref;
                map_index = linear_map_index;
                span64 = linear_span64;
                std::wcout << L"[*] Final sample was wrapped; using the most recent safe linear FIFO sample instead.\n";
                break;
            }

            std::wcerr << L"[!] FIFO remained wrapped at the final capture sample (GET=0x" << std::hex << get
                       << L", PUT=0x" << put << std::dec << L").\n"
                       << L"[!] Ring-boundary reconstruction is intentionally not guessed; no safe linear window was captured after "
                       << opt.fifo_samples << L" attempts (" << wrapped_samples << L" wrapped sample(s)).\n";
            if (renderer_ring_deep)
            {
                std::wcout << L"[*] MAG RendererRing Deep Capture completed successfully; skipping only the wrapped primary FIFO snapshot.\n";
                return 0;
            }
            return 12;
        }

        span64 = static_cast<std::uint64_t>(put) - get;
        if (span64 > max_fifo_capture)
        {
            std::wcerr << L"[!] Active FIFO span is unexpectedly large (" << span64 << L" bytes); refusing the capture.\n";
            return 13;
        }

        have_linear_sample = true;
        linear_put = put;
        linear_get = get;
        linear_ref = ref;
        linear_map_index = map_index;
        linear_span64 = span64;

        if (span64 != 0)
        {
            if (!opt.fifo_follow_calls || recent_primary_has_socom_call(process, vm_base, maps[map_index], put, opt.fifo_history_bytes))
                break;

            if (sample + 1 < opt.fifo_samples)
            {
                std::wcout << L"[*] Active FIFO found, but recent history has not reached a confirmed SOCOM4 secondary CALL yet; retrying in "
                           << opt.fifo_sample_ms << L" ms...\n";
                Sleep(opt.fifo_sample_ms);
                continue;
            }
            std::wcout << L"[*] No confirmed SOCOM4 secondary CALL appeared within the capture retries; preserving the best primary snapshot anyway.\n";
            break;
        }

        if (sample + 1 < opt.fifo_samples)
        {
            std::wcout << L"[*] FIFO empty at sample " << (sample + 1) << L"/" << opt.fifo_samples
                       << L"; retrying in " << opt.fifo_sample_ms << L" ms...\n";
            Sleep(opt.fifo_sample_ms);
        }
    }

    const auto& m = maps[map_index];
    const std::uint32_t get_ea = m.ea + (get - m.io);
    const std::uint32_t put_ea = m.ea + (put - m.io);
    const std::uint64_t available_history = static_cast<std::uint64_t>(get) - m.io;
    const std::uint64_t history_back = std::min(opt.fifo_history_bytes, available_history);
    const std::uint32_t history_io = get - static_cast<std::uint32_t>(history_back);
    const std::uint32_t history_ea = m.ea + (history_io - m.io);
    const std::uint64_t history_size64 = static_cast<std::uint64_t>(put) - history_io;

    std::ofstream meta(opt.out_dir / L"active_fifo.csv");
    if (!meta)
    {
        std::wcerr << L"[!] Could not create active_fifo.csv.\n";
        return 14;
    }
    meta << "control_ea,put,get,ref,map_io,map_ea,get_ea,put_ea,size,history_io,history_ea,history_size\n"
         << "0x" << hex8(opt.control_ea)
         << ",0x" << hex8(put)
         << ",0x" << hex8(get)
         << ",0x" << hex8(ref)
         << ",0x" << hex8(m.io)
         << ",0x" << hex8(m.ea)
         << ",0x" << hex8(get_ea)
         << ",0x" << hex8(put_ea)
         << ',' << span64
         << ",0x" << hex8(history_io)
         << ",0x" << hex8(history_ea)
         << ',' << history_size64 << '\n';

    std::wcout << L"[+] CellGcmControl EA 0x" << std::hex << opt.control_ea
               << L": GET=0x" << get << L" PUT=0x" << put << L" REF=0x" << ref << std::dec << L"\n";
    std::wcout << L"[+] Active FIFO: EA 0x" << std::hex << get_ea << L"..0x" << put_ea
               << std::dec << L" (" << span64 << L" bytes)\n";
    std::wcout << L"[+] Recent FIFO history: EA 0x" << std::hex << history_ea << L"..0x" << put_ea
               << std::dec << L" (" << history_size64 << L" bytes; GET boundary at +" << history_back << L")\n";

    std::ofstream texture_csv(opt.out_dir / L"active_fifo_textures.csv");
    if (!texture_csv)
    {
        std::wcerr << L"[!] Could not create active_fifo_textures.csv.\n";
        return 14;
    }
    texture_csv << "index,command_ea,io_offset,unit,data_ea,location,offset,format,mipmaps,dimension,cubemap,width,height,pitch,"
                   "header,format_reg,address,control0,control1,filter,image_rect,border_color,estimated_size,dumped\n";

    constexpr std::uint64_t max_fifo_history_capture = 128ull * 1024 * 1024;
    if (history_size64 > max_fifo_history_capture)
    {
        std::wcerr << L"[!] Recent FIFO history span is unexpectedly large (" << history_size64 << L" bytes); refusing the history capture.\n";
        return 13;
    }
    std::vector<std::uint8_t> history;
    if (history_size64)
    {
        history.resize(static_cast<std::size_t>(history_size64));
        if (!read_remote(process, vm_base + history_ea, history.data(), history.size()))
        {
            std::wcerr << L"[!] Could not read the recent FIFO history span.\n";
            return 15;
        }
        std::ofstream history_out(opt.out_dir / L"fifo_history.bin", std::ios::binary);
        if (!history_out)
        {
            std::wcerr << L"[!] Could not create fifo_history.bin.\n";
            return 16;
        }
        history_out.write(reinterpret_cast<const char*>(history.data()), static_cast<std::streamsize>(history.size()));
        if (!history_out)
        {
            std::wcerr << L"[!] Failed while writing fifo_history.bin.\n";
            return 16;
        }
        std::wcout << L"[+] Recent primary FIFO history: " << (opt.out_dir / L"fifo_history.bin") << L"\n";
    }

    if (opt.fifo_follow_calls && !history.empty())
    {
        // The corrected SOCOM 4 captures show a three-way cycle of six secondary
        // CellGcmContextData buffers. Their CALL words are ordinary RSX FIFO CALLs:
        // target IO offset with low bits == 2. Keep this deliberately profile-specific
        // until another title gives us equally strong context-range evidence.
        std::map<std::uint32_t, std::uint32_t> socom_secondary_sizes;
        if (const auto* profile = profiles::find(opt.profile);
            profile && profile->deep_capture == profiles::DeepCaptureKind::socom_secondary_calls)
        {
            for (std::size_t i = 0; i < profile->deep_buffer_count; ++i)
            {
                const auto& range = profile->deep_buffers[i];
                socom_secondary_sizes.emplace(range.begin, range.end - range.begin);
            }
        }
        std::map<std::uint32_t, std::uint32_t> last_call_site;

        for (std::size_t i = 0; i + 4 <= history.size();)
        {
            const std::uint32_t cmd = read_be32(history.data() + i);
            const std::uint32_t site_io = history_io + static_cast<std::uint32_t>(i);
            if (cmd == 0x00020000u)
            {
                i += 4;
                continue;
            }
            if ((cmd & 0xE0000003u) == 0x20000000u)
            {
                i += 4;
                continue;
            }
            if ((cmd & 3u) != 0)
            {
                if ((cmd & 3u) == 2u)
                {
                    const std::uint32_t target = cmd & ~3u;
                    if (socom_secondary_sizes.contains(target))
                        last_call_site[target] = site_io;
                }
                i += 4;
                continue;
            }

            const std::uint32_t count = (cmd >> 18) & 0x7ffu;
            const std::uint64_t step = 4ull * (static_cast<std::uint64_t>(count) + 1);
            if (step > history.size() - i) break;
            i += static_cast<std::size_t>(step);
        }

        std::ofstream calls_csv(opt.out_dir / L"fifo_calls.csv");
        if (!calls_csv)
        {
            std::wcerr << L"[!] Could not create fifo_calls.csv.\n";
            return 16;
        }
        calls_csv << "call_site_io,call_site_ea,target_io,target_ea,capture_size,file\n";
        std::ofstream called_textures_csv(opt.out_dir / L"fifo_called_textures.csv");
        if (!called_textures_csv)
        {
            std::wcerr << L"[!] Could not create fifo_called_textures.csv.\n";
            return 16;
        }
        called_textures_csv << "index,target_io,command_ea,io_offset,unit,data_ea,location,offset,format,mipmaps,dimension,cubemap,width,height,pitch,"
                               "header,format_reg,address,control0,control1,filter,image_rect,border_color,estimated_size,dumped\n";
        std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint16_t, std::uint16_t>> called_seen;
        std::size_t called_texture_count = 0;
        std::size_t called_dumped_count = 0;
        std::uint64_t called_dumped_bytes = 0;
        std::size_t captured_calls = 0;
        for (const auto& [target_io, call_site_io] : last_call_site)
        {
            const auto target_map_index = io_map_for_offset(target_io, maps);
            const auto site_map_index = io_map_for_offset(call_site_io, maps);
            if (!target_map_index || !site_map_index) continue;
            const auto& tm = maps[*target_map_index];
            const auto& sm = maps[*site_map_index];
            const std::uint32_t target_ea = tm.ea + (target_io - tm.io);
            const std::uint32_t site_ea = sm.ea + (call_site_io - sm.io);
            const std::uint32_t capture_size = socom_secondary_sizes.at(target_io);

            std::vector<std::uint8_t> secondary(capture_size);
            if (!read_remote(process, vm_base + target_ea, secondary.data(), secondary.size()))
            {
                std::wcerr << L"[!] Could not read secondary FIFO buffer at EA 0x" << std::hex << target_ea << std::dec << L".\n";
                continue;
            }

            const std::string file_name = "fifo_call_" + hex8(target_io) + ".bin";
            std::ofstream out(opt.out_dir / file_name, std::ios::binary);
            if (!out) continue;
            out.write(reinterpret_cast<const char*>(secondary.data()), static_cast<std::streamsize>(secondary.size()));
            if (!out) continue;

            // Only parse the live body of the secondary context. Everything after
            // its first packet-boundary RETURN is unused/stale tail memory.
            std::size_t used_size = secondary.size();
            for (std::size_t pos = 0; pos + 4 <= secondary.size();)
            {
                const std::uint32_t cmd = read_be32(secondary.data() + pos);
                if (cmd == 0x00020000u)
                {
                    used_size = pos + 4;
                    break;
                }
                if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) != 0)
                {
                    pos += 4;
                    continue;
                }
                const std::uint32_t count = (cmd >> 18) & 0x7ffu;
                const std::uint64_t step = 4ull * (static_cast<std::uint64_t>(count) + 1);
                if (step > secondary.size() - pos) break;
                pos += static_cast<std::size_t>(step);
            }

            for (std::size_t pos = 0; pos + 32 <= used_size && called_texture_count < opt.max_textures; pos += 4)
            {
                auto r = parse_rsx_texture_block(secondary.data() + pos, used_size - pos,
                                                 target_ea + static_cast<std::uint32_t>(pos),
                                                 target_io + static_cast<std::uint32_t>(pos), maps);
                if (!r) continue;

                r->pitch = find_control3_pitch_for_texture(secondary.data(), used_size, pos,
                                                           r->unit, r->format, r->width);
                if (r->pitch)
                {
                    TextureDesc sized{};
                    sized.format = r->format;
                    sized.width = r->width;
                    sized.height = r->height;
                    sized.depth = 1;
                    sized.pitch = r->pitch;
                    r->estimated_size = estimate_size(sized);
                }

                std::array<std::uint8_t, 16> probe{};
                if (!read_remote(process, vm_base + r->data_ea, probe.data(), probe.size())) continue;
                const auto key = std::make_tuple(r->location, r->offset, r->format, r->width, r->height);
                if (!called_seen.insert(key).second) continue;

                const std::size_t idx = called_texture_count++;
                bool dumped = false;
                const std::uint64_t remaining = opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, called_dumped_bytes);
                if (!opt.list_only && r->estimated_size <= remaining)
                {
                    dumped = dump_rsx_payload(process, vm_base, *r, opt.out_dir, idx, opt.preview_variants);
                    if (dumped) { ++called_dumped_count; called_dumped_bytes += r->estimated_size; }
                }

                called_textures_csv << idx
                                    << ",0x" << hex8(target_io)
                                    << ",0x" << hex8(r->command_ea)
                                    << ",0x" << hex8(r->io_offset)
                                    << ',' << static_cast<unsigned>(r->unit)
                                    << ",0x" << hex8(r->data_ea)
                                    << ',' << static_cast<unsigned>(r->location)
                                    << ",0x" << hex8(r->offset)
                                    << ",0x" << hex2(r->format)
                                    << ',' << r->mipmap
                                    << ',' << static_cast<unsigned>(r->dimension)
                                    << ',' << static_cast<unsigned>(r->cubemap)
                                    << ',' << r->width << ',' << r->height << ',' << r->pitch
                                    << ",0x" << hex8(r->header)
                                    << ",0x" << hex8(r->format_reg)
                                    << ",0x" << hex8(r->address)
                                    << ",0x" << hex8(r->control0)
                                    << ",0x" << hex8(r->control1)
                                    << ",0x" << hex8(r->filter)
                                    << ",0x" << hex8(r->image_rect)
                                    << ",0x" << hex8(r->border_color)
                                    << ',' << r->estimated_size
                                    << ',' << (dumped ? "yes" : "no") << '\n';
            }

            calls_csv << "0x" << hex8(call_site_io)
                      << ",0x" << hex8(site_ea)
                      << ",0x" << hex8(target_io)
                      << ",0x" << hex8(target_ea)
                      << ',' << capture_size
                      << ',' << file_name << '\n';
            ++captured_calls;
            std::wcout << L"    CALL IO 0x" << std::hex << call_site_io << L" -> 0x" << target_io
                       << L" (EA 0x" << target_ea << std::dec << L", " << capture_size << L" bytes)\n";
        }
        std::wcout << L"[+] Confirmed SOCOM secondary FIFO buffers captured: " << captured_calls << L"\n";
        std::wcout << L"[+] CALL manifest: " << (opt.out_dir / L"fifo_calls.csv") << L"\n";
        std::wcout << L"[+] Unique textures referenced by called secondary buffers: " << called_texture_count << L"\n";
        if (!opt.list_only)
            std::wcout << L"[+] Called-buffer payloads dumped: " << called_dumped_count << L" (" << (called_dumped_bytes / (1024 * 1024)) << L" MB)\n";
        std::wcout << L"[+] Called-buffer texture manifest: " << (opt.out_dir / L"fifo_called_textures.csv") << L"\n";
    }

    if (span64 == 0)
    {
        std::wcout << L"[*] GET equaled PUT in all " << opt.fifo_samples
                   << L" samples; no pending primary FIFO bytes were captured, but recent FIFO history was preserved.\n";
        return 0;
    }

    std::vector<std::uint8_t> fifo(static_cast<std::size_t>(span64));
    if (!read_remote(process, vm_base + get_ea, fifo.data(), fifo.size()))
    {
        std::wcerr << L"[!] Could not read the active FIFO span.\n";
        return 15;
    }

    {
        std::ofstream raw(opt.out_dir / L"active_fifo.bin", std::ios::binary);
        if (!raw)
        {
            std::wcerr << L"[!] Could not create active_fifo.bin.\n";
            return 16;
        }
        raw.write(reinterpret_cast<const char*>(fifo.data()), static_cast<std::streamsize>(fifo.size()));
        if (!raw)
        {
            std::wcerr << L"[!] Failed while writing active_fifo.bin.\n";
            return 16;
        }
    }

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint16_t, std::uint16_t>> seen;
    std::size_t candidate_count = 0;
    std::size_t dumped_count = 0;
    std::uint64_t dumped_bytes = 0;
    for (std::size_t i = 0; i + 32 <= fifo.size() && candidate_count < opt.max_textures; i += 4)
    {
        auto r = parse_rsx_texture_block(fifo.data() + i, fifo.size() - i,
                                         get_ea + static_cast<std::uint32_t>(i),
                                         get + static_cast<std::uint32_t>(i), maps);
        if (!r) continue;

        r->pitch = find_control3_pitch_for_texture(fifo.data(), fifo.size(), i,
                                                   r->unit, r->format, r->width);
        if (r->pitch)
        {
            TextureDesc sized{};
            sized.format = r->format;
            sized.width = r->width;
            sized.height = r->height;
            sized.depth = 1;
            sized.pitch = r->pitch;
            r->estimated_size = estimate_size(sized);
        }

        std::array<std::uint8_t, 16> probe{};
        if (!read_remote(process, vm_base + r->data_ea, probe.data(), probe.size())) continue;
        const auto key = std::make_tuple(r->location, r->offset, r->format, r->width, r->height);
        if (!seen.insert(key).second) continue;

        const std::size_t idx = candidate_count++;
        bool dumped = false;
        const std::uint64_t remaining = opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, dumped_bytes);
        if (!opt.list_only && r->estimated_size <= remaining)
        {
            dumped = dump_rsx_payload(process, vm_base, *r, opt.out_dir, idx, opt.preview_variants);
            if (dumped) { ++dumped_count; dumped_bytes += r->estimated_size; }
        }

        texture_csv << idx << ",0x" << hex8(r->command_ea)
                    << ",0x" << hex8(r->io_offset)
                    << ',' << static_cast<unsigned>(r->unit)
                    << ",0x" << hex8(r->data_ea)
                    << ',' << static_cast<unsigned>(r->location)
                    << ",0x" << hex8(r->offset)
                    << ",0x" << hex2(r->format)
                    << ',' << r->mipmap
                    << ',' << static_cast<unsigned>(r->dimension)
                    << ',' << static_cast<unsigned>(r->cubemap)
                    << ',' << r->width << ',' << r->height << ',' << r->pitch
                    << ",0x" << hex8(r->header)
                    << ",0x" << hex8(r->format_reg)
                    << ",0x" << hex8(r->address)
                    << ",0x" << hex8(r->control0)
                    << ",0x" << hex8(r->control1)
                    << ",0x" << hex8(r->filter)
                    << ",0x" << hex8(r->image_rect)
                    << ",0x" << hex8(r->border_color)
                    << ',' << r->estimated_size
                    << ',' << (dumped ? "yes" : "no") << '\n';
    }

    std::wcout << L"[+] Raw primary FIFO snapshot: " << (opt.out_dir / L"active_fifo.bin") << L"\n";
    std::wcout << L"[+] Live-window texture bindings found inline: " << candidate_count << L"\n";
    if (!opt.list_only)
        std::wcout << L"[+] Payloads dumped successfully: " << dumped_count << L" (" << (dumped_bytes / (1024 * 1024)) << L" MB)\n";
    std::wcout << L"[+] FIFO metadata: " << (opt.out_dir / L"active_fifo.csv") << L"\n"
               << L"[+] Texture manifest: " << (opt.out_dir / L"active_fifo_textures.csv") << L"\n";
    return 0;
}

static int scan_rsx_texture_commands(HANDLE process, std::uintptr_t vm_base, const Options& opt,
                                     const std::vector<IoMap>& maps)
{
    std::ofstream csv(opt.out_dir / L"rsx_textures.csv");
    if (!csv)
    {
        std::wcerr << L"[!] Could not create rsx_textures.csv.\n";
        return 10;
    }
    csv << "index,command_ea,io_offset,unit,data_ea,location,offset,format,mipmaps,dimension,cubemap,width,height,pitch,"
           "header,format_reg,address,control0,control1,filter,image_rect,border_color,estimated_size,dumped\n";

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint16_t, std::uint16_t>> seen;
    std::size_t candidate_count = 0;
    std::size_t dumped_count = 0;
    std::uint64_t dumped_bytes = 0;
    constexpr std::size_t overlap = 260; // Covers the largest packet accepted above.
    constexpr std::size_t chunk_size = 4 * 1024 * 1024;

    std::wcout << L"[+] Scanning mapped RSX IO arenas for coherent fragment-texture packets...\n";
    for (const auto& m : maps)
    {
        std::uint32_t pos = 0;
        while (pos < m.size && candidate_count < opt.max_textures)
        {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, static_cast<std::uint64_t>(m.size) - pos));
            const std::size_t extra = (pos + want < m.size) ? std::min<std::size_t>(overlap, m.size - pos - want) : 0;
            std::vector<std::uint8_t> data(want + extra);
            SIZE_T got = 0;
            const std::uintptr_t host = vm_base + static_cast<std::uint64_t>(m.ea) + pos;
            if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(host), data.data(), data.size(), &got) && got >= 32)
            {
                const std::size_t primary = std::min<std::size_t>(want, got);
                for (std::size_t i = 0; i + 32 <= primary && candidate_count < opt.max_textures; i += 4)
                {
                    const std::uint32_t command_ea = m.ea + pos + static_cast<std::uint32_t>(i);
                    const std::uint32_t io = m.io + pos + static_cast<std::uint32_t>(i);
                    auto r = parse_rsx_texture_block(data.data() + i, got - i, command_ea, io, maps);
                    if (!r) continue;

                    r->pitch = find_control3_pitch_for_texture(data.data(), got, i,
                                                               r->unit, r->format, r->width);
                    if (r->pitch)
                    {
                        TextureDesc sized{};
                        sized.format = r->format;
                        sized.width = r->width;
                        sized.height = r->height;
                        sized.depth = 1;
                        sized.pitch = r->pitch;
                        r->estimated_size = estimate_size(sized);
                    }

                    std::array<std::uint8_t, 16> probe{};
                    if (!read_remote(process, vm_base + r->data_ea, probe.data(), probe.size())) continue;
                    const auto key = std::make_tuple(r->location, r->offset, r->format, r->width, r->height);
                    if (!seen.insert(key).second) continue;

                    const std::size_t idx = candidate_count++;
                    bool dumped = false;
                    const std::uint64_t remaining = opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, dumped_bytes);
                    if (!opt.list_only && r->estimated_size <= remaining)
                    {
                        dumped = dump_rsx_payload(process, vm_base, *r, opt.out_dir, idx, opt.preview_variants);
                        if (dumped) { ++dumped_count; dumped_bytes += r->estimated_size; }
                    }

                    csv << idx << ",0x" << hex8(r->command_ea)
                        << ",0x" << hex8(r->io_offset)
                        << ',' << static_cast<unsigned>(r->unit)
                        << ",0x" << hex8(r->data_ea)
                        << ',' << static_cast<unsigned>(r->location)
                        << ",0x" << hex8(r->offset)
                        << ",0x" << hex2(r->format)
                        << ',' << r->mipmap
                        << ',' << static_cast<unsigned>(r->dimension)
                        << ',' << static_cast<unsigned>(r->cubemap)
                        << ',' << r->width << ',' << r->height
                        << ',' << r->pitch
                        << ",0x" << hex8(r->header)
                        << ",0x" << hex8(r->format_reg)
                        << ",0x" << hex8(r->address)
                        << ",0x" << hex8(r->control0)
                        << ",0x" << hex8(r->control1)
                        << ",0x" << hex8(r->filter)
                        << ",0x" << hex8(r->image_rect)
                        << ",0x" << hex8(r->border_color)
                        << ',' << r->estimated_size
                        << ',' << (dumped ? "yes" : "no") << '\n';

                    std::wcout << L"    [" << idx << L"] cmd=0x" << std::hex << r->command_ea
                               << L" io=0x" << r->io_offset << L" data=0x" << r->data_ea
                               << L" fmt=0x" << static_cast<unsigned>(r->format) << std::dec
                               << L" " << r->width << L"x" << r->height
                               << L" unit=" << static_cast<unsigned>(r->unit)
                               << L" pitch=" << r->pitch
                               << (dumped ? L" dumped" : L"") << L"\n";
                }
            }
            if (want == 0) break;
            pos += static_cast<std::uint32_t>(want);
        }
    }

    std::wcout << L"[+] Finished. Unique RSX texture bindings: " << candidate_count << L"\n";
    if (!opt.list_only)
        std::wcout << L"[+] Payloads dumped successfully: " << dumped_count << L" (" << (dumped_bytes / (1024 * 1024)) << L" MB)\n";
    std::wcout << L"[+] Manifest: " << (opt.out_dir / L"rsx_textures.csv") << L"\n";
    return 0;
}

static void usage()
{
    std::wcout <<
        L"RPCS3 Texture Dumper v0.1\n"
        L"Usage: RPCS3TextureDumper.exe [options]\n\n"
        L"  --process NAME       Process name (default rpcs3.exe)\n"
        L"  --profile NAME       Built-in RSX mapping profile (BCUS98135/SOCOM4, BCUS98110/MAG)\n"
        L"  --auto-tune          Choose safe capture limits/retry timing automatically\n"
        L"  --log PATH           Current uncompressed RPCS3.log\n"
        L"  --out DIR            Output directory\n"
        L"  --guest-start HEX    Descriptor scan start EA\n"
        L"  --guest-end HEX      Descriptor scan end EA\n"
        L"  --vm-base HEX        Override detected host vm::g_base_addr\n"
        L"  --max N              Maximum unique candidates (default 2000)\n"
        L"  --dump               Write raw payloads (default is manifest-only)\n"
        L"  --preview-variants   Also emit neutral/Flip-X/Flip-XY BMP diagnostics (Flip-Y is default)\n"
        L"  --budget-mb N        Maximum payload bytes written (default 1024 MB)\n"
        L"  --rsx-scan           Scan mapped RSX memory for FIFO texture packet history\n"
        L"  --fifo-scan          Find moving CellGcmControl/context state (live FIFO diagnostic)\n"
        L"  --fifo-capture       Snapshot the active primary GET-to-PUT FIFO window\n"
        L"  --fifo-follow-calls  Dump confirmed SOCOM4 secondary buffers called by recent FIFO history\n"
        L"  --renderer-ring      Scan RendererRing buffers configured by the selected profile\n"
        L"  --mag-renderer-ring  Legacy alias for --renderer-ring\n"
        L"  --control-ea HEX     CellGcmControl guest EA (profile default when known)\n"
        L"  --fifo-history-mb N  Recent executed bytes to keep before GET (default 2 MB)\n"
        L"  --fifo-sample-ms N   Delay between FIFO samples/capture retries (default 250 ms)\n"
        L"  --fifo-samples N     FIFO samples/capture attempts (default 8)\n"
        L"  --tile-scan          Scan guest state for packed CellGcmTileInfo entries\n"
        L"  --tile-offset HEX    Highlight tile regions covering this RSX offset\n"
        L"  --descriptor-scan    Scan CellGcmTexture-like structs (legacy diagnostic)\n"
        L"  --list-only          Force manifest-only mode\n";
}

static bool parse_args(int argc, wchar_t** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto need = [&](const wchar_t* what) -> const wchar_t*
        {
            if (i + 1 >= argc)
            {
                std::wcerr << L"Missing value for " << what << L"\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == L"--help" || a == L"-h") { usage(); return false; }
        if (a == L"--dump") { o.list_only = false; continue; }
        if (a == L"--auto-tune") { o.auto_tune = true; continue; }
        if (a == L"--preview-variants") { o.preview_variants = true; continue; }
        if (a == L"--list-only") { o.list_only = true; continue; }
        if (a == L"--rsx-scan") { o.rsx_scan = true; continue; }
        if (a == L"--fifo-scan") { o.fifo_scan = true; continue; }
        if (a == L"--fifo-capture") { o.fifo_capture = true; continue; }
        if (a == L"--fifo-follow-calls") { o.fifo_follow_calls = true; continue; }
        if (a == L"--renderer-ring" || a == L"--mag-renderer-ring") { o.renderer_ring = true; continue; }
        if (a == L"--tile-scan") { o.tile_scan = true; continue; }
        if (a == L"--descriptor-scan") { o.rsx_scan = false; continue; }
        if (a == L"--process") { if (const auto* v = need(L"--process")) o.process = v; else return false; continue; }
        if (a == L"--profile") { if (const auto* v = need(L"--profile")) o.profile = v; else return false; continue; }
        if (a == L"--log") { if (const auto* v = need(L"--log")) o.log_path = v; else return false; continue; }
        if (a == L"--out") { if (const auto* v = need(L"--out")) o.out_dir = v; else return false; continue; }

        auto number_arg = [&](const wchar_t* name, auto& target) -> bool
        {
            const auto* v = need(name);
            if (!v) return false;
            const auto n = parse_u64(v);
            if (!n) { std::wcerr << L"Invalid number for " << name << L"\n"; return false; }
            target = static_cast<std::remove_reference_t<decltype(target)>>(*n);
            return true;
        };
        if (a == L"--guest-start") { if (!number_arg(L"--guest-start", o.guest_start)) return false; continue; }
        if (a == L"--guest-end") { if (!number_arg(L"--guest-end", o.guest_end)) return false; continue; }
        if (a == L"--vm-base") { if (!number_arg(L"--vm-base", o.vm_base_override)) return false; continue; }
        if (a == L"--max") { if (!number_arg(L"--max", o.max_textures)) return false; continue; }
        if (a == L"--tile-offset") { if (!number_arg(L"--tile-offset", o.tile_offset_filter)) return false; continue; }
        if (a == L"--control-ea") { if (!number_arg(L"--control-ea", o.control_ea)) return false; continue; }
        if (a == L"--fifo-history-mb")
        {
            std::uint64_t mb = 0;
            if (!number_arg(L"--fifo-history-mb", mb)) return false;
            if (mb > 64) { std::wcerr << L"--fifo-history-mb is capped at 64.\n"; return false; }
            o.fifo_history_bytes = mb * 1024ull * 1024ull;
            continue;
        }
        if (a == L"--fifo-sample-ms")
        {
            if (!number_arg(L"--fifo-sample-ms", o.fifo_sample_ms)) return false;
            if (o.fifo_sample_ms < 16 || o.fifo_sample_ms > 5000)
            {
                std::wcerr << L"--fifo-sample-ms must be between 16 and 5000.\n";
                return false;
            }
            continue;
        }
        if (a == L"--fifo-samples")
        {
            if (!number_arg(L"--fifo-samples", o.fifo_samples)) return false;
            if (o.fifo_samples < 2 || o.fifo_samples > 100)
            {
                std::wcerr << L"--fifo-samples must be between 2 and 100.\n";
                return false;
            }
            continue;
        }
        if (a == L"--budget-mb")
        {
            std::uint64_t mb = 0;
            if (!number_arg(L"--budget-mb", mb)) return false;
            if (mb > 16384) { std::wcerr << L"--budget-mb is capped at 16384.\n"; return false; }
            o.dump_budget_bytes = mb * 1024ull * 1024ull;
            continue;
        }

        std::wcerr << L"Unknown argument: " << a << L"\n";
        return false;
    }
    return true;
}

static fs::path auto_log_path(const ModuleInfo& mod)
{
    const auto beside = mod.path.parent_path() / L"RPCS3.log";
    if (fs::exists(beside)) return beside;
    const auto cwd = fs::current_path() / L"RPCS3.log";
    if (fs::exists(cwd)) return cwd;
    return {};
}

int cli_main(int argc, wchar_t** argv)
{
    Options opt;
    if (!parse_args(argc, argv, opt)) return argc > 1 ? 1 : 0;
    apply_automatic_capture_tuning(opt);
    if (opt.guest_start >= opt.guest_end)
    {
        std::wcerr << L"[!] guest-start must be below guest-end.\n";
        return 1;
    }

    const auto pid = find_process_id(opt.process);
    if (!pid)
    {
        std::wcerr << L"[!] Could not find " << opt.process << L". Boot the game first.\n";
        return 2;
    }

    UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, *pid));
    if (!process)
    {
        std::wcerr << L"[!] OpenProcess failed (" << GetLastError() << L").\n";
        return 3;
    }

    const auto mod = get_main_module(*pid);
    if (!mod)
    {
        std::wcerr << L"[!] Could not enumerate the RPCS3 main module.\n";
        return 4;
    }

    std::uintptr_t vm_base = opt.vm_base_override;
    if (!vm_base)
    {
        auto detected = find_vm_base_signature(process.value, *mod);
        if (!detected)
        {
            std::wcout << L"[*] Legacy VM signature not found; probing the mapped PS3 PPU ELF...\n";
            detected = find_vm_base_from_guest_elf(process.value);
        }
        if (!detected)
        {
            std::wcerr << L"[!] Could not locate RPCS3's guest VM base by signature or PPU ELF mapping.\n"
                       << L"[!] Make sure the PS3 game is fully booted; --vm-base remains available for diagnostics.\n";
            return 5;
        }
        vm_base = *detected;
    }

    std::wcout << L"[+] PID: " << *pid << L"\n";
    std::wcout << L"[+] RPCS3: " << mod->path << L"\n";
    std::wcout << L"[+] vm::g_base_addr = 0x" << std::hex << std::uppercase << vm_base << std::dec << L"\n";
    if (!verify_guest_elf(process.value, vm_base))
    {
        std::wcerr << L"[!] Guest ELF check failed at VM+0x10000. Make sure a PS3 game is booted and the VM base is correct.\n";
        return 6;
    }
    std::wcout << L"[+] Guest ELF mapping verified.\n";

    std::vector<IoMap> maps;
    if (!opt.profile.empty())
    {
        const auto profile_maps = profile_io_maps(opt.profile);
        if (!profile_maps)
        {
            std::wcerr << L"[!] Unknown mapping profile: " << opt.profile << L"\n";
            return 7;
        }
        maps = *profile_maps;
        std::wcout << L"[+] Using built-in RSX mapping profile: " << opt.profile << L"\n";
    }
    else
    {
        if (opt.log_path.empty()) opt.log_path = auto_log_path(*mod);
        if (opt.log_path.empty() || !fs::exists(opt.log_path))
        {
            std::wcerr << L"[!] Current RPCS3.log not found. Pass it with --log PATH,\n"
                       << L"[!] or use a confirmed built-in profile such as BCUS98135 or BCUS98110.\n";
            return 7;
        }
        maps = parse_io_maps(opt.log_path);
        if (maps.empty())
        {
            std::wcerr << L"[!] No sys_rsx_context_iomap entries found in " << opt.log_path << L".\n";
            return 8;
        }
        std::wcout << L"[+] RSX IO maps recovered from " << opt.log_path << L"\n";
    }
    std::wcout << L"[+] RSX IO maps:\n";
    for (const auto& m : maps)
    {
        std::wcout << L"    IO 0x" << std::hex << std::setw(8) << std::setfill(L'0') << m.io
                   << L" -> EA 0x" << std::setw(8) << m.ea
                   << L" size 0x" << std::setw(8) << m.size << std::dec << L"\n";
    }

    std::error_code ec;
    fs::create_directories(opt.out_dir, ec);
    if (ec)
    {
        std::wcerr << L"[!] Could not create output directory: " << opt.out_dir << L"\n";
        return 9;
    }

    if (opt.fifo_capture)
    {
        if (!opt.control_ea)
        {
            const auto* profile = profiles::find(opt.profile);
            if (profile && profile->control_ea)
                opt.control_ea = profile->control_ea;
            else
            {
                std::wcerr << L"[!] --fifo-capture needs --control-ea for this profile.\n";
                return 9;
            }
        }
        return capture_active_fifo(process.value, vm_base, opt, maps);
    }

    if (opt.fifo_scan)
    {
        const int fifo_result = scan_gcm_fifo_state(process.value, vm_base, opt, maps);
        if (fifo_result || (!opt.tile_scan && !opt.rsx_scan)) return fifo_result;
    }

    if (opt.tile_scan)
    {
        const int tile_result = scan_gcm_tile_info(process.value, vm_base, opt);
        if (tile_result || !opt.rsx_scan) return tile_result;
    }

    if (opt.rsx_scan)
        return scan_rsx_texture_commands(process.value, vm_base, opt, maps);

    std::ofstream csv(opt.out_dir / L"textures.csv");
    if (!csv)
    {
        std::wcerr << L"[!] Could not create textures.csv.\n";
        return 10;
    }
    csv << "index,descriptor_ea,data_ea,location,offset,format,mipmaps,dimension,cubemap,width,height,depth,pitch,remap,estimated_size,dumped\n";

    const std::uintptr_t host_begin = vm_base + opt.guest_start;
    const std::uintptr_t host_end = vm_base + opt.guest_end;
    std::uintptr_t cursor = host_begin;
    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint16_t, std::uint16_t, std::uint16_t>> seen;
    std::size_t candidate_count = 0;
    std::size_t dumped_count = 0;
    std::uint64_t dumped_bytes = 0;
    constexpr std::size_t desc_size = 24;
    constexpr std::size_t max_chunk = 8 * 1024 * 1024;

    std::wcout << L"[+] Scanning committed guest VM pages...\n";
    while (cursor < host_end && candidate_count < opt.max_textures)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process.value, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;
        const std::uintptr_t region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t region_end = region_base + mbi.RegionSize;
        const std::uintptr_t scan_begin = std::max(cursor, region_base);
        const std::uintptr_t scan_end = std::min(host_end, region_end);

        if (mbi.State == MEM_COMMIT && readable_protection(mbi.Protect) && scan_end > scan_begin)
        {
            std::uintptr_t chunk_start = scan_begin;
            while (chunk_start < scan_end && candidate_count < opt.max_textures)
            {
                const std::size_t want = static_cast<std::size_t>(std::min<std::uintptr_t>(max_chunk, scan_end - chunk_start));
                std::vector<std::uint8_t> data(want);
                SIZE_T got = 0;
                if (ReadProcessMemory(process.value, reinterpret_cast<LPCVOID>(chunk_start), data.data(), want, &got) && got >= desc_size)
                {
                    const std::uint64_t guest_chunk = chunk_start - vm_base;
                    std::size_t i = static_cast<std::size_t>((4 - (guest_chunk & 3)) & 3);
                    for (; i + desc_size <= got && candidate_count < opt.max_textures; i += 4)
                    {
                        const auto guest64 = guest_chunk + i;
                        if (guest64 > 0xffffffffull) break;
                        auto t = parse_candidate(data.data() + i, static_cast<std::uint32_t>(guest64), maps);
                        if (!t) continue;

                        std::array<std::uint8_t, 16> probe{};
                        if (!read_remote(process.value, vm_base + t->data_ea, probe.data(), probe.size())) continue;
                        const auto key = std::make_tuple(t->location, t->offset, t->format, t->width, t->height, t->depth);
                        if (!seen.insert(key).second) continue;

                        const std::size_t idx = candidate_count++;
                        bool dumped = false;
                        if (!opt.list_only && t->estimated_size <= opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, dumped_bytes))
                        {
                            dumped = dump_payload(process.value, vm_base, *t, opt.out_dir, idx, opt.preview_variants);
                            if (dumped)
                            {
                                ++dumped_count;
                                dumped_bytes += t->estimated_size;
                            }
                        }

                        csv << idx << ",0x" << hex8(t->descriptor_ea)
                            << ",0x" << hex8(t->data_ea)
                            << ',' << static_cast<unsigned>(t->location)
                            << ",0x" << hex8(t->offset)
                            << ",0x" << hex2(t->format)
                            << ',' << static_cast<unsigned>(t->mipmap)
                            << ',' << static_cast<unsigned>(t->dimension)
                            << ',' << static_cast<unsigned>(t->cubemap)
                            << ',' << t->width << ',' << t->height << ',' << t->depth
                            << ',' << t->pitch
                            << ",0x" << hex8(t->remap)
                            << ',' << t->estimated_size
                            << ',' << (dumped ? "yes" : "no") << '\n';

                        std::wcout << L"    [" << idx << L"] desc=0x" << std::hex << t->descriptor_ea
                                   << L" data=0x" << t->data_ea
                                   << L" fmt=0x" << static_cast<unsigned>(t->format) << std::dec
                                   << L" " << t->width << L"x" << t->height
                                   << L" loc=" << static_cast<unsigned>(t->location)
                                   << L" bytes=" << t->estimated_size
                                   << (dumped ? L" dumped" : L"") << L"\n";
                    }
                }
                if (want == 0) break;
                // Keep descriptor-sized overlap between large chunks.
                if (chunk_start + want >= scan_end) break;
                chunk_start += want - desc_size;
            }
        }
        if (region_end <= cursor) break;
        cursor = region_end;
    }

    std::wcout << L"[+] Finished. Unique texture candidates: " << candidate_count << L"\n";
    if (!opt.list_only)
    {
        std::wcout << L"[+] Payloads dumped successfully: " << dumped_count
                   << L" (" << (dumped_bytes / (1024 * 1024)) << L" MB)\n";
    }
    std::wcout << L"[+] Manifest: " << (opt.out_dir / L"textures.csv") << L"\n";
    return 0;
}
