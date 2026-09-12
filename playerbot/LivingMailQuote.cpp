#include "LivingNativeMailCollection.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {
std::string EncodeNativeMailQuote(const NativeMailQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"mail\":"+std::to_string(q.mail)+",\"guid\":"+std::to_string(q.guid)+
        ",\"entry\":"+std::to_string(q.entry)+",\"quantity\":"+std::to_string(q.quantity)+",\"mailbox\":"+std::to_string(q.mailbox)+
        ",\"mailbox_entry\":"+std::to_string(q.mailboxEntry)+",\"delivered_at\":"+std::to_string(q.deliveredAt)+
        ",\"expires_at\":"+std::to_string(q.expiresAt)+",\"money_before\":"+std::to_string(q.moneyBefore)+
        ",\"mail_money\":"+std::to_string(q.mailMoney)+",\"attachments_before\":"+std::to_string(q.attachmentsBefore)+
        ",\"bag_before\":"+std::to_string(q.bagBefore)+",\"total_before\":"+std::to_string(q.totalBefore)+",\"to\":"+std::to_string(q.to)+
        (q.mergeGuid ? ",\"merge_guid\":"+std::to_string(q.mergeGuid)+",\"merge_count\":"+std::to_string(q.mergeCount) : "")+'}';
}
bool DecodeNativeMailQuote(const std::string& value,NativeMailQuote& q) {
    q={};
    try {
        boost::property_tree::ptree p;std::istringstream input(value);boost::property_tree::read_json(input,p);
        q.actor=p.get<uint32_t>("actor");q.mail=p.get<uint32_t>("mail");q.guid=p.get<uint32_t>("guid");
        q.entry=p.get<uint32_t>("entry");q.quantity=p.get<uint32_t>("quantity");q.mailbox=p.get<uint64_t>("mailbox");
        q.mailboxEntry=p.get<uint32_t>("mailbox_entry");q.deliveredAt=p.get<uint64_t>("delivered_at");
        q.expiresAt=p.get<uint64_t>("expires_at");q.moneyBefore=p.get<uint32_t>("money_before");
        q.mailMoney=p.get<uint32_t>("mail_money");q.attachmentsBefore=p.get<uint32_t>("attachments_before");
        q.bagBefore=p.get<uint32_t>("bag_before");q.totalBefore=p.get<uint32_t>("total_before");q.to=p.get<uint16_t>("to");
        q.mergeGuid=p.get<uint32_t>("merge_guid",0);q.mergeCount=p.get<uint32_t>("merge_count",0);
        return value==EncodeNativeMailQuote(q) && q.actor && q.mail && q.guid && q.entry && q.quantity &&
            q.mailbox && q.mailboxEntry && q.attachmentsBefore && bool(q.mergeGuid)==bool(q.mergeCount) && q.mergeGuid!=q.guid &&
            uint64_t(q.mergeCount)+q.quantity<=UINT32_MAX;
    } catch (...) {q={};return false;}
}
}
