#include "rsx_texture.hpp"

#include "preview.hpp"
#include "process_memory.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace rsx_texture
{
using IoMap = rsx::IoMap;
using rsx::resolve_texture_ea;

std::uint16_t read_be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
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

std::uint64_t estimate_size(const TextureDesc& t)
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

std::uint32_t find_control3_pitch_for_texture(const std::uint8_t* data, std::size_t data_size,
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

std::string hex8(std::uint32_t v);

std::optional<TextureDesc> parse_candidate(const std::uint8_t* p, std::uint32_t descriptor_ea, const std::vector<IoMap>& maps)
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

std::string hex8(std::uint32_t v)
{
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
    return ss.str();
}

std::string hex2(std::uint8_t v)
{
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(v);
    return ss.str();
}

bool dump_payload(HANDLE process, std::uintptr_t vm_base, const TextureDesc& t, const fs::path& out_dir,
                         std::size_t index, bool preview_variants)
{
    std::vector<std::uint8_t> data(static_cast<std::size_t>(t.estimated_size));
    if (!process_memory::read(process, vm_base + t.data_ea, data.data(), data.size())) return false;

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

std::optional<RsxTexture> parse_rsx_texture_block(const std::uint8_t* p, std::size_t available,
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

bool dump_rsx_payload(HANDLE process, std::uintptr_t vm_base, const RsxTexture& r,
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
}
