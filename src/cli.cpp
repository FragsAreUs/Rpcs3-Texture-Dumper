#include "cli.hpp"

#include "modules/capture_engine.hpp"
#include "modules/options.hpp"
#include "modules/process_memory.hpp"
#include "modules/profiles.hpp"
#include "modules/rsx_maps.hpp"

#include <windows.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
fs::path auto_log_path(const process_memory::ModuleInfo& module)
{
    const auto beside_executable = module.path.parent_path() / L"RPCS3.log";
    if (fs::exists(beside_executable)) return beside_executable;
    const auto current_directory = fs::current_path() / L"RPCS3.log";
    if (fs::exists(current_directory)) return current_directory;
    return {};
}
}

int cli_main(int argc, wchar_t** argv)
{
    app::Options options;
    if (!app::parse_args(argc, argv, options)) return argc > 1 ? 1 : 0;
    app::apply_automatic_capture_tuning(options);
    if (options.guest_start >= options.guest_end)
    {
        std::wcerr << L"[!] guest-start must be below guest-end.\n";
        return 1;
    }

    const auto process_id = process_memory::find_process_id(options.process);
    if (!process_id)
    {
        std::wcerr << L"[!] Could not find " << options.process << L". Boot the game first.\n";
        return 2;
    }

    process_memory::UniqueHandle process(
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, *process_id));
    if (!process)
    {
        std::wcerr << L"[!] OpenProcess failed (" << GetLastError() << L").\n";
        return 3;
    }

    const auto module = process_memory::get_main_module(*process_id);
    if (!module)
    {
        std::wcerr << L"[!] Could not enumerate the RPCS3 main module.\n";
        return 4;
    }

    std::uintptr_t vm_base = options.vm_base_override;
    if (!vm_base)
    {
        auto detected = process_memory::find_vm_base_signature(process.get(), *module);
        if (!detected)
        {
            std::wcout << L"[*] Legacy VM signature not found; probing the mapped PS3 PPU ELF...\n";
            detected = process_memory::find_vm_base_from_guest_elf(process.get());
        }
        if (!detected)
        {
            std::wcerr << L"[!] Could not locate RPCS3's guest VM base by signature or PPU ELF mapping.\n"
                       << L"[!] Make sure the PS3 game is fully booted; --vm-base remains available for diagnostics.\n";
            return 5;
        }
        vm_base = *detected;
    }

    std::wcout << L"[+] PID: " << *process_id << L"\n";
    std::wcout << L"[+] RPCS3: " << module->path << L"\n";
    std::wcout << L"[+] vm::g_base_addr = 0x"
               << std::hex << std::uppercase << vm_base << std::dec << L"\n";
    if (!process_memory::verify_guest_elf(process.get(), vm_base))
    {
        std::wcerr << L"[!] Guest ELF check failed at VM+0x10000. "
                      L"Make sure a PS3 game is booted and the VM base is correct.\n";
        return 6;
    }
    std::wcout << L"[+] Guest ELF mapping verified.\n";

    std::vector<rsx::IoMap> maps;
    if (!options.profile.empty())
    {
        const auto profile_maps = rsx::profile_io_maps(options.profile);
        if (!profile_maps)
        {
            std::wcerr << L"[!] Unknown mapping profile: " << options.profile << L"\n";
            return 7;
        }
        maps = *profile_maps;
        std::wcout << L"[+] Using built-in RSX mapping profile: " << options.profile << L"\n";
    }
    else
    {
        if (options.log_path.empty()) options.log_path = auto_log_path(*module);
        if (options.log_path.empty() || !fs::exists(options.log_path))
        {
            std::wcerr << L"[!] Current RPCS3.log not found. Pass it with --log PATH,\n"
                       << L"[!] or use a confirmed built-in profile such as BCUS98135, BCUS98110, or BLES00564.\n";
            return 7;
        }
        maps = rsx::parse_io_maps(options.log_path);
        if (maps.empty())
        {
            std::wcerr << L"[!] No sys_rsx_context_iomap entries found in "
                       << options.log_path << L".\n";
            return 8;
        }
        std::wcout << L"[+] RSX IO maps recovered from " << options.log_path << L"\n";
    }

    std::wcout << L"[+] RSX IO maps:\n";
    for (const auto& map : maps)
    {
        std::wcout << L"    IO 0x" << std::hex << std::setw(8) << std::setfill(L'0') << map.io
                   << L" -> EA 0x" << std::setw(8) << map.ea
                   << L" size 0x" << std::setw(8) << map.size << std::dec << L"\n";
    }

    std::error_code error;
    fs::create_directories(options.out_dir, error);
    if (error)
    {
        std::wcerr << L"[!] Could not create output directory: " << options.out_dir << L"\n";
        return 9;
    }

    return capture_engine::run(process.get(), vm_base, std::move(options), maps);
}
