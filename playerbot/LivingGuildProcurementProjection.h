#pragma once
#include "LivingGuildProcurement.h"
#include "LivingActivityResources.h"
#include <algorithm>

namespace LivingActivity {
// Acknowledged work and resource CLAIMS, never native bank stock or delivery
// receipts. Subtotals are disjoint and bounded by each accepted assignment;
// a vendor bundle's personal surplus must not inflate requested quantities.
struct GuildProcurementStatus {
    bool ready=false;
    uint32_t assigned=0,carried=0,banked=0,mail=0;
    uint64_t updatedAtMs=0;
    std::string phase="queued",blocker="procurement_snapshot_unavailable";
};
class GuildProcurementProjection {
public:
    GuildProcurementProjection(uint32_t guild,uint32_t entry,const std::string& goal)
        :guild_(guild),entry_(entry),goal_(goal) {}
    bool Matches(const Task& task,GuildProcurementJob& job,std::string& why) const {
        if(!IsGuildProcurementTask(task) || !task.accepted || task.mode!=Mode::Active || Terminal(task.phase)) {
            why.clear();return false;
        }
        if(!ValidateGuildProcurementTask(task,why) || !DecodeGuildProcurementJob(task.checkpoint.data,job,why))return false;
        why.clear();return job.guild==guild_ && job.entry==entry_ && job.goal==goal_;
    }
    bool Add(const Task& task,const UnsettledClaimBatch& claims,const std::string& executionBlocker,std::string& why) {
        GuildProcurementJob job;
        if(!Matches(task,job,why))return why.empty();
        auto reject=[&](const char* reason){why=reason;return false;};
        if(!IsUuid(task.id) || !task.revision || !tasks_.insert(task.id).second || !claims.complete ||
            !claims.bookRevision || claims.claims.size()>16)
            return reject("procurement_snapshot_incomplete");
        uint64_t bags=0,bank=0,mail=0;std::set<std::string> ids;
        for(const auto& claim:claims.claims) {
            if(!ValidResourceClaim(claim) || claim.task!=task.id || claim.actor!=task.actor || !ids.insert(claim.id).second)
                return reject("procurement_claim_snapshot_invalid");
            if(claim.state!="held")return reject("procurement_claim_reconciliation_pending");
            if(claim.itemEntry!=job.entry)continue; // Money and capacity prerequisites are not requested goods.
            auto add=[&](uint64_t& value){value=std::min<uint64_t>(job.quantity,value+std::min<uint64_t>(job.quantity,claim.quantity));};
            if(claim.location=="bags")add(bags);
            else if(claim.location=="bank")add(bank);
            else if(claim.location=="mail")add(mail);
            else return reject("procurement_claim_location_unavailable");
        }
        if(uint64_t(value_.assigned)+job.quantity>UINT32_MAX)return reject("procurement_snapshot_quantity_limit");
        const uint32_t carried=uint32_t(bags),banked=uint32_t(std::min<uint64_t>(bank,job.quantity-carried));
        value_.assigned+=job.quantity;value_.carried+=carried;value_.banked+=banked;
        value_.mail+=uint32_t(std::min<uint64_t>(mail,job.quantity-carried-banked));
        // One deterministic example is a summary, not a claim that every donor
        // shares its state. All quantities still include every matching task.
        if(representative_.empty() || std::make_pair(task.createdAtMs,task.id)<representativeOrder_) {
            representative_=task.id;representativeOrder_={task.createdAtMs,task.id};
            value_.phase=Name(task.phase);
            value_.blocker=executionBlocker.empty()?task.checkpoint.blocker:executionBlocker;
        }
        value_.updatedAtMs=std::max(value_.updatedAtMs,task.updatedAtMs);why.clear();return true;
    }
    GuildProcurementStatus Finish() const {
        auto result=value_;result.ready=true;
        if(!result.assigned){result.phase="none";result.blocker.clear();}
        return result;
    }
private:
    uint32_t guild_,entry_;std::string goal_,representative_;
    std::pair<uint64_t,std::string> representativeOrder_;
    std::set<std::string> tasks_;GuildProcurementStatus value_;
};
}
