#include "rsx_maps.hpp"

#include <fstream>
#include <map>
#include <regex>

namespace rsx
{
std::vector<IoMap> parse_io_maps(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) return {};

    std::map<std::uint32_t, IoMap> latest;
    const std::regex pattern(
        R"(sys_rsx_context_iomap\([^\n]*?io=0x([0-9a-fA-F]+),\s*ea=0x([0-9a-fA-F]+),\s*size=0x([0-9a-fA-F]+))");
    std::string line;
    while (std::getline(input, line))
    {
        std::smatch match;
        if (!std::regex_search(line, match, pattern)) continue;

        IoMap map{};
        map.io = static_cast<std::uint32_t>(std::stoull(match[1].str(), nullptr, 16));
        map.ea = static_cast<std::uint32_t>(std::stoull(match[2].str(), nullptr, 16));
        map.size = static_cast<std::uint32_t>(std::stoull(match[3].str(), nullptr, 16));
        latest[map.io] = map;
    }

    std::vector<IoMap> result;
    result.reserve(latest.size());
    for (const auto& [_, map] : latest) result.push_back(map);
    return result;
}

std::optional<std::vector<IoMap>> profile_io_maps(const std::wstring& profile)
{
    const auto* selected = profiles::find(profile);
    if (!selected) return std::nullopt;
    return std::vector<IoMap>(
        selected->io_maps.begin(),
        selected->io_maps.begin() + selected->io_map_count);
}

std::optional<std::uint32_t> resolve_texture_ea(
    std::uint8_t location,
    std::uint32_t offset,
    const std::vector<IoMap>& maps)
{
    if (location == 0)
    {
        if (offset >= 0x10000000) return std::nullopt;
        return 0xC0000000u + offset;
    }
    if (location == 1)
    {
        for (const auto& map : maps)
        {
            const std::uint64_t begin = map.io;
            const std::uint64_t end = begin + map.size;
            if (offset >= begin && offset < end)
                return map.ea + (offset - map.io);
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> io_map_for_offset(
    std::uint32_t offset,
    const std::vector<IoMap>& maps)
{
    for (std::size_t index = 0; index < maps.size(); ++index)
    {
        const std::uint64_t begin = maps[index].io;
        const std::uint64_t end = begin + maps[index].size;
        if (offset >= begin && offset < end) return index;
    }
    return std::nullopt;
}

std::optional<std::size_t> io_map_for_ea(
    std::uint32_t ea,
    const std::vector<IoMap>& maps)
{
    for (std::size_t index = 0; index < maps.size(); ++index)
    {
        const std::uint64_t begin = maps[index].ea;
        const std::uint64_t end = begin + maps[index].size;
        if (ea >= begin && ea < end) return index;
    }
    return std::nullopt;
}

bool ea_in_io_arena(std::uint32_t ea, const std::vector<IoMap>& maps)
{
    return io_map_for_ea(ea, maps).has_value();
}
}
