#include "capture_engine.hpp"
#include "diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "process_memory.hpp"
#include "profiles.hpp"
#include "rsx_maps.hpp"
#include "rsx_texture.hpp"

using IoMap = rsx::IoMap;
using Options = app::Options;

using TextureDesc = rsx_texture::TextureDesc;
using RsxTexture = rsx_texture::RsxTexture;

static bool is_renderer_ring_profile(const std::wstring& profile)
{
    const auto* selected = profiles::find(profile);
    return selected && selected->deep_capture == profiles::DeepCaptureKind::renderer_ring;
}

using diagnostics::scan_gcm_fifo_state;
using diagnostics::scan_gcm_tile_info;
using diagnostics::scan_rsx_texture_commands;
using rsx_texture::dump_payload;
using rsx_texture::dump_rsx_payload;
using rsx_texture::estimate_size;
using rsx_texture::find_control3_pitch_for_texture;
using rsx_texture::hex2;
using rsx_texture::hex8;
using rsx_texture::parse_candidate;
using rsx_texture::parse_rsx_texture_block;
using rsx_texture::read_be32;
static bool readable_protection(DWORD protection)
{
    return process_memory::readable_protection(protection);
}

static bool read_remote(HANDLE process, std::uintptr_t address, void* output, std::size_t size)
{
    return process_memory::read(process, address, output, size);
}

using rsx::io_map_for_offset;

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

int capture_engine::run(
    HANDLE process,
    std::uintptr_t vm_base,
    app::Options opt,
    const std::vector<rsx::IoMap>& maps)
{
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
        return capture_active_fifo(process, vm_base, opt, maps);
    }

    if (opt.fifo_scan)
    {
        const int fifo_result = scan_gcm_fifo_state(process, vm_base, opt, maps);
        if (fifo_result || (!opt.tile_scan && !opt.rsx_scan)) return fifo_result;
    }

    if (opt.tile_scan)
    {
        const int tile_result = scan_gcm_tile_info(process, vm_base, opt);
        if (tile_result || !opt.rsx_scan) return tile_result;
    }

    if (opt.rsx_scan)
        return scan_rsx_texture_commands(process, vm_base, opt, maps);

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
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;
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
                if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(chunk_start), data.data(), want, &got) && got >= desc_size)
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
                        if (!read_remote(process, vm_base + t->data_ea, probe.data(), probe.size())) continue;
                        const auto key = std::make_tuple(t->location, t->offset, t->format, t->width, t->height, t->depth);
                        if (!seen.insert(key).second) continue;

                        const std::size_t idx = candidate_count++;
                        bool dumped = false;
                        if (!opt.list_only && t->estimated_size <= opt.dump_budget_bytes - std::min(opt.dump_budget_bytes, dumped_bytes))
                        {
                            dumped = dump_payload(process, vm_base, *t, opt.out_dir, idx, opt.preview_variants);
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
