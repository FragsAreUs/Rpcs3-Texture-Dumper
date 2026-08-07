#include "preview.hpp"

#include <fstream>

namespace fs = std::filesystem;

namespace
{
struct Rgb
{
    std::uint8_t r = 0, g = 0, b = 0;
};

Rgb rgb565(std::uint16_t v)
{
    const unsigned r = (v >> 11) & 31u;
    const unsigned g = (v >> 5) & 63u;
    const unsigned b = v & 31u;
    return {
        static_cast<std::uint8_t>((r * 255u + 15u) / 31u),
        static_cast<std::uint8_t>((g * 255u + 31u) / 63u),
        static_cast<std::uint8_t>((b * 255u + 15u) / 31u)
    };
}

Rgb mix(Rgb a, Rgb b, unsigned wa, unsigned wb, unsigned div)
{
    return {
        static_cast<std::uint8_t>((a.r * wa + b.r * wb) / div),
        static_cast<std::uint8_t>((a.g * wa + b.g * wb) / div),
        static_cast<std::uint8_t>((a.b * wa + b.b * wb) / div)
    };
}

void decode_bc_color(const std::uint8_t* p, bool bc1, Rgb out[16])
{
    const std::uint16_t c0 = static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(p[2] | (static_cast<std::uint16_t>(p[3]) << 8));
    Rgb pal[4] = {rgb565(c0), rgb565(c1), {}, {}};
    if (!bc1 || c0 > c1)
    {
        pal[2] = mix(pal[0], pal[1], 2, 1, 3);
        pal[3] = mix(pal[0], pal[1], 1, 2, 3);
    }
    else
    {
        pal[2] = mix(pal[0], pal[1], 1, 1, 2);
        pal[3] = {};
    }

    const std::uint32_t bits = static_cast<std::uint32_t>(p[4]) |
        (static_cast<std::uint32_t>(p[5]) << 8) |
        (static_cast<std::uint32_t>(p[6]) << 16) |
        (static_cast<std::uint32_t>(p[7]) << 24);
    for (unsigned i = 0; i < 16; ++i) out[i] = pal[(bits >> (i * 2)) & 3u];
}

bool write_bmp(const std::vector<std::uint8_t>& data, std::uint8_t format,
               std::uint16_t width, std::uint16_t height, std::uint32_t pitch,
               const fs::path& path,
               bool flip_x, bool flip_y)
{
    const std::uint8_t base = static_cast<std::uint8_t>(format & ~0x60u);
    const bool bc1 = base == 0x86u;
    if (!bc1 && base != 0x87u && base != 0x88u) return false;

    const unsigned block_bytes = bc1 ? 8u : 16u;
    const unsigned bw = (width + 3u) / 4u;
    const unsigned bh = (height + 3u) / 4u;
    const std::size_t packed_row_bytes = static_cast<std::size_t>(bw) * block_bytes;

    // CELL_GCM_TEXTURE_LN (0x20) means compressed block rows use CONTROL3
    // pitch. SOCOM 4 can align this above the tightly packed BC row width.
    // Ignoring that padding makes every following row start at the wrong block.
    std::size_t source_row_bytes = packed_row_bytes;
    if ((format & 0x20u) && pitch >= packed_row_bytes)
        source_row_bytes = pitch;

    const std::size_t needed = bh == 0 ? 0 :
        (static_cast<std::size_t>(bh - 1u) * source_row_bytes + packed_row_bytes);
    if (width == 0 || height == 0 || data.size() < needed) return false;

    std::vector<Rgb> image(static_cast<std::size_t>(width) * height);
    for (unsigned by = 0; by < bh; ++by)
    {
        for (unsigned bx = 0; bx < bw; ++bx)
        {
            const auto* block = data.data() + static_cast<std::size_t>(by) * source_row_bytes +
                                static_cast<std::size_t>(bx) * block_bytes;
            Rgb colors[16]{};
            decode_bc_color(block + (bc1 ? 0u : 8u), bc1, colors);
            for (unsigned py = 0; py < 4; ++py)
            {
                for (unsigned px = 0; px < 4; ++px)
                {
                    const unsigned x = bx * 4u + px;
                    const unsigned y = by * 4u + py;
                    if (x < width && y < height)
                        image[static_cast<std::size_t>(y) * width + x] = colors[py * 4u + px];
                }
            }
        }
    }

    const std::uint32_t row_bytes = (static_cast<std::uint32_t>(width) * 3u + 3u) & ~3u;
    const std::uint32_t pixel_bytes = row_bytes * height;
    const std::uint32_t file_bytes = 54u + pixel_bytes;
    std::vector<std::uint8_t> bmp(file_bytes, 0);
    auto put16 = [&](std::size_t o, std::uint16_t v)
    {
        bmp[o] = static_cast<std::uint8_t>(v); bmp[o + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto put32 = [&](std::size_t o, std::uint32_t v)
    {
        bmp[o] = static_cast<std::uint8_t>(v); bmp[o + 1] = static_cast<std::uint8_t>(v >> 8);
        bmp[o + 2] = static_cast<std::uint8_t>(v >> 16); bmp[o + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    bmp[0] = 'B'; bmp[1] = 'M'; put32(2, file_bytes); put32(10, 54); put32(14, 40);
    put32(18, width); put32(22, height); put16(26, 1); put16(28, 24); put32(34, pixel_bytes);

    for (unsigned y = 0; y < height; ++y)
    {
        const unsigned src_y = flip_y ? y : (height - 1u - y);
        auto* row = bmp.data() + 54u + static_cast<std::size_t>(y) * row_bytes;
        for (unsigned x = 0; x < width; ++x)
        {
            const unsigned src_x = flip_x ? (width - 1u - x) : x;
            const auto c = image[static_cast<std::size_t>(src_y) * width + src_x];
            row[x * 3u + 0] = c.b;
            row[x * 3u + 1] = c.g;
            row[x * 3u + 2] = c.r;
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
    return static_cast<bool>(out);
}
}

namespace preview
{
bool write_bc_previews(const std::vector<std::uint8_t>& data,
                       std::uint8_t format,
                       std::uint16_t width,
                       std::uint16_t height,
                       std::uint32_t pitch,
                       const fs::path& raw_path,
                       bool variants,
                       bool default_flip_y)
{
    fs::path normal = raw_path;
    normal.replace_extension(L".bmp");
    const bool wrote = write_bmp(data, format, width, height, pitch, normal, false, default_flip_y);
    if (!wrote) return false;
    if (!variants) return true;

    const auto stem = raw_path.stem().wstring();
    const auto parent = raw_path.parent_path();
    if (default_flip_y)
        write_bmp(data, format, width, height, pitch, parent / (stem + L"_neutral.bmp"), false, false);
    else
        write_bmp(data, format, width, height, pitch, parent / (stem + L"_flipy.bmp"), false, true);
    write_bmp(data, format, width, height, pitch, parent / (stem + L"_flipx.bmp"), true, false);
    write_bmp(data, format, width, height, pitch, parent / (stem + L"_flipxy.bmp"), true, true);
    return true;
}
}
