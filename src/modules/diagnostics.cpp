#include "diagnostics.hpp"

#include "process_memory.hpp"
#include "rsx_texture.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace diagnostics
{
using IoMap = rsx::IoMap;
using Options = app::Options;
using RsxTexture = rsx_texture::RsxTexture;
using TextureDesc = rsx_texture::TextureDesc;

static bool readable_protection(DWORD protection)
{
    return process_memory::readable_protection(protection);
}

static bool read_remote(HANDLE process, std::uintptr_t address, void* output, std::size_t size)
{
    return process_memory::read(process, address, output, size);
}

using rsx::ea_in_io_arena;
using rsx::io_map_for_ea;
using rsx::io_map_for_offset;
using rsx_texture::dump_rsx_payload;
using rsx_texture::estimate_size;
using rsx_texture::find_control3_pitch_for_texture;
using rsx_texture::hex2;
using rsx_texture::hex8;
using rsx_texture::parse_rsx_texture_block;
using rsx_texture::read_be32;

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

using rsx_texture::dump_payload;
using rsx_texture::dump_rsx_payload;

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

int scan_gcm_tile_info(HANDLE process, std::uintptr_t vm_base, const Options& opt)
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

int scan_gcm_fifo_state(HANDLE process, std::uintptr_t vm_base, const Options& opt,
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

int scan_rsx_texture_commands(HANDLE process, std::uintptr_t vm_base, const Options& opt,
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
}
