#include "botpch.h"
#include "LivingNativeGuildProcurement.h"
#include "LivingActivityCoordinator.h"
#include "LivingNativeBankWithdrawal.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "PlayerbotInventoryPressure.h"
#include "strategy/values/ItemUsageValue.h"

namespace LivingActivity {
bool ReadNativeGuildProcurementMaterials(Player& actor,const Task& task,const UnsettledClaimBatch& claims,
    GuildProcurementMaterialPlan& result,std::string& why) {
    result={};GuildProcurementJob job;
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.IsInWorld() ||
        actor.GetGUIDLow()!=task.actor || !DecodeGuildProcurementJob(task.checkpoint.data,job,why)) {
        why="guild_procurement_inventory_unavailable";return false;
    }
    std::vector<NativeResourceBalance> bags;unsigned inspected=0;
    for(auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
        if(++inspected>256){why="guild_procurement_inventory_snapshot_limit";return false;}
        if(!item || item->GetEntry()!=job.entry)continue;
        // Native delivery currently requires exclusive stack custody. Never
        // seize another job's portion, quest materials, equipment or a trade.
        if(item->GetOwnerGuid()!=actor.GetObjectGuid() || !item->CanBeTraded() || item->IsConjuredConsumable() ||
            item->IsInTrade() || item->IsSoulBound() ||
            sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
            sGuildSupplies.ReservedEntry(task.actor,job.entry) ||
                ai::ItemUsageValue::IsNeededForQuest(&actor,job.entry,true))continue;
        const bool alreadyClaimed=std::any_of(claims.claims.begin(),claims.claims.end(),[&](const auto& c){
            return c.itemGuid==item->GetGUIDLow() && c.itemEntry==job.entry;
        });
        if(!alreadyClaimed) {
            bool reserved=false;const auto disposition=sPlayerbotInventoryPressure.Classify(&actor,item,&reserved);
            if(reserved || (disposition!=LivingWowItemDisposition::Vendor && disposition!=LivingWowItemDisposition::Auction))continue;
        }
        NativeResourceBalance native{task.actor,item->GetGUIDLow(),job.entry,item->GetCount(),0,"bags"};
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,native,available,why))return false;
        if(available==item->GetCount())bags.push_back(native);
    }
    return PlanGuildProcurementMaterials(task,claims,bags,result,why);
}
bool NativeGuildProcurementReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);GuildProcurementJob job;
    if(!saved || saved->actor!=actor.GetGUIDLow() || saved->revision!=request.transition.expectedRevision ||
        saved->checkpoint.data!=request.transition.task.checkpoint.data || request.changes.empty() ||
        !DecodeGuildProcurementJob(saved->checkpoint.data,job,why) ||
        !sLivingActivityCoordinator.ValidateGuildProcurementDemand(*saved,why))return false;
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,claims,why))return false;
    // Bank transfer remains the shared native whole-stack operation. Any real
    // surplus is trimmed from claims AFTER withdrawal, not remotely split.
    if(request.changes.size()==1 && request.changes.front().after.location=="bank") {
        const auto& c=request.changes.front().after;NativeBankQuote quote;
        if(!PlanNativeBankWithdrawal(actor,*saved,{job.entry,job.quantity},quote,why))return false;
        if(request.changes.front().expectedRevision || c.revision!=1 || !IsUuid(c.id) || c.task!=saved->id ||
            c.actor!=saved->actor || c.state!="held" || c.nativeReference || c.copper ||
            c.itemGuid!=quote.guid || c.itemEntry!=quote.entry || c.quantity!=quote.quantity) {
            why="guild_procurement_bank_reservation_changed";return false;
        }
        why.clear();return true;
    }
    GuildProcurementMaterialPlan plan;
    if(!ReadNativeGuildProcurementMaterials(actor,*saved,claims,plan,why))return false;
    if(plan.changes.size()!=request.changes.size()){why="guild_procurement_material_plan_changed";return false;}
    std::set<std::string> used;
    for(const auto& proposed:plan.changes) {
        bool found=false;
        for(const auto& actual:request.changes) {
            auto expected=proposed.after;
            if(!proposed.expectedRevision)expected.id=actual.after.id;
            if(!IsUuid(actual.after.id) || proposed.expectedRevision!=actual.expectedRevision ||
                !SameResourceClaim(expected,actual.after))continue;
            if(!used.insert(actual.after.id).second){why="guild_procurement_material_duplicate_claim";return false;}
            found=true;break;
        }
        if(!found){why="guild_procurement_material_plan_changed";return false;}
    }
    why.clear();return true;
}
}
