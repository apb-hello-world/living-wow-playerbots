#pragma once
#include "LivingActivity.h"
#include "GuildGovernancePolicy.h"
#include "GuildSupplyPolicy.h"
#include <boost/property_tree/json_parser.hpp>
#include <set>
#include <sstream>

namespace LivingActivity {
// Accepted demand, NOT a parcel or evidence of possession. Native acquisitions
// keep their own item/money claims and receipts. Only a verified custody handoff
// may create a guild_delivery record. No profession recipe is synthesized.
struct GuildProcurementJob {
    uint32_t guild=0,donor=0,entry=0,quantity=0;
    std::string goal,batch;
};
inline bool ValidGuildProcurementJob(const GuildProcurementJob& job) {
    return job.guild && job.donor && job.entry && job.quantity && livingguild::Id(job.goal) && IsUuid(job.batch);
}
inline std::string GuildProcurementSourceKey(const GuildProcurementJob& job) {
    if(!ValidGuildProcurementJob(job))throw std::invalid_argument("invalid_guild_procurement_job");
    return job.goal+":"+std::to_string(job.donor)+":"+job.batch;
}
inline std::string EncodeGuildProcurementJob(const GuildProcurementJob& job) {
    if(!ValidGuildProcurementJob(job))throw std::invalid_argument("invalid_guild_procurement_job");
    return "{\"workflow\":\"guild_procurement_v1\",\"guild\":"+std::to_string(job.guild)+
        ",\"donor\":"+std::to_string(job.donor)+",\"entry\":"+std::to_string(job.entry)+
        ",\"quantity\":"+std::to_string(job.quantity)+",\"goal\":\""+job.goal+"\",\"batch\":\""+job.batch+"\"}";
}
inline bool DecodeGuildProcurementJob(const std::string& data,GuildProcurementJob& job,std::string& blocker) {
    job={};blocker="invalid_guild_procurement_checkpoint";
    if(data.empty() || data.size()>1024)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
        const std::set<std::string> fields{"workflow","guild","donor","entry","quantity","goal","batch"};
        std::set<std::string> seen;
        for(const auto& field:p)
            if(!fields.count(field.first) || !field.second.empty() || !seen.insert(field.first).second)return false;
        if(seen!=fields || p.get<std::string>("workflow")!="guild_procurement_v1")return false;
        auto number=[&](const char* key) {
            const auto text=p.get<std::string>(key);
            if(text.empty() || text.size()>10 || text.front()=='0' || text.find_first_not_of("0123456789")!=std::string::npos)
                throw std::invalid_argument("invalid_procurement_number");
            const auto n=std::stoull(text);
            if(n>UINT32_MAX)throw std::invalid_argument("procurement_number_overflow");
            return uint32_t(n);
        };
        GuildProcurementJob parsed{number("guild"),number("donor"),number("entry"),number("quantity"),
            p.get<std::string>("goal"),p.get<std::string>("batch")};
        if(!ValidGuildProcurementJob(parsed))return false;
        job=parsed;blocker.clear();return true;
    } catch(const std::exception&) {return false;}
}
inline bool IsGuildProcurementTask(const Task& task) {
    return task.source=="guild_procurement" || task.kind==Kind::GuildProcurement;
}
inline bool ValidateGuildProcurementTask(const Task& task,std::string& blocker) {
    if(!IsGuildProcurementTask(task)){blocker.clear();return true;}
    GuildProcurementJob job;
    if(task.source!="guild_procurement" || task.kind!=Kind::GuildProcurement || task.root!=task.id ||
        !task.parent.empty() || !task.accepted || task.priority!=Priority::Delivery ||
        !DecodeGuildProcurementJob(task.checkpoint.data,job,blocker) || task.actor!=job.donor ||
        task.sourceKey!=GuildProcurementSourceKey(job)) {
        blocker="invalid_guild_procurement_task";return false;
    }
    blocker.clear();return true;
}
inline bool PreserveGuildProcurementIntent(const Task& before,const Task& after,std::string& blocker) {
    if(!ValidateGuildProcurementTask(after,blocker))return false;
    if(before.accepted && IsGuildProcurementTask(before) &&
        (!IsGuildProcurementTask(after) || before.checkpoint.data!=after.checkpoint.data)) {
        blocker="accepted_guild_procurement_intent_is_immutable";return false;
    }
    blocker.clear();return true;
}
// The native bank reservation reduces usable bank stock. Accepted assignments
// reduce NEW assignments, without inflating displayed native delivery counts.
// An assignment continues covering its goods after purchase/collection until
// the native delivery ledger actually takes custody. The transactional handoff
// moves that coverage to nativeTransit, exactly once. Call on admission or
// acknowledged transitions with a complete snapshot, including pending writes.
inline uint32_t UnassignedGuildProcurement(uint32_t target,uint32_t bank,uint32_t bankReserved,
    uint32_t nativeTransit,uint64_t assignedNotInNativeDelivery) {
    const auto missing=livingguild::SupplyOutstanding(target,bank,bankReserved,nativeTransit);
    return assignedNotInNativeDelivery>=missing?0:missing-uint32_t(assignedNotInNativeDelivery);
}
}
