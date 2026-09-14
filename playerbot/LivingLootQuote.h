#pragma once
#include "LivingActivity.h"
#include <boost/property_tree/json_parser.hpp>
#include <set>
#include <sstream>
#include <tuple>

namespace LivingActivity {
// One ALREADY GENERATED native world-loot slot. This is not a loot table,
// expected drop, virtual item, or permission to generate a node's contents.
// The generation is the native Loot creation timestamp within the saved boot.
struct NativeLootQuote {
    uint32_t actor=0,entry=0,quantity=0,slot=0,type=0,money=0,bagCount=0;
    uint64_t source=0,generation=0;
};
inline bool ValidNativeLootQuote(const NativeLootQuote& q) {
    return q.actor && q.entry && q.quantity && q.quantity<=255 && q.slot<=255 &&
        q.type && q.type<=255 && q.source && q.generation &&
        uint64_t(q.bagCount)+q.quantity<=UINT32_MAX;
}
inline std::string EncodeNativeLootQuote(const NativeLootQuote& q) {
    if(!ValidNativeLootQuote(q))throw std::invalid_argument("exact_native_loot_slot_required");
    return "{\"actor\":"+std::to_string(q.actor)+",\"source\":"+std::to_string(q.source)+
        ",\"generation\":"+std::to_string(q.generation)+",\"slot\":"+std::to_string(q.slot)+
        ",\"type\":"+std::to_string(q.type)+",\"entry\":"+std::to_string(q.entry)+
        ",\"quantity\":"+std::to_string(q.quantity)+",\"money\":"+std::to_string(q.money)+
        ",\"bag_count\":"+std::to_string(q.bagCount)+'}';
}
namespace LootQuoteDetail {
inline uint64_t Number(const boost::property_tree::ptree& p,const char* name,uint64_t maximum) {
    const auto text=p.get<std::string>(name);
    if(text.empty() || text.size()>20 || (text.size()>1 && text.front()=='0') ||
        text.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("invalid_loot_number");
    const auto value=std::stoull(text);
    if(value>maximum)throw std::invalid_argument("loot_number_overflow");
    return value;
}
inline bool Fields(const boost::property_tree::ptree& p,std::set<std::string> fields,const char* nested="") {
    if(!p.data().empty())return false;
    for(const auto& f:p)if(!fields.erase(f.first) || (f.first!=nested && !f.second.empty()))return false;
    return fields.empty();
}
inline NativeLootQuote Quote(const boost::property_tree::ptree& p) {
    if(!Fields(p,{"actor","source","generation","slot","type","entry","quantity","money","bag_count"}))
        throw std::invalid_argument("invalid_loot_fields");
    NativeLootQuote q;
    q.actor=Number(p,"actor",UINT32_MAX);q.source=Number(p,"source",UINT64_MAX);
    q.generation=Number(p,"generation",UINT64_MAX);q.slot=Number(p,"slot",255);
    q.type=Number(p,"type",255);q.entry=Number(p,"entry",UINT32_MAX);
    q.quantity=Number(p,"quantity",255);q.money=Number(p,"money",UINT32_MAX);
    q.bagCount=Number(p,"bag_count",UINT32_MAX);
    if(!ValidNativeLootQuote(q))throw std::invalid_argument("invalid_native_loot_quote");
    return q;
}
inline bool Boolean(const boost::property_tree::ptree& p,const char* name) {
    const auto value=p.get<std::string>(name);
    if(value!="true" && value!="false")throw std::invalid_argument("invalid_loot_boolean");
    return value=="true";
}
}
inline bool DecodeNativeLootQuote(const std::string& json,NativeLootQuote& q) {
    q={};if(json.empty() || json.size()>1024)return false;
    try {boost::property_tree::ptree p;std::istringstream in(json);boost::property_tree::read_json(in,p);
        q=LootQuoteDetail::Quote(p);return true;
    }catch(const std::exception&){return false;}
}
struct NativeLootResult {
    NativeLootQuote before;
    uint32_t money=0,bagCount=0;
    bool dispatched=false,slotConsumed=false;
};
inline std::string EncodeNativeLootResult(const NativeLootResult& r) {
    return "{\"before\":"+EncodeNativeLootQuote(r.before)+",\"money\":"+std::to_string(r.money)+
        ",\"bag_count\":"+std::to_string(r.bagCount)+",\"dispatched\":"+(r.dispatched?"true":"false")+
        ",\"slot_consumed\":"+(r.slotConsumed?"true":"false")+'}';
}
inline bool DecodeNativeLootResult(const std::string& json,NativeLootResult& r) {
    r={};if(json.empty() || json.size()>2048)return false;
    try {boost::property_tree::ptree p;std::istringstream in(json);boost::property_tree::read_json(in,p);
        if(!LootQuoteDetail::Fields(p,{"before","money","bag_count","dispatched","slot_consumed"},"before"))return false;
        r.before=LootQuoteDetail::Quote(p.get_child("before"));
        r.money=LootQuoteDetail::Number(p,"money",UINT32_MAX);r.bagCount=LootQuoteDetail::Number(p,"bag_count",UINT32_MAX);
        r.dispatched=LootQuoteDetail::Boolean(p,"dispatched");r.slotConsumed=LootQuoteDetail::Boolean(p,"slot_consumed");
        return true;
    }catch(const std::exception&){return false;}
}
inline OperationState VerifyNativeLootResult(const NativeLootResult& r,std::string& why) {
    if(!ValidNativeLootQuote(r.before)) {why="native_loot_snapshot_invalid";return OperationState::Reconciling;}
    if(r.dispatched && r.slotConsumed && r.money==r.before.money &&
        uint64_t(r.before.bagCount)+r.before.quantity==r.bagCount) {
        why="native_loot_slot_and_items_observed";return OperationState::Verified;
    }
    if(!r.slotConsumed && r.money==r.before.money && r.bagCount==r.before.bagCount) {
        why="native_loot_rejected_without_effect";return OperationState::Rejected;
    }
    why="native_loot_partial_effect_requires_reconciliation";return OperationState::Reconciling;
}
}
