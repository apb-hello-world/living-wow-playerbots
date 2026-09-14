#include "botpch.h"
#include "LivingNativeLootCollection.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingGuildProcurement.h"
#include "LivingProfessionDemand.h"
#include "LivingServiceExecution.h"
#include "Loot/LootMgr.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"
#include <chrono>

namespace LivingActivity {
namespace {
uint64_t Generation(const Loot& loot) {
    const auto n=std::chrono::duration_cast<std::chrono::microseconds>(loot.GetCreateTime().time_since_epoch()).count();
    return n>0?uint64_t(n):0;
}
Loot* WorldLoot(Player& actor,uint64_t source) {
    const ObjectGuid guid(source);auto* ai=actor.GetPlayerbotAI();
    if(!ai)return nullptr;
    if(guid.IsGameObject()) {auto* go=ai->GetGameObject(guid);return go?go->m_loot:nullptr;}
    if(guid.IsCreature()) {auto* creature=ai->GetCreature(guid);return creature?creature->m_loot:nullptr;}
    return nullptr;
}
}
bool InspectNativeLootQuote(Player& actor,uint64_t source,uint32_t slot,NativeLootQuote& q,std::string& why) {
    q={};auto reject=[&](const char* s){why=s;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.GetSession() ||
        !actor.IsInWorld() || !actor.GetMap() || !actor.IsStopped() || actor.GetTradeData() ||
        ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || LivingServiceExecution::Busy(&actor))
        return reject("native_loot_safety_pause");
    // The initial procurement adapter does not bypass shared loot, human-first
    // rolls, quest permissions or dungeon commitments. Those retain native AI.
    if(actor.GetGroup() || actor.GetMap()->IsDungeon())return reject("native_loot_group_or_instance_adapter_required");
    auto* loot=sLootMgr.GetLoot(&actor);
    if(!loot || loot!=WorldLoot(actor,source) || loot->GetLootGuid().GetRawValue()!=source || slot>255 ||
        !loot->GetLootTarget() || !actor.IsWithinDistInMap(loot->GetLootTarget(),INTERACTION_DISTANCE))
        return reject("native_loot_open_world_source_required");
    if(loot->GetLootType()!=LOOT_CORPSE && loot->GetLootType()!=LOOT_SKINNING)
        return reject("native_loot_source_kind_unsupported");
    auto* item=loot->GetLootItemInSlot(slot);
    if(!item || item->lootSlot!=slot || !item->count || !item->itemProto || item->isBlocked ||
        !item->IsAllowed(&actor,loot))return reject("native_loot_slot_not_owned");
    const auto slotType=item->GetSlotTypeForSharedLoot(&actor,loot);
    if(slotType!=LOOT_SLOT_NORMAL && slotType!=LOOT_SLOT_OWNER)return reject("native_loot_slot_requires_native_roll");
    if(item->randomPropertyId || item->randomSuffix || item->itemProto->Bonding!=NO_BIND ||
        ai::ItemUsageValue::IsNeededForQuest(&actor,item->itemId,true))return reject("native_loot_personal_item_protected");
    const auto protection=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!protection || !protection->ready || protection->HasUncertainItem(actor.GetGUIDLow(),item->itemId) ||
        sGuildSupplies.ReservedEntry(actor.GetGUIDLow(),item->itemId))return reject("native_loot_item_reconciliation_required");
    ItemPosCountVec positions;
    if(actor.CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,item->itemId,item->count)!=EQUIP_ERR_OK ||
        positions.empty() || positions.size()>MaximumItemGainStacks)return reject("native_loot_capacity_required");
    for(const auto& position:positions)if(auto* existing=actor.GetItemByPos(position.pos)) {
        if(existing->IsInTrade() || sPlayerbotActionBroker.IsItemReserved(existing->GetGUIDLow()) ||
            protection->ProtectedItem(existing->GetGUIDLow()))return reject("native_loot_destination_reserved");
    }
    q={actor.GetGUIDLow(),item->itemId,item->count,slot,uint32_t(loot->GetLootType()),actor.GetMoney(),
        actor.GetItemCount(item->itemId,false),source,Generation(*loot)};
    if(!ValidNativeLootQuote(q))return reject("native_loot_quote_out_of_range");
    why.clear();return true;
}
bool NativeLootCollection::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    auto reject=[&](const char* s){why=s;return false;};
    if(request.kind!=OperationKind() || request.effects!=OperationEffects() || request.persistence!=PersistencePolicy() ||
        !request.consumption.empty() || !request.mailGain.Empty() || !request.itemTransfer.id.empty() ||
        request.itemGain.entry!=quote.entry || request.itemGain.quantity!=quote.quantity || quote.actor!=actor.GetGUIDLow() ||
        request.beforeState!=EncodeNativeLootQuote(quote))return reject("native_loot_exact_intent_required");
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    if(!saved || saved->actor!=actor.GetGUIDLow() || saved->checkpoint.data!=request.transition.task.checkpoint.data ||
        (saved->revision!=request.transition.expectedRevision && saved->revision!=request.transition.task.revision))
        return reject("native_loot_saved_intent_changed");
    const auto& task=*saved;
    if(!IsGuildProcurementTask(task) || !ValidateGuildProcurementTask(task,why) ||
        !sLivingActivityCoordinator.ValidateGuildProcurementDemand(task,why))return false;
    NativeLootQuote current;
    if(!InspectNativeLootQuote(actor,quote.source,quote.slot,current,why))return false;
    if(EncodeNativeLootQuote(current)!=EncodeNativeLootQuote(quote))return reject("native_loot_generation_or_slot_changed");
    NativeProfessionDemand demand;
    if(!InspectNativeProfessionDemand(actor,task,demand)) {why=demand.blocker;return false;}
    for(const auto& row:demand.stock)if(row.entry==quote.entry) {
        // Never add an incidental drop to an obligation already covered by a
        // carried, banked or paid-incoming quantity. Native slots are indivisible.
        uint64_t required=0;
        for(const auto& r:demand.requirements)if(r.entry==quote.entry)required=r.perAttempt;
        const uint64_t covered=uint64_t(row.bag)+row.bank+row.delivered+row.paidInTransit;
        if(required>covered && quote.quantity<=required-covered) {why.clear();return true;}
        return reject("native_loot_demand_already_covered_or_slot_exceeds_need");
    }
    return reject("native_loot_not_a_requested_material");
}
NativeObservation NativeLootCollection::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation observation;std::string why;
    if(!ValidateNative(actor,request,why)) {observation.state=OperationState::Rejected;observation.evidence=why;return observation;}
    WorldPacket packet(CMSG_AUTOSTORE_LOOT_ITEM,1);packet<<uint8_t(quote.slot);
    actor.GetSession()->HandleAutostoreLootItemOpcode(packet);
    NativeLootResult result{quote,actor.GetMoney(),actor.GetItemCount(quote.entry,false),true,false};
    // The native handler can release/deactivate the source on its last slot.
    // Do not dereference a pre-handler Loot/slot pointer afterwards.
    auto* after=WorldLoot(actor,quote.source);
    if(!after)result.slotConsumed=true;
    else if(Generation(*after)==quote.generation) {
        auto* item=after->GetLootItemInSlot(quote.slot);
        result.slotConsumed=!item || !item->IsAllowed(&actor,after);
    }
    observation.state=VerifyNativeLootResult(result,observation.evidence);
    observation.nativeReference="loot:"+std::to_string(quote.source)+":generation:"+std::to_string(quote.generation)+
        ":slot:"+std::to_string(quote.slot);
    observation.afterState=EncodeNativeLootResult(result);
    // The coordinator separately verifies every gained native GUID/slot/count,
    // commits protection, and persists items with the operation in one receipt.
    return observation;
}
std::string NativeLootCollection::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& outcome) const {
    return "SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+" FROM characters WHERE guid="+
        std::to_string(actor.GetGUIDLow())+" AND money="+std::to_string(actor.GetMoney());
}
}
