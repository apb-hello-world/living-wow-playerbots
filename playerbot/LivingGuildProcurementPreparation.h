#pragma once
#include "LivingGuildProcurement.h"
#include "LivingActivityResources.h"
#include <algorithm>
#include <map>

namespace LivingActivity {
// Reservation bookkeeping only. Candidates are actual, unshared bag stacks
// inspected by the native adapter. Paid mail/banked claims remain obligations;
// they are not counted as carried goods or released to make a plan look ready.
struct GuildProcurementMaterialPlan {
    std::vector<ClaimReceiptChange> changes; // New claims receive an ID at admission.
    bool carriedReady=false;
    uint64_t carried=0,incoming=0;
};
inline bool PlanGuildProcurementMaterials(const Task& task,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& bags,GuildProcurementMaterialPlan& out,std::string& why) {
    out={};GuildProcurementJob job;auto reject=[&](const char* code){why=code;return false;};
    if(!ValidateGuildProcurementTask(task,why) || !IsGuildProcurementTask(task) ||
        !DecodeGuildProcurementJob(task.checkpoint.data,job,why) || task.mode!=Mode::Active ||
        task.phase!=Phase::Preparing || !batch.complete || !batch.bookRevision || batch.claims.size()>16 || bags.size()>256)
        return reject("guild_procurement_material_snapshot_invalid");
    std::map<uint32_t,NativeResourceBalance> stock;
    for(const auto& item:bags)
        if(!ValidNativeResourceBalance(item) || item.actor!=task.actor || item.itemEntry!=job.entry ||
            item.location!="bags" || item.nativeReference || !stock.emplace(item.itemGuid,item).second)
            return reject("guild_procurement_material_identity_invalid");
    std::map<uint32_t,std::vector<const ResourceClaim*>> held;
    std::set<std::string> ids;
    for(const auto& c:batch.claims) {
        if(!ValidResourceClaim(c) || c.actor!=task.actor || c.task!=task.id || !ids.insert(c.id).second ||
            c.state!="held" || c.revision>=UINT64_MAX-1)
            return reject("guild_procurement_material_claim_unreconciled");
        if(c.itemEntry!=job.entry) {
            if(c.location!="money" && c.location!="bank" && c.location!="bags")
                return reject("guild_procurement_material_unrelated_claim");
            continue; // Shared capacity/money adapters own their own prerequisites.
        }
        if(c.location=="bank" || c.location=="mail") {out.incoming+=c.quantity;continue;}
        if(c.location!="bags" || c.copper || c.nativeReference || !stock.count(c.itemGuid))
            return reject("guild_procurement_carried_claim_not_backed");
        held[c.itemGuid].push_back(&c);out.carried+=c.quantity;
    }
    for(auto& row:held) {
        std::sort(row.second.begin(),row.second.end(),[](const auto* a,const auto* b){return a->id<b->id;});
        uint64_t total=0;for(const auto* c:row.second)total+=c->quantity;
        if(total>stock.at(row.first).quantity)return reject("guild_procurement_carried_claim_not_backed");
    }
    // Finish collecting paid/withdrawn stock before releasing surplus. This
    // keeps every paid receipt's identity until its actual native outcome.
    if(!out.incoming && out.carried>=job.quantity) {
        uint32_t remaining=job.quantity;
        for(const auto& row:held)for(const auto* c:row.second) {
            const auto kept=uint32_t(std::min<uint64_t>(remaining,c->quantity));remaining-=kept;
            if(kept==c->quantity)continue;
            auto after=*c;++after.revision;
            if(kept)after.quantity=kept;else after.state="released";
            out.changes.push_back({after,c->revision});
        }
        out.carriedReady=out.changes.empty();why.clear();return true;
    }
    uint64_t committed=out.carried+out.incoming;
    if(committed>=job.quantity){why.clear();return true;}
    uint32_t remaining=job.quantity-uint32_t(committed);
    size_t additions=0;
    for(const auto& row:stock) {
        uint32_t owned=0;for(const auto* c:held[row.first])owned+=c->quantity;
        const auto added=std::min(remaining,row.second.quantity-owned);
        if(!added)continue;
        ResourceClaim c;uint64_t expected=0;
        if(!held[row.first].empty()) {
            c=*held[row.first].front();expected=c.revision;++c.revision;c.quantity+=added;
        } else {
            c.task=task.id;c.actor=task.actor;c.itemGuid=row.first;c.itemEntry=job.entry;
            c.quantity=added;c.location="bags";c.state="held";++additions;
        }
        out.changes.push_back({c,expected});remaining-=added;
        if(!remaining)break;
    }
    if(batch.claims.size()+additions>16){out={};return reject("guild_procurement_material_claim_limit");}
    why.clear();return true;
}
}
