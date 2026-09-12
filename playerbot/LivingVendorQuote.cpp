#include "LivingVendorQuote.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>

namespace LivingActivity {
std::string EncodeNativeVendorQuote(const NativeVendorQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"vendor\":"+std::to_string(q.vendor)+
        ",\"vendor_entry\":"+std::to_string(q.vendorEntry)+",\"entry\":"+std::to_string(q.entry)+
        ",\"quantity\":"+std::to_string(q.quantity)+",\"buy_units\":"+std::to_string(q.buyUnits)+
        ",\"copper\":"+std::to_string(q.copper)+",\"money\":"+std::to_string(q.moneyBefore)+'}';
}
bool DecodeNativeVendorQuote(const std::string& value,NativeVendorQuote& quote) {
    quote={};
    if(value.empty() || value.size()>1024)return false;
    try {
        NativeVendorQuote q;boost::property_tree::ptree fields;std::istringstream input(value);
        boost::property_tree::read_json(input,fields);
        q.actor=fields.get<uint32_t>("actor");q.vendor=fields.get<uint64_t>("vendor");
        q.vendorEntry=fields.get<uint32_t>("vendor_entry");q.entry=fields.get<uint32_t>("entry");
        q.quantity=fields.get<uint32_t>("quantity");q.buyUnits=fields.get<uint32_t>("buy_units");
        q.copper=fields.get<uint32_t>("copper");q.moneyBefore=fields.get<uint32_t>("money");
        if(!q.actor || !q.vendor || !q.vendorEntry || !q.entry || !q.quantity || !q.buyUnits || q.buyUnits>255 ||
            q.quantity%q.buyUnits || !q.copper || q.copper>uint32_t(INT32_MAX) || q.moneyBefore<q.copper ||
            EncodeNativeVendorQuote(q)!=value)return false;
        quote=q;return true;
    } catch(const std::exception&) {return false;}
}
}
