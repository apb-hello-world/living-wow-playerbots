#pragma once
#include <boost/property_tree/ptree.hpp>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

namespace LivingActivity {
// Routing evidence only. The existing commission agreement remains the owner
// of goods, recipient and payment; this record cannot certify delivery.
struct CommissionMeetingState {
    uint32_t map=0,instance=0,attempts=0;
    float x=0,y=0,z=0,bestDistance=0;
    uint64_t noProgressMs=0;
};
inline boost::property_tree::ptree EncodeCommissionMeetingState(const CommissionMeetingState& s) {
    boost::property_tree::ptree p;
    p.put("version",1);p.put("map",s.map);p.put("instance",s.instance);
    p.put("x",s.x);p.put("y",s.y);p.put("z",s.z);p.put("best_distance",s.bestDistance);
    p.put("no_progress_ms",s.noProgressMs);p.put("attempts",s.attempts);return p;
}
inline bool DecodeCommissionMeetingState(const boost::property_tree::ptree& p,CommissionMeetingState& s) {
    const std::set<std::string> fields{"version","map","instance","x","y","z","best_distance","no_progress_ms","attempts"};
    std::set<std::string> seen;
    try {
        for(const auto& f:p)if(!fields.count(f.first) || !seen.insert(f.first).second || !f.second.empty())return false;
        if(seen!=fields)return false;
        auto number=[&](const char* key,uint64_t limit) {
            const auto v=p.get<std::string>(key);
            if(v.empty() || v.size()>20 || v.find_first_not_of("0123456789")!=std::string::npos ||
                (v.size()>1 && v[0]=='0'))throw std::invalid_argument("meeting_number");
            const auto n=std::stoull(v);if(n>limit)throw std::invalid_argument("meeting_number");return n;
        };
        if(number("version",1)!=1)return false;
        s.map=uint32_t(number("map",UINT32_MAX));s.instance=uint32_t(number("instance",UINT32_MAX));
        s.attempts=uint32_t(number("attempts",100));s.noProgressMs=number("no_progress_ms",120000);
        s.x=p.get<float>("x");s.y=p.get<float>("y");s.z=p.get<float>("z");s.bestDistance=p.get<float>("best_distance");
        return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) &&
            std::abs(s.x)<=100000 && std::abs(s.y)<=100000 && std::abs(s.z)<=100000 &&
            std::isfinite(s.bestDistance) && s.bestDistance>=0 && s.bestDistance<=300000;
    }catch(const std::exception&){return false;}
}
}
