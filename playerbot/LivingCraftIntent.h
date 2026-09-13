#pragma once
#include "LivingCraftCapture.h"
#include <boost/property_tree/json_parser.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace LivingActivity {
// Versioned by the presence of inventory_before. Legacy intents stay readable;
// only new native preparation may supply this exact pre-operation snapshot.
// Claimed quantities are not necessarily the entire merged physical stack.
namespace CraftIntentCodec {
    using Tree=boost::property_tree::ptree;
    inline void Object(const Tree& p,std::initializer_list<const char*> names) {
        if (!p.data().empty() || p.size()!=names.size()) throw std::invalid_argument("craft_intent_object_invalid");
        for (const auto* name:names) if(p.count(name)!=1) throw std::invalid_argument("craft_intent_field_invalid");
    }
    inline uint32_t Number(const Tree& p) {
        const auto& s=p.data();
        if(!p.empty() || s.empty() || s.size()>10 || (s.size()>1 && s.front()=='0') ||
            s.find_first_not_of("0123456789")!=std::string::npos) throw std::invalid_argument("craft_intent_number_invalid");
        const auto n=std::stoull(s);
        if(n>UINT32_MAX)throw std::invalid_argument("craft_intent_number_overflow");
        return uint32_t(n);
    }
    inline CraftFrame Frame(uint32_t actor,const Tree& p) {
        Object(p,{"skill","money","stacks"});CraftFrame f;f.actor=actor;
        f.skill=Number(p.get_child("skill"));f.money=Number(p.get_child("money"));
        const auto& stacks=p.get_child("stacks");
        if(!stacks.data().empty() || stacks.size()>32 || f.skill>UINT16_MAX)
            throw std::invalid_argument("craft_intent_frame_bound");
        for(const auto& row:stacks) {
            if(!row.first.empty() || !row.second.data().empty() || row.second.size()!=5)
                throw std::invalid_argument("craft_intent_stack_invalid");
            uint32_t n[5];unsigned i=0;
            for(const auto& value:row.second) {
                if(!value.first.empty())throw std::invalid_argument("craft_intent_stack_invalid");
                n[i++]=Number(value.second);
            }
            if(n[4]>255)throw std::invalid_argument("craft_intent_slot_invalid");
            f.stacks.push_back({actor,n[0],n[1],n[2],n[3],uint8_t(n[4])});
        }
        if(!ValidCraftFrame(f))throw std::invalid_argument("craft_intent_frame_invalid");
        return f;
    }
    inline std::optional<CraftFrame> OptionalFrame(const Tree& p,uint32_t actor,uint32_t skill,uint32_t money) {
        if(!p.count("inventory_before"))return {};
        if(p.count("inventory_before")!=1)throw std::invalid_argument("craft_intent_frame_duplicate");
        auto f=Frame(actor,p.get_child("inventory_before"));
        if(f.skill!=skill || f.money!=money)throw std::invalid_argument("craft_intent_frame_state_mismatch");
        return f;
    }
}
inline std::string EncodeCraftFrame(const CraftFrame& frame) {
    if(!ValidCraftFrame(frame) || frame.stacks.size()>32 || frame.skill>UINT16_MAX)
        throw std::invalid_argument("craft_intent_frame_invalid");
    std::string s="{\"skill\":"+std::to_string(frame.skill)+",\"money\":"+std::to_string(frame.money)+",\"stacks\":[";
    for(const auto& item:frame.stacks) {
        if(s.back()!='[')s+=',';
        s+='['+std::to_string(item.guid)+','+std::to_string(item.entry)+','+std::to_string(item.count)+','+
            std::to_string(item.bagGuid)+','+std::to_string(item.slot)+']';
    }
    return s+"]}";
}
inline std::string EncodeCraftIntent(const ProfessionJob& job,const CraftFrame& frame) {
    return "{\"recipe\":"+std::to_string(job.recipe)+",\"skill\":"+std::to_string(frame.skill)+
        ",\"money\":"+std::to_string(frame.money)+",\"inventory_before\":"+EncodeCraftFrame(frame)+'}';
}
struct ProfessionCastIntent {
    uint32_t skill=0,money=0;
    std::optional<CraftFrame> inventoryBefore;
};
inline bool DecodeCraftIntent(uint32_t actor,const ProfessionJob& job,const std::string& json,
    ProfessionCastIntent& result,std::string& blocker) {
    result={};
    try {
        if(json.empty() || json.size()>4096)throw std::invalid_argument("craft_intent_bound");
        CraftIntentCodec::Tree p;std::istringstream input(json);boost::property_tree::read_json(input,p);
        if(p.count("inventory_before"))CraftIntentCodec::Object(p,{"recipe","skill","money","inventory_before"});
        else CraftIntentCodec::Object(p,{"recipe","skill","money"});
        ProfessionCastIntent value;value.skill=CraftIntentCodec::Number(p.get_child("skill"));
        value.money=CraftIntentCodec::Number(p.get_child("money"));
        if(!actor || !value.skill || value.skill>UINT16_MAX || CraftIntentCodec::Number(p.get_child("recipe"))!=job.recipe)
            throw std::invalid_argument("craft_intent_identity_invalid");
        value.inventoryBefore=CraftIntentCodec::OptionalFrame(p,actor,value.skill,value.money);
        result=std::move(value);blocker.clear();return true;
    } catch(const std::exception&) {blocker="native_craft_intent_invalid";return false;}
}
}
