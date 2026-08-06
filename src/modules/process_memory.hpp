#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace process_memory
{
class UniqueHandle
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value);
    ~UniqueHandle();

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;

    explicit operator bool() const;
    HANDLE get() const;

private:
    HANDLE value_ = nullptr;
};

struct ModuleInfo
{
    std::uintptr_t base = 0;
    std::uint32_t size = 0;
    std::filesystem::path path;
};

bool readable_protection(DWORD protection);
std::optional<DWORD> find_process_id(const std::wstring& name);
std::optional<ModuleInfo> get_main_module(DWORD process_id);
bool read(HANDLE process, std::uintptr_t address, void* output, std::size_t size);
std::optional<std::uintptr_t> find_vm_base_signature(HANDLE process, const ModuleInfo& module);
std::optional<std::uintptr_t> find_vm_base_from_guest_elf(HANDLE process);
bool verify_guest_elf(HANDLE process, std::uintptr_t vm_base);
}
