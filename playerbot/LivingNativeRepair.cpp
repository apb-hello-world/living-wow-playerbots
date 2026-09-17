#include "botpch.h"
#include "LivingNativeRepair.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingServiceExecution.h"
#include "LivingPurchaseBudget.h"
#include "LivingTaskItemRequirements.h"
#include "LivingPartyRepair.h"

namespace LivingActivity {
namespace {
Item* BrokenEquipment(Player& actor,bool damaged=false) {
    // Restore real damage/defence before less critical armour. Native slots,
    // not preferred gear or item quality, define this bounded scan.
    const uint8_t first=actor.getClass()==CLASS_HUNTER?EQUIPMENT_SLOT_RANGED:EQUIPMENT_SLOT_MAINHAND;
    auto broken=[&](uint8_t slot)->Item* {
        auto* item=actor.GetItemByPos(INVENTORY_SLOT_BAG_0,slot);
        return item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) &&
            (damaged ? item->GetUInt32Value(ITEM_FIELD_DURABILITY)<item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) :
                !item->GetUInt32Value(ITEM_FIELD_DURABILITY))?item:nullptr;
    };
    if(auto* item=broken(first))return item;
    for(const auto slot:{EQUIPMENT_SLOT_MAINHAND,EQUIPMENT_SLOT_OFFHAND,EQUIPMENT_SLOT_RANGED})
        if(auto* item=broken(uint8_t(slot)))return item;
    for(uint8_t slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot)
        if(auto* item=broken(slot))return item;
    return nullptr;
}
bool RepairMoney(const ResourceClaim& c,const Task& task) {
    return ValidResourceClaim(c) && c.task==task.id && c.actor==task.actor && c.location=="money" &&
        c.state=="held" && c.copper && !c.itemGuid && !c.itemEntry && !c.quantity && !c.nativeReference;
}
bool SafeRepair(Player& actor) {
    return actor.GetPlayerbotAI() && actor.GetSession() && actor.IsInWorld() && actor.GetMap() &&
        actor.IsAlive() && !actor.IsBeingTeleported() && !actor.GetTradeData() &&
        !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor);
}
}
bool HasNativeCriticalRepair(Player& actor) {return BrokenEquipment(actor)!=nullptr;}
bool HasNativeDamagedEquipment(Player& actor) {return BrokenEquipment(actor,true)!=nullptr;}
bool PlanNativeCriticalRepair(Player& actor,const Task& task,NativeRepairQuote& q,ResourceClaim& held,std::string& blocker) {
    q={};held={};auto reject=[&](const char* why){blocker=why;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !SafeRepair(actor) || actor.GetGUIDLow()!=task.actor ||
        task.mode!=Mode::Active || !task.accepted || task.root!=task.id || !RepairPrerequisiteStep(task.checkpoint.step))
        return reject("critical_repair_safety_or_task_pause");
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,blocker))return false;
    for(const auto& c:claims.claims)if(c.location=="money") {
        if(!held.id.empty() || !RepairMoney(c,task))return reject("critical_repair_money_requires_reconciliation");
        held=c;
    }
    auto* item=BrokenEquipment(actor,IsPartyRepairTask(task));
    if(!item)return reject("critical_repair_not_required");
    if(item->GetOwnerGuid()!=actor.GetObjectGuid() || item->IsInTrade())return reject("critical_repair_item_unavailable");
    Creature* vendor=nullptr;
    if(actor.IsStopped())for(const auto guid:actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get())
        if((vendor=actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_REPAIR)))break;
    if(!vendor)return reject("critical_repair_travel_required");
    const auto* proto=item->GetProto();
    const auto* costs=proto?sDurabilityCostsStore.LookupEntry(proto->ItemLevel):nullptr;
    const auto* quality=proto?sDurabilityQualityStore.LookupEntry((proto->Quality+1)*2):nullptr;
    if(!costs || !quality)return reject("critical_repair_native_price_unavailable");
    const uint32_t maximum=item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    const uint32_t durability=item->GetUInt32Value(ITEM_FIELD_DURABILITY);
    uint32_t copper=0;
    if(!NativeRepairPrice(maximum-durability,costs->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class,proto->SubClass)],
        double(quality->quality_mod),actor.GetReputationPriceDiscount(vendor),copper))return reject("critical_repair_native_price_invalid");
    uint32_t available=0;
    if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
        {task.actor,0,0,0,actor.GetMoney(),"money"},available,blocker))return false;
    if(available<copper)
        return reject("critical_repair_unreserved_money_shortfall");
    q={task.actor,item->GetGUIDLow(),item->GetEntry(),maximum,durability,actor.GetMoney(),copper,
        vendor->GetEntry(),vendor->GetObjectGuid().GetRawValue(),item->GetPos()};
    if(!ValidNativeRepairQuote(q))return reject("critical_repair_quote_invalid");
    blocker.clear();return true;
}
bool NativeRepairReservation::ValidatePurpose(Player& actor,const ReservationRequest& r,std::string& blocker) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    auto reject=[&](){blocker="critical_repair_exact_reservation_required";return false;};
    if(!saved || saved->actor!=actor.GetGUIDLow() || saved->revision!=r.transition.expectedRevision ||
        !RepairPrerequisiteStep(saved->checkpoint.step) || r.changes.size()!=1)return reject();
    const auto& change=r.changes.front();const auto& c=change.after;
    // Only this prerequisite may release its own otherwise unconsumed escrow.
    // A native-effect intent is excluded by the shared reservation validator.
    if(c.state=="released") {
        UnsettledClaimBatch claims;
        if(!sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,claims,blocker))return false;
        for(const auto& held:claims.claims)if(held.id==c.id && RepairMoney(held,*saved)) {
            auto next=held;++next.revision;next.state="released";
            if(held.revision==change.expectedRevision && SameResourceClaim(c,next)){blocker.clear();return true;}
        }
        return reject();
    }
    NativeRepairQuote q;ResourceClaim held;
    if(saved->phase!=Phase::Preparing || saved->checkpoint.step!="maintenance_repair_prepare" ||
        !PlanNativeCriticalRepair(actor,*saved,q,held,blocker))return false;
    if(!held.id.empty() || change.expectedRevision || c.revision!=1 || !RepairMoney(c,*saved) || c.copper!=q.copper)return reject();
    blocker.clear();return true;
}
bool NativeCriticalRepair::ValidateNative(Player& actor,const OperationRequest& r,std::string& blocker) {
    NativeRepairQuote current;ResourceClaim held;
    const auto task=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    if(!task || r.beforeState!=EncodeNativeRepairQuote(quote) || r.consumption.size()!=1 ||
        !r.itemGain.Empty() || !r.mailGain.Empty() || !r.itemTransfer.id.empty() ||
        !PlanNativeCriticalRepair(actor,*task,current,held,blocker) || EncodeNativeRepairQuote(current)!=EncodeNativeRepairQuote(quote) ||
        held.id.empty() || held.copper!=quote.copper || !SameResourceClaim(held,r.consumption.front().before) ||
        r.consumption.front().used!=quote.copper) {
        if(blocker.empty())blocker="critical_repair_quote_or_claim_changed";return false;
    }
    blocker.clear();return true;
}
NativeObservation NativeCriticalRepair::ExecuteNative(Player& actor,const OperationRequest& r) {
    NativeObservation out;std::string blocker;
    if(!ValidateNative(actor,r,blocker)){out.state=OperationState::Rejected;out.evidence=blocker;return out;}
    auto* vendor=actor.GetNPCIfCanInteractWith(ObjectGuid(quote.vendor),UNIT_NPC_FLAG_REPAIR);
    // The legacy RepairAllAction may supply cheat money. This adapter instead
    // pays the exact native price from the actor's real, reserved copper.
    actor.DurabilityRepair(quote.position,true,actor.GetReputationPriceDiscount(vendor),false);
    NativePurchaseEpoch().Changed(actor.GetGUIDLow());
    const auto* item=actor.GetItemByPos(quote.position);
    const uint32_t durability=item?item->GetUInt32Value(ITEM_FIELD_DURABILITY):0;
    out.nativeReference="repair:"+std::to_string(quote.item);
    out.afterState="{\"item\":"+std::to_string(item?item->GetGUIDLow():0)+",\"entry\":"+std::to_string(item?item->GetEntry():0)+
        ",\"position\":"+std::to_string(item?item->GetPos():0)+",\"maximum\":"+
        std::to_string(item?item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY):0)+",\"durability\":"+std::to_string(durability)+
        ",\"money\":"+std::to_string(actor.GetMoney())+'}';
    if(item && VerifyNativeRepair(quote,item->GetGUIDLow(),item->GetEntry(),item->GetPos(),
        item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY),durability,actor.GetMoney())) {
        out.state=OperationState::Verified;out.evidence="native_repair_durability_and_payment_observed";
    } else if(item && item->GetGUIDLow()==quote.item && item->GetEntry()==quote.entry &&
        item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)==quote.maximum && durability==quote.durability && actor.GetMoney()==quote.money) {
        out.state=OperationState::Rejected;out.evidence="native_repair_rejected_without_effect";
    } else out.evidence="native_repair_requires_reconciliation";
    for(const auto* name:{"durability","repair cost","min repair cost"})
        if(auto* value=actor.GetPlayerbotAI()->GetAiObjectContext()->GetUntypedValue(name))value->Reset();
    return out;
}
std::string NativeCriticalRepair::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& after) const {
    const auto* item=actor.GetItemByPos(quote.position);
    if(!item || item->GetGUIDLow()!=quote.item)return "";
    std::string equipmentProof;
    if(IsPartyRepairTask(after) && after.phase==Phase::Completed) {
        // Completion proves all equipped durable items, not an aggregate score.
        for(uint8_t slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot) {
            const auto* equipped=actor.GetItemByPos(INVENTORY_SLOT_BAG_0,slot);
            if(!equipped || !equipped->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))continue;
            equipmentProof+=" AND EXISTS(SELECT 1 FROM character_inventory ev JOIN item_instance ei ON ei.guid=ev.item"
                " WHERE ev.guid=c.guid AND ev.bag=0 AND ev.slot="+std::to_string(slot)+" AND ei.guid="+
                std::to_string(equipped->GetGUIDLow())+" AND ei.owner_guid=c.guid AND ei.durability="+
                std::to_string(equipped->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))+")";
        }
    }
    return "SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+" FROM characters c JOIN character_inventory v ON v.guid=c.guid"
        " JOIN item_instance i ON i.guid=v.item WHERE c.guid="+std::to_string(actor.GetGUIDLow())+" AND c.money="+
        std::to_string(actor.GetMoney())+" AND v.bag=0 AND v.slot="+std::to_string(quote.position&255)+
        " AND v.item="+std::to_string(quote.item)+" AND i.itemEntry="+std::to_string(quote.entry)+" AND i.owner_guid=c.guid"
        " AND i.durability="+std::to_string(item->GetUInt32Value(ITEM_FIELD_DURABILITY))+equipmentProof;
}
}
