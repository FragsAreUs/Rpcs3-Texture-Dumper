#include "profiles.hpp"

#include <cwctype>

namespace profiles
{
namespace
{
bool iequals(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

constexpr std::array<GameProfile, 3> kProfiles = {{
    {
        L"BCUS98135",
        L"SOCOM 4",
        L"v01.00",
        L"SOCOM 4 - BCUS98135 v01.00",
        {L"BCUS98135", L"SOCOM4", L"SOCOM 4"},
        0x50100040u,
        {{{0x00000000u, 0x40000000u, 0x03600000u},
          {0x03600000u, 0x43600000u, 0x00D00000u},
          {0x0E000000u, 0x32500000u, 0x00100000u}}},
        3,
        DeepCaptureKind::socom_secondary_calls,
        {{{0x0216C580u, 0x02194580u},
          {0x02194600u, 0x021BC600u},
          {0x021BC680u, 0x021E4680u},
          {0x021E4700u, 0x021EC700u},
          {0x021EC780u, 0x021F4780u},
          {0x021F4800u, 0x021FC800u}}},
        6,
        true,
    },
    {
        L"BCUS98110",
        L"MAG",
        L"v02.12",
        L"MAG - BCUS98110 v02.12",
        {L"BCUS98110", L"MAG", L""},
        0x60100040u,
        {{{0x00000000u, 0x50000000u, 0x02300000u},
          {0x0E000000u, 0x34E00000u, 0x00100000u},
          {0u, 0u, 0u}}},
        2,
        DeepCaptureKind::renderer_ring,
        {{{0x00001000u, 0x000A4000u},
          {0x000A4000u, 0x000D3000u},
          {0x000D3000u, 0x0011B000u},
          {0x0011B000u, 0x00197000u},
          {0x00197000u, 0x00200000u},
          {0u, 0u}}},
        5,
        true,
    },
    {
        L"BLES00564-BLUS30298",
        L"Wolfenstein",
        L"v01.02",
        L"Wolfenstein v01.02",
        {L"BLES00564", L"BLUS30298", L"WOLFENSTEIN"},
        0x80100040u,
        {{{0x00000000u, 0x70000000u, 0x00100000u},
          {0x00100000u, 0x70100000u, 0x01D00000u},
          {0u, 0u, 0u}}},
        2,
        DeepCaptureKind::command_ring,
        {{{0x00000000u, 0x00100000u},
          {0u, 0u},
          {0u, 0u},
          {0u, 0u},
          {0u, 0u},
          {0u, 0u}}},
        1,
        false,
    },
}};
}

std::span<const GameProfile> all()
{
    return kProfiles;
}

const GameProfile& default_profile()
{
    return kProfiles.front();
}

const GameProfile* find(std::wstring_view name)
{
    for (const auto& profile : kProfiles)
    {
        if (iequals(name, profile.id) || iequals(name, profile.game_name)) return &profile;
        for (const auto alias : profile.aliases)
        {
            if (!alias.empty() && iequals(name, alias)) return &profile;
        }
    }
    return nullptr;
}

bool default_preview_flip_y(std::wstring_view name)
{
    const auto* profile = find(name);
    return !profile || profile->preview_flip_y;
}

std::wstring dump_folder_name(const GameProfile& profile)
{
    std::wstring name = std::wstring(profile.game_name) + L" - " + std::wstring(profile.id);
    for (auto& character : name)
    {
        const bool invalid = character < 32 || character == L'<' || character == L'>' ||
                             character == L':' || character == L'"' || character == L'/' ||
                             character == L'\\' || character == L'|' || character == L'?' ||
                             character == L'*';
        if (invalid) character = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name.empty() ? std::wstring(profile.id) : name;
}
}
