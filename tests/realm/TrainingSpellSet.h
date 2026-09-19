#pragma once
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <limits>
// A native unordered spell container may enumerate differently after login.
// Compare exact unique IDs; reject malformed/duplicate fixture evidence.
inline bool SameTrainingSpellSet(const std::string& left,const std::string& right) {
    auto parse=[](const std::string& text,std::set<uint32_t>& ids) {
        if(text.empty() || text.back()==',')return false;
        std::istringstream input(text);std::string token;
        while(std::getline(input,token,',')) {
            if(token.empty() || token.size()>10 || token.find_first_not_of("0123456789")!=std::string::npos)return false;
            const auto id=std::stoull(token);
            if(!id || id>std::numeric_limits<uint32_t>::max() || !ids.insert(uint32_t(id)).second)return false;
        }
        return !ids.empty();
    };
    std::set<uint32_t> a,b;return parse(left,a) && parse(right,b) && a==b;
}
