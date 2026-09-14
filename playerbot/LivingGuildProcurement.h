#pragma once
#include "LivingActivity.h"
#include "LivingProfessionJob.h"
#include "GuildGovernancePolicy.h"
#include "GuildSupplyPolicy.h"
#include <boost/property_tree/json_parser.hpp>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>
#include <tuple>

namespace LivingActivity {
// Accepted demand, NOT a parcel or evidence of possession. Native acquisitions
// keep their own item/money claims and receipts. Only a verified custody handoff
// may create a guild_delivery record. No profession recipe is synthesized.
struct GuildProcurementJob {
    uint32_t guild=0,donor=0,entry=0,quantity=0;
    std::string goal,batch;
    // Optional exact recipe under this SAME root. V1 jobs remain byte-for-byte
    // compatible. This is finite native preparation, never a competing task.
    std::string craft={};
    uint64_t craftFinishedRevision=0;
};
inline bool ValidGuildProcurementJob(const GuildProcurementJob& job) {
    if(!job.guild || !job.donor || !job.entry || !job.quantity || !livingguild::Id(job.goal) || !IsUuid(job.batch))return false;
    if(job.craft.empty())return !job.craftFinishedRevision;
    // Only a plain profession workflow may be nested. Check before calling its
    // decoder so malicious recursive procurement envelopes cannot recurse.
    try {
        boost::property_tree::ptree p;std::istringstream in(job.craft);boost::property_tree::read_json(in,p);
        if(p.get<std::string>("workflow")!="profession_job_v1")return false;
    } catch(const std::exception&) {return false;}
    ProfessionWorkflow flow;std::string why;
    return DecodeProfessionWorkflow(job.craft,flow,why) &&
        flow.intent.operation==ProfessionOperation::CreateItem && flow.intent.purpose==ProfessionPurpose::RequestedItem &&
        flow.intent.outputEntry==job.entry && flow.intent.outputQuantity==job.quantity &&
        std::none_of(flow.intent.reagents.begin(),flow.intent.reagents.end(),[&](const auto& r){return r.entry==job.entry;}) &&
        (!job.craftFinishedRevision || flow.tools.empty() ||
            (flow.tools.back().finishedRevision && flow.tools.back().finishedRevision<job.craftFinishedRevision));
}
inline std::string GuildProcurementSourceKey(const GuildProcurementJob& job) {
    if(!ValidGuildProcurementJob(job))throw std::invalid_argument("invalid_guild_procurement_job");
    return job.goal+":"+std::to_string(job.donor)+":"+job.batch;
}
inline std::string EncodeGuildProcurementJob(const GuildProcurementJob& job) {
    if(!ValidGuildProcurementJob(job))throw std::invalid_argument("invalid_guild_procurement_job");
    auto result="{\"workflow\":\""+std::string(job.craft.empty()?"guild_procurement_v1":"guild_procurement_v2")+"\",\"guild\":"+std::to_string(job.guild)+
        ",\"donor\":"+std::to_string(job.donor)+",\"entry\":"+std::to_string(job.entry)+
        ",\"quantity\":"+std::to_string(job.quantity)+",\"goal\":\""+job.goal+"\",\"batch\":\""+job.batch+"\"";
    if(!job.craft.empty())result+=",\"craft\":"+job.craft+",\"craft_finished_revision\":"+std::to_string(job.craftFinishedRevision);
    result+='}';
    if(result.size()>8192)throw std::invalid_argument("guild_procurement_checkpoint_bound");
    return result;
}
inline bool DecodeGuildProcurementJob(const std::string& data,GuildProcurementJob& job,std::string& blocker) {
    job={};blocker="invalid_guild_procurement_checkpoint";
    if(data.empty() || data.size()>8192)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
        const auto workflow=p.get<std::string>("workflow");
        const bool crafting=workflow=="guild_procurement_v2";
        std::set<std::string> fields{"workflow","guild","donor","entry","quantity","goal","batch"};
        if(crafting){fields.insert("craft");fields.insert("craft_finished_revision");}
        std::set<std::string> seen;
        for(const auto& field:p)
            if(!fields.count(field.first) || (field.first!="craft" && !field.second.empty()) || !seen.insert(field.first).second)return false;
        if(seen!=fields || (!crafting && workflow!="guild_procurement_v1"))return false;
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
        if(crafting) {
            const auto& nested=p.get_child("craft");
            if(nested.empty() || !nested.data().empty() || nested.get<std::string>("workflow")!="profession_job_v1")return false;
            std::ostringstream out;boost::property_tree::write_json(out,nested,false);parsed.craft=out.str();
            const auto revision=p.get<std::string>("craft_finished_revision");
            if(revision.empty() || revision.size()>20 || revision.find_first_not_of("0123456789")!=std::string::npos)return false;
            parsed.craftFinishedRevision=std::stoull(revision);
        }
        if(!ValidGuildProcurementJob(parsed))return false;
        job=parsed;blocker.clear();return true;
    } catch(const std::exception&) {return false;}
}
inline bool IsGuildProcurementTask(const Task& task) {
    return task.source=="guild_procurement" || task.kind==Kind::GuildProcurement;
}
inline bool IsGuildCraftTask(const Task& task) {
    if(!IsGuildProcurementTask(task))return false;
    GuildProcurementJob job;std::string why;
    return DecodeGuildProcurementJob(task.checkpoint.data,job,why) && !job.craft.empty();
}
inline bool ValidateGuildProcurementTask(const Task& task,std::string& blocker) {
    if(!IsGuildProcurementTask(task)){blocker.clear();return true;}
    GuildProcurementJob job;
    if(task.source!="guild_procurement" || task.kind!=Kind::GuildProcurement || task.root!=task.id ||
        !task.parent.empty() || !task.accepted || task.priority!=Priority::Delivery ||
        !DecodeGuildProcurementJob(task.checkpoint.data,job,blocker) || task.actor!=job.donor ||
        task.sourceKey!=GuildProcurementSourceKey(job) || job.craftFinishedRevision>task.revision) {
        blocker="invalid_guild_procurement_task";return false;
    }
    blocker.clear();return true;
}
inline bool PreserveGuildProcurementIntent(const Task& before,const Task& after,std::string& blocker) {
    if(!ValidateGuildProcurementTask(after,blocker))return false;
    if(before.accepted && IsGuildProcurementTask(before)) {
        GuildProcurementJob oldJob,newJob;
        if(!IsGuildProcurementTask(after) || !DecodeGuildProcurementJob(before.checkpoint.data,oldJob,blocker) ||
            !DecodeGuildProcurementJob(after.checkpoint.data,newJob,blocker) ||
            std::tie(oldJob.guild,oldJob.donor,oldJob.entry,oldJob.quantity,oldJob.goal,oldJob.batch)!=
            std::tie(newJob.guild,newJob.donor,newJob.entry,newJob.quantity,newJob.goal,newJob.batch) ||
            oldJob.craft.empty()!=newJob.craft.empty()) {
            blocker="accepted_guild_procurement_intent_is_immutable";return false;
        }
        if(!oldJob.craft.empty() && !PreserveProfessionIntent(before,after,blocker))return false;
        if(oldJob.craftFinishedRevision!=newJob.craftFinishedRevision &&
            (oldJob.craftFinishedRevision || newJob.craftFinishedRevision!=after.revision ||
             after.revision!=before.revision+1 || before.phase!=Phase::Verifying || after.phase!=Phase::Preparing ||
             after.checkpoint.step!="guild_procurement_craft_ready")) {
            blocker="guild_procurement_craft_handoff_receipt_required";return false;
        }
    }
    blocker.clear();return true;
}
inline std::string GuildProcurementCraftReceiptGuard(const GuildProcurementJob& job) {
    if(job.craft.empty())return {};
    if(!job.craftFinishedRevision)return " AND 1=0";
    return " AND EXISTS(SELECT 1 FROM living_activity_transition r WHERE r.task_id=living_activity_task.task_id"
        " AND r.task_revision="+std::to_string(job.craftFinishedRevision)+" AND r.code='guild_procurement_craft_verified')";
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
struct GuildProcurementGoalSnapshot {
    uint32_t target=0,bankReserved=0,banked=0,nativeTransit=0;
};
// Admission-time view of accepted tasks, then their pending replacements.
// A pending transition replaces the same task, not a second assignment. Count
// across goals for the same guild/item, matching the native shared-bank target
// semantics. No physical stock is inferred from any task phase.
class GuildProcurementCoverage {
public:
    GuildProcurementCoverage(uint32_t guild,uint32_t entry,const std::string& excluded):guild_(guild),entry_(entry),excluded_(excluded) {}
    bool Add(const Task& task,std::string& blocker) {
        if(!IsGuildProcurementTask(task))return true;
        GuildProcurementJob job;
        if(!ValidateGuildProcurementTask(task,blocker) || !DecodeGuildProcurementJob(task.checkpoint.data,job,blocker))return false;
        if(task.id==excluded_ || job.guild!=guild_ || job.entry!=entry_)return true;
        if(!IsUuid(task.id) || !task.revision){blocker="guild_procurement_coverage_identity_invalid";return false;}
        const uint32_t quantity=task.mode==Mode::Active && !Terminal(task.phase)?job.quantity:0;
        auto prior=rows_.find(task.id);
        if(prior!=rows_.end() && (task.revision<prior->second.first ||
            (task.revision==prior->second.first && quantity!=prior->second.second))) {
            blocker="guild_procurement_coverage_revision_conflict";return false;
        }
        if(prior==rows_.end() && rows_.size()>=20000){blocker="guild_procurement_coverage_unbounded";return false;}
        rows_[task.id]={task.revision,quantity};return true;
    }
    bool AddPending(const Task& task,std::string& blocker) {
        // Proposed terminal state is NOT a receipt. Until its journal commits,
        // nativeTransit still excludes its parcels; keep the original accepted
        // coverage to avoid a second donor ordering during that handoff gap.
        if(IsGuildProcurementTask(task) && Terminal(task.phase)) {
            if(!ValidateGuildProcurementTask(task,blocker))return false;
            const auto prior=rows_.find(task.id);
            if(prior!=rows_.end() && prior->second.second) {
                if(task.revision<=prior->second.first){blocker="guild_procurement_coverage_revision_conflict";return false;}
                blocker.clear();return true;
            }
        }
        return Add(task,blocker);
    }
    uint64_t Assigned() const {uint64_t total=0;for(const auto& row:rows_)total+=row.second.second;return total;}
private:
    uint32_t guild_,entry_;
    std::string excluded_;
    std::map<std::string,std::pair<uint64_t,uint32_t>> rows_;
};
}
