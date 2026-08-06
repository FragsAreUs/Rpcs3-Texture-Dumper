#include "process_memory.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <vector>

namespace process_memory
{
UniqueHandle::UniqueHandle(HANDLE value) : value_(value) {}

UniqueHandle::~UniqueHandle()
{
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_)
{
    other.value_ = nullptr;
}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept
{
    if (this != &other)
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = other.value_;
        other.value_ = nullptr;
    }
    return *this;
}

UniqueHandle::operator bool() const
{
    return value_ && value_ != INVALID_HANDLE_VALUE;
}

HANDLE UniqueHandle::get() const
{
    return value_;
}

bool readable_protection(DWORD protection)
{
    if (protection & PAGE_GUARD) return false;
    const DWORD base = protection & 0xff;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

std::optional<DWORD> find_process_id(const std::wstring& name)
{
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return std::nullopt;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return std::nullopt;
    do
    {
        if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) return entry.th32ProcessID;
    } while (Process32NextW(snapshot.get(), &entry));
    return std::nullopt;
}

std::optional<ModuleInfo> get_main_module(DWORD process_id)
{
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (!snapshot) return std::nullopt;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) return std::nullopt;
    return ModuleInfo{
        reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
        entry.modBaseSize,
        entry.szExePath,
    };
}

bool read(HANDLE process, std::uintptr_t address, void* output, std::size_t size)
{
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(
        process,
        reinterpret_cast<LPCVOID>(address),
        output,
        size,
        &bytes_read) && bytes_read == size;
}

namespace
{
std::optional<std::uintptr_t> scan_pattern(
    HANDLE process,
    const ModuleInfo& module,
    const std::vector<int>& pattern)
{
    constexpr std::size_t chunk_size = 4 * 1024 * 1024;
    if (pattern.empty()) return std::nullopt;
    std::vector<std::uint8_t> buffer(chunk_size + pattern.size());

    std::size_t position = 0;
    while (position < module.size)
    {
        const std::size_t wanted = std::min<std::size_t>(chunk_size, module.size - position);
        SIZE_T bytes_read = 0;
        if (ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(module.base + position),
                buffer.data(),
                wanted,
                &bytes_read) &&
            bytes_read >= pattern.size())
        {
            for (std::size_t i = 0; i + pattern.size() <= bytes_read; ++i)
            {
                bool matches = true;
                for (std::size_t j = 0; j < pattern.size(); ++j)
                {
                    if (pattern[j] >= 0 && buffer[i + j] != static_cast<std::uint8_t>(pattern[j]))
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches) return module.base + position + i;
            }
        }
        if (wanted < chunk_size) break;
        position += chunk_size - (pattern.size() - 1);
    }
    return std::nullopt;
}

std::optional<std::uintptr_t> resolve_rip_pointer(
    HANDLE process,
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size)
{
    std::int32_t relative = 0;
    if (!read(process, instruction + displacement_offset, &relative, sizeof(relative))) return std::nullopt;
    const std::uintptr_t slot = instruction + instruction_size + relative;
    std::uintptr_t value = 0;
    if (!read(process, slot, &value, sizeof(value))) return std::nullopt;
    return value;
}
}

std::optional<std::uintptr_t> find_vm_base_signature(HANDLE process, const ModuleInfo& module)
{
    const std::vector<int> pattern{0x48, 0x2B, 0x15, -1, -1, -1, -1, 0x48, 0xB8};
    const auto hit = scan_pattern(process, module, pattern);
    if (!hit) return std::nullopt;
    return resolve_rip_pointer(process, *hit, 3, 7);
}

bool verify_guest_elf(HANDLE process, std::uintptr_t vm_base)
{
    std::array<std::uint8_t, 20> header{};
    if (!read(process, vm_base + 0x10000, header.data(), header.size())) return false;
    return header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
           header[4] == 2 && header[5] == 2 && header[6] == 1 &&
           header[18] == 0x00 && header[19] == 0x15;
}

std::optional<std::uintptr_t> find_vm_base_from_guest_elf(HANDLE process)
{
    constexpr std::uintptr_t guest_elf_ea = 0x10000;
    constexpr std::uintptr_t vm_stride = 0x100000000ull;
    constexpr std::uintptr_t common_vm_base = 0x300000000ull;
    if (verify_guest_elf(process, common_vm_base)) return common_vm_base;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const auto max_address = reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress);
    std::uintptr_t cursor = 0;

    while (cursor < max_address)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &memory, sizeof(memory))) break;

        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize == 0 || region_base >= max_address) break;
        if (memory.RegionSize > max_address - region_base) break;
        const std::uintptr_t region_end = region_base + memory.RegionSize;

        if (memory.State == MEM_COMMIT && readable_protection(memory.Protect))
        {
            const std::uintptr_t block = region_base & ~(vm_stride - 1);
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
}
