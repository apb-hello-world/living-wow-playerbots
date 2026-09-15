#pragma once
#include <boost/property_tree/json_parser.hpp>
#include <set>
#include <sstream>
#include <string>

namespace LivingActivity {
// An unavailable, partial, duplicated or unrelated response is never an ACK.
inline bool ProjectionAcknowledged(const std::string& response,const std::set<std::string>& expected) {
    if(response.empty() || response.size()>8192 || expected.empty() || expected.size()>8)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(response);boost::property_tree::read_json(in,p);
        if(p.get<unsigned>("version")!=1 || p.get<std::string>("status")!="recorded")return false;
        std::set<std::string> actual;
        for(const auto& item:p.get_child("event_ids")) {
            if(!item.first.empty() || !item.second.empty() || !actual.insert(item.second.get_value<std::string>()).second)return false;
        }
        return actual==expected;
    }catch(const std::exception&){return false;}
}
}
