#pragma once
#include "LivingActivity.h"
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace LivingActivity {
// One equipped, actually broken item. No repair-all, bank spending, gear
// replacement, or cheat-money path belongs to this prerequisite.
struct NativeRepairQuote {
    uint32_t actor=0,item=0,entry=0,maximum=0,durability=0,money=0,copper=0;
    uint32_t vendorEntry=0;
    uint64_t vendor=0;
    uint16_t position=0;
};
inline bool NativeRepairPrice(uint32_t loss,uint32_t multiplier,double quality,float discount,uint32_t& copper) {
    copper=0;
    if(!loss || !std::isfinite(quality) || quality<0 || !std::isfinite(discount) || discount<=0 || discount>1 ||
        uint64_t(loss)*multiplier>UINT32_MAX)return false;
    const double base=uint32_t(loss*multiplier)*quality;
    if(base>INT32_MAX)return false;
    // Match the pinned core: truncate base, then apply float reputation
    // discount and truncate again. Native repairs charge at least one copper.
    const auto discounted=uint32_t(base)*discount;
    if(double(discounted)>double(INT32_MAX))return false;
    copper=std::max(1u,uint32_t(discounted));return true;
}
inline bool ValidNativeRepairQuote(const NativeRepairQuote& q) {
    return q.actor && q.item && q.entry && q.maximum && !q.durability && q.money>=q.copper &&
        q.copper && q.copper<=INT32_MAX && q.vendor && q.vendorEntry &&
        (q.position>>8)==255 && (q.position&255)<19;
}
inline std::string EncodeNativeRepairQuote(const NativeRepairQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"item\":"+std::to_string(q.item)+",\"entry\":"+std::to_string(q.entry)+
        ",\"maximum\":"+std::to_string(q.maximum)+",\"durability\":"+std::to_string(q.durability)+
        ",\"money\":"+std::to_string(q.money)+",\"copper\":"+std::to_string(q.copper)+
        ",\"vendor\":"+std::to_string(q.vendor)+",\"vendor_entry\":"+std::to_string(q.vendorEntry)+
        ",\"position\":"+std::to_string(q.position)+'}';
}
inline bool DecodeNativeRepairQuote(const std::string& data,NativeRepairQuote& q) {
    q={};if(data.size()>1024)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
        q.actor=p.get<uint32_t>("actor");q.item=p.get<uint32_t>("item");q.entry=p.get<uint32_t>("entry");
        q.maximum=p.get<uint32_t>("maximum");q.durability=p.get<uint32_t>("durability");
        q.money=p.get<uint32_t>("money");q.copper=p.get<uint32_t>("copper");
        q.vendor=p.get<uint64_t>("vendor");q.vendorEntry=p.get<uint32_t>("vendor_entry");q.position=p.get<uint16_t>("position");
        return ValidNativeRepairQuote(q) && EncodeNativeRepairQuote(q)==data;
    } catch(...) {q={};return false;}
}
inline bool VerifyNativeRepair(const NativeRepairQuote& q,uint32_t item,uint32_t entry,uint16_t position,
    uint32_t maximum,uint32_t durability,uint32_t money) {
    return ValidNativeRepairQuote(q) && item==q.item && entry==q.entry && position==q.position &&
        maximum==q.maximum && durability==q.maximum && money==q.money-q.copper;
}
inline bool RepairPrerequisiteStep(const std::string& step) {
    return step=="maintenance_service_repair" || step=="maintenance_repair_prepare" || step=="maintenance_repair";
}
}
