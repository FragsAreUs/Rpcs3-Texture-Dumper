#include "options.hpp"

#include "profiles.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace app
{
namespace
{
std::optional<std::uint64_t> parse_u64(std::wstring_view text)
{
    std::wstring value(text);
    int base = 10;
    if (value.size() > 2 && value[0] == L'0' && (value[1] == L'x' || value[1] == L'X'))
    {
        value.erase(0, 2);
        base = 16;
    }

    try
    {
        std::size_t used = 0;
        const auto parsed = std::stoull(value, &used, base);
        if (used != value.size()) return std::nullopt;
        return parsed;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool is_socom4_profile(const std::wstring& name)
{
    const auto* profile = profiles::find(name);
    return profile && profile->deep_capture == profiles::DeepCaptureKind::socom_secondary_calls;
}
}

void apply_automatic_capture_tuning(Options& options)
{
    if (!options.auto_tune) return;

    options.dump_budget_bytes = 1024ull * 1024 * 1024;
    options.max_textures = 4096;
    options.fifo_sample_ms = 100;
    options.fifo_samples = 100;
    options.fifo_history_bytes = (is_socom4_profile(options.profile) ? 8ull : 4ull) * 1024 * 1024;
}

void usage()
{
    std::wcout <<
        L"RPCS3 Texture Dumper v0.1\n"
        L"Usage: RPCS3TextureDumper.exe [options]\n\n"
        L"  --process NAME       Process name (default rpcs3.exe)\n"
        L"  --profile NAME       Built-in RSX mapping profile (SOCOM4, MAG, WOLFENSTEIN)\n"
        L"  --auto-tune          Choose safe capture limits/retry timing automatically\n"
        L"  --log PATH           Current uncompressed RPCS3.log\n"
        L"  --out DIR            Output directory\n"
        L"  --guest-start HEX    Descriptor scan start EA\n"
        L"  --guest-end HEX      Descriptor scan end EA\n"
        L"  --vm-base HEX        Override detected host vm::g_base_addr\n"
        L"  --max N              Maximum unique candidates (default 2000)\n"
        L"  --dump               Write raw payloads (default is manifest-only)\n"
        L"  --preview-variants   Also emit the three non-default BMP orientations\n"
        L"  --budget-mb N        Maximum payload bytes written (default 1024 MB)\n"
        L"  --rsx-scan           Scan mapped RSX memory for FIFO texture packet history\n"
        L"  --fifo-scan          Find moving CellGcmControl/context state (live FIFO diagnostic)\n"
        L"  --fifo-capture       Snapshot the active primary GET-to-PUT FIFO window\n"
        L"  --fifo-follow-calls  Dump confirmed SOCOM4 secondary buffers called by recent FIFO history\n"
        L"  --renderer-ring      Scan command-buffer ranges configured by the selected profile\n"
        L"  --command-ring       Alias for --renderer-ring\n"
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

bool parse_args(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        auto need = [&](const wchar_t* name) -> const wchar_t*
        {
            if (i + 1 >= argc)
            {
                std::wcerr << L"Missing value for " << name << L"\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == L"--help" || argument == L"-h") { usage(); return false; }
        if (argument == L"--dump") { options.list_only = false; continue; }
        if (argument == L"--auto-tune") { options.auto_tune = true; continue; }
        if (argument == L"--preview-variants") { options.preview_variants = true; continue; }
        if (argument == L"--list-only") { options.list_only = true; continue; }
        if (argument == L"--rsx-scan") { options.rsx_scan = true; continue; }
        if (argument == L"--fifo-scan") { options.fifo_scan = true; continue; }
        if (argument == L"--fifo-capture") { options.fifo_capture = true; continue; }
        if (argument == L"--fifo-follow-calls") { options.fifo_follow_calls = true; continue; }
        if (argument == L"--renderer-ring" || argument == L"--command-ring" ||
            argument == L"--mag-renderer-ring") { options.renderer_ring = true; continue; }
        if (argument == L"--tile-scan") { options.tile_scan = true; continue; }
        if (argument == L"--descriptor-scan") { options.rsx_scan = false; continue; }
        if (argument == L"--process") { if (const auto* value = need(L"--process")) options.process = value; else return false; continue; }
        if (argument == L"--profile") { if (const auto* value = need(L"--profile")) options.profile = value; else return false; continue; }
        if (argument == L"--log") { if (const auto* value = need(L"--log")) options.log_path = value; else return false; continue; }
        if (argument == L"--out") { if (const auto* value = need(L"--out")) options.out_dir = value; else return false; continue; }

        auto number_arg = [&](const wchar_t* name, auto& target) -> bool
        {
            const auto* value = need(name);
            if (!value) return false;
            const auto parsed = parse_u64(value);
            if (!parsed)
            {
                std::wcerr << L"Invalid number for " << name << L"\n";
                return false;
            }
            target = static_cast<std::remove_reference_t<decltype(target)>>(*parsed);
            return true;
        };

        if (argument == L"--guest-start") { if (!number_arg(L"--guest-start", options.guest_start)) return false; continue; }
        if (argument == L"--guest-end") { if (!number_arg(L"--guest-end", options.guest_end)) return false; continue; }
        if (argument == L"--vm-base") { if (!number_arg(L"--vm-base", options.vm_base_override)) return false; continue; }
        if (argument == L"--max") { if (!number_arg(L"--max", options.max_textures)) return false; continue; }
        if (argument == L"--tile-offset") { if (!number_arg(L"--tile-offset", options.tile_offset_filter)) return false; continue; }
        if (argument == L"--control-ea") { if (!number_arg(L"--control-ea", options.control_ea)) return false; continue; }

        if (argument == L"--fifo-history-mb")
        {
            std::uint64_t megabytes = 0;
            if (!number_arg(L"--fifo-history-mb", megabytes)) return false;
            if (megabytes > 64)
            {
                std::wcerr << L"--fifo-history-mb is capped at 64.\n";
                return false;
            }
            options.fifo_history_bytes = megabytes * 1024ull * 1024ull;
            continue;
        }
        if (argument == L"--fifo-sample-ms")
        {
            if (!number_arg(L"--fifo-sample-ms", options.fifo_sample_ms)) return false;
            if (options.fifo_sample_ms < 16 || options.fifo_sample_ms > 5000)
            {
                std::wcerr << L"--fifo-sample-ms must be between 16 and 5000.\n";
                return false;
            }
            continue;
        }
        if (argument == L"--fifo-samples")
        {
            if (!number_arg(L"--fifo-samples", options.fifo_samples)) return false;
            if (options.fifo_samples < 2 || options.fifo_samples > 100)
            {
                std::wcerr << L"--fifo-samples must be between 2 and 100.\n";
                return false;
            }
            continue;
        }
        if (argument == L"--budget-mb")
        {
            std::uint64_t megabytes = 0;
            if (!number_arg(L"--budget-mb", megabytes)) return false;
            if (megabytes > 16384)
            {
                std::wcerr << L"--budget-mb is capped at 16384.\n";
                return false;
            }
            options.dump_budget_bytes = megabytes * 1024ull * 1024ull;
            continue;
        }

        std::wcerr << L"Unknown argument: " << argument << L"\n";
        return false;
    }
    return true;
}
}
