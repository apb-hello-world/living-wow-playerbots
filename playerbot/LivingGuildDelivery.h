#pragma once
#include "LivingActivity.h"
#include "GuildGovernancePolicy.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>

namespace LivingActivity {
// One actor's accepted delivery leg. Native item/stack identities belong to
// resource claims; they may merge or move without rewriting this obligation.
// A recipient gets a distinct leg keyed by the real incoming mail ID. This is
// not permission to send mail, contribute funds or access guild storage.
struct GuildDeliveryJob {
    uint64_t delivery=0;
    uint32_t guild=0,donor=0,entry=0,quantity=0,incomingMail=0;
    std::string goal;
    bool money=false;
};
inline bool ValidGuildDeliveryJob(const GuildDeliveryJob& j) {
    return j.delivery && j.guild && j.donor && j.quantity && livingguild::Id(j.goal) &&
        (j.money ? !j.entry && !j.incomingMail && j.quantity<=uint32_t(INT32_MAX) : j.entry!=0);
}
inline std::string GuildDeliverySourceKey(const GuildDeliveryJob& j,uint32_t actor) {
    if(!ValidGuildDeliveryJob(j) || !actor || (!j.incomingMail && actor!=j.donor))
        throw std::invalid_argument("exact_native_delivery_leg_required");
    return std::to_string(j.delivery)+":"+std::to_string(actor)+":"+std::to_string(j.incomingMail);
}
inline std::string EncodeGuildDeliveryJob(const GuildDeliveryJob& j) {
    if(!ValidGuildDeliveryJob(j)) throw std::invalid_argument("invalid_guild_delivery_job");
    return "{\"workflow\":\"guild_delivery_v1\",\"delivery\":"+std::to_string(j.delivery)+
        ",\"guild\":"+std::to_string(j.guild)+",\"goal\":\""+j.goal+"\",\"donor\":"+std::to_string(j.donor)+
        ",\"entry\":"+std::to_string(j.entry)+",\"quantity\":"+std::to_string(j.quantity)+
        ",\"incoming_mail\":"+std::to_string(j.incomingMail)+",\"money\":"+(j.money?"1":"0")+'}';
}
inline bool DecodeGuildDeliveryJob(const std::string& data,GuildDeliveryJob& job,std::string& blocker) {
    job={};blocker="invalid_guild_delivery_checkpoint";
    if(data.empty() || data.size()>1024) return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
        const std::set<std::string> fields{"workflow","delivery","guild","goal","donor","entry","quantity","incoming_mail","money"};
        std::set<std::string> seen;
        for(const auto& field:p)
            if(!fields.count(field.first) || !field.second.empty() || !seen.insert(field.first).second) return false;
        if(seen!=fields || p.get<std::string>("workflow")!="guild_delivery_v1") return false;
        auto number=[&](const char* key,uint64_t maximum) {
            const auto text=p.get<std::string>(key);
            if(text.empty() || text.size()>20 || (text.size()>1 && text.front()=='0') ||
                text.find_first_not_of("0123456789")!=std::string::npos)
                throw std::invalid_argument("invalid_delivery_number");
            const auto n=std::stoull(text);
            if(n>maximum) throw std::invalid_argument("delivery_number_overflow");
            return n;
        };
        GuildDeliveryJob j;j.delivery=number("delivery",UINT64_MAX);j.guild=uint32_t(number("guild",UINT32_MAX));
        j.donor=uint32_t(number("donor",UINT32_MAX));j.entry=uint32_t(number("entry",UINT32_MAX));
        j.quantity=uint32_t(number("quantity",UINT32_MAX));j.incomingMail=uint32_t(number("incoming_mail",UINT32_MAX));
        j.money=number("money",1)!=0;j.goal=p.get<std::string>("goal");
        if(!ValidGuildDeliveryJob(j)) return false;
        job=j;blocker.clear();return true;
    } catch(const std::exception&) {return false;}
}
inline bool IsManagedGuildDelivery(const Task& task) {
    return task.mode==Mode::Active && (task.source=="guild_delivery" || task.kind==Kind::GuildDelivery);
}
inline bool ValidateGuildDeliveryTask(const Task& task,std::string& blocker) {
    // Existing imported records remain read-only compatibility history. They
    // must not become executable merely by changing mode or acquiring a lease.
    if(!IsManagedGuildDelivery(task)) {blocker.clear();return true;}
    GuildDeliveryJob job;
    if(task.source!="guild_delivery" || task.kind!=Kind::GuildDelivery || task.root!=task.id ||
        !task.parent.empty() || task.priority!=Priority::Delivery || !task.accepted ||
        !DecodeGuildDeliveryJob(task.checkpoint.data,job,blocker) ||
        (!job.incomingMail && task.actor!=job.donor) || !task.actor ||
        task.sourceKey!=GuildDeliverySourceKey(job,task.actor)) {
        blocker="invalid_guild_delivery_task";return false;
    }
    blocker.clear();return true;
}
inline bool PreserveGuildDeliveryIntent(const Task& before,const Task& after,std::string& blocker) {
    if(!ValidateGuildDeliveryTask(after,blocker)) return false;
    if(before.accepted && IsManagedGuildDelivery(before) &&
        (!IsManagedGuildDelivery(after) || before.checkpoint.data!=after.checkpoint.data)) {
        blocker="accepted_guild_delivery_intent_is_immutable";return false;
    }
    blocker.clear();return true;
}
}
