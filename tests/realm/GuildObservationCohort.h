#pragma once
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Test-only input parsing. Login selection does not confer event eligibility,
// change native groups/builds, or admit tasks. Validate every ID before login.
namespace LivingIsolated {
inline std::vector<uint32_t> GuildDungeonCohort(const std::string& text) {
    std::vector<uint32_t> result;
    std::set<uint32_t> unique;
    size_t start=0;
    while(start<text.size() && result.size()<5) {
        const auto end=text.find(',',start);
        const auto token=text.substr(start,end==std::string::npos?end:end-start);
        if(token.empty() || token.size()>9 || token.find_first_not_of("0123456789")!=std::string::npos)
            throw std::invalid_argument("five_exact_guild_dungeon_actors_required");
        const auto actor=uint32_t(std::stoul(token));
        if(!actor || !unique.insert(actor).second)
            throw std::invalid_argument("distinct_guild_dungeon_actors_required");
        result.push_back(actor);
        if(end==std::string::npos) {start=text.size();break;}
        start=end+1;
        if(start==text.size())throw std::invalid_argument("trailing_guild_actor_separator");
    }
    if(result.size()!=5 || start!=text.size())
        throw std::invalid_argument("five_exact_guild_dungeon_actors_required");
    return result;
}
}
