#pragma once
#include "LivingActivity.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <set>

namespace LivingActivity {
// Opening a real gathering node is NOT acquisition. Its cast receipt records
// only the generated native loot and skill state; loot_collect owns item gains.
struct NativeGatherQuote {
    uint32_t actor=0,entry=0,skill=0,spell=0,required=0,value=0,maximum=0,effective=0,money=0,bagCount=0;
    uint64_t source=0;
};
inline bool ValidNativeGatherQuote(const NativeGatherQuote& q) {
    return q.actor && q.entry && q.source && q.required && q.value && q.value<=q.maximum &&
        q.maximum<=65535 && q.effective>=q.required && q.effective<=65535 &&
        ((q.skill==186 && q.spell==2575) || (q.skill==182 && q.spell==2366));
}
inline std::string EncodeNativeGatherQuote(const NativeGatherQuote& q) {
    if(!ValidNativeGatherQuote(q))throw std::invalid_argument("invalid_native_gather_quote");
    return "{\"actor\":"+std::to_string(q.actor)+",\"entry\":"+std::to_string(q.entry)+
        ",\"skill\":"+std::to_string(q.skill)+",\"spell\":"+std::to_string(q.spell)+
        ",\"required\":"+std::to_string(q.required)+",\"value\":"+std::to_string(q.value)+
        ",\"maximum\":"+std::to_string(q.maximum)+",\"effective\":"+std::to_string(q.effective)+
        ",\"money\":"+std::to_string(q.money)+",\"bag_count\":"+std::to_string(q.bagCount)+
        ",\"source\":"+std::to_string(q.source)+'}';
}
inline bool DecodeNativeGatherQuote(const std::string& text,NativeGatherQuote& q) {
    q={};if(text.empty() || text.size()>1024)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        const std::set<std::string> fields{"actor","entry","skill","spell","required","value","maximum","effective","money","bag_count","source"};
        std::set<std::string> seen;
        for(const auto& row:p)if(!fields.count(row.first) || !row.second.empty() || !seen.insert(row.first).second)return false;
        if(seen!=fields)return false;
        auto number=[&](const char* key,uint64_t limit) {
            const auto v=p.get<std::string>(key);
            if(v.empty() || v.size()>20 || (v.size()>1 && v[0]=='0') || v.find_first_not_of("0123456789")!=std::string::npos)
                throw std::invalid_argument("invalid_gather_number");
            const auto n=std::stoull(v);if(n>limit)throw std::invalid_argument("gather_number_overflow");return n;
        };
        q={uint32_t(number("actor",UINT32_MAX)),uint32_t(number("entry",UINT32_MAX)),uint32_t(number("skill",65535)),
            uint32_t(number("spell",UINT32_MAX)),uint32_t(number("required",65535)),uint32_t(number("value",65535)),
            uint32_t(number("maximum",65535)),uint32_t(number("effective",65535)),uint32_t(number("money",UINT32_MAX)),
            uint32_t(number("bag_count",UINT32_MAX)),number("source",UINT64_MAX)};
        return ValidNativeGatherQuote(q);
    } catch(const std::exception&) {q={};return false;}
}
struct NativeGatherResult {
    NativeGatherQuote before;
    uint32_t value=0,maximum=0,money=0,bagCount=0;
    uint64_t generation=0;
    bool started=false,effect=false,finished=false,succeeded=false,owned=false,uncertain=false;
};
inline OperationState VerifyNativeGatherResult(const NativeGatherResult& r,std::string& why) {
    why="native_gather_outcome_uncertain";
    if(!ValidNativeGatherQuote(r.before) || r.uncertain || !r.started || !r.finished)return OperationState::Reconciling;
    if(r.money!=r.before.money || r.bagCount!=r.before.bagCount || r.maximum!=r.before.maximum ||
        r.value<r.before.value || r.value>r.maximum)return OperationState::Reconciling;
    if(r.effect && r.succeeded && r.generation && r.owned) {why="native_gather_opened_loot_not_collected";return OperationState::Verified;}
    if(!r.succeeded && !r.generation && r.value==r.before.value) {why="native_gather_cast_rejected";return OperationState::Rejected;}
    return OperationState::Reconciling;
}
inline std::string EncodeNativeGatherResult(const NativeGatherResult& r) {
    return "{\"before\":"+EncodeNativeGatherQuote(r.before)+",\"value\":"+std::to_string(r.value)+
        ",\"maximum\":"+std::to_string(r.maximum)+",\"money\":"+std::to_string(r.money)+
        ",\"bag_count\":"+std::to_string(r.bagCount)+",\"generation\":"+std::to_string(r.generation)+
        ",\"started\":"+(r.started?"true":"false")+",\"effect\":"+(r.effect?"true":"false")+
        ",\"finished\":"+(r.finished?"true":"false")+",\"succeeded\":"+(r.succeeded?"true":"false")+
        ",\"owned\":"+(r.owned?"true":"false")+",\"uncertain\":"+(r.uncertain?"true":"false")+'}';
}
}
