#include "botpch.h"
#include "LivingNativeBankWithdrawal.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingActivityTransfer.h"
#include "LivingActivityStackTransfer.h"
#include "LivingServiceExecution.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {
namespace {
bool SafeBankActor(Player& actor) {
    return actor.GetPlayerbotAI() && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
        !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        actor.IsStopped() && !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor);
}
bool ExactEmptyDestination(Player& actor,Item& item,uint8_t bag,uint8_t slot) {
    if (!Player::IsInventoryPos(bag,slot)) return false;
    if (actor.GetItemByPos(bag,slot)) return false;
    ItemPosCountVec positions;uint8_t affected=0;
    return actor.CanStoreItem(bag,slot,positions,&item,affected,false)==EQUIP_ERR_OK &&
        positions.size()==1 && positions[0].pos==uint16_t(uint16_t(bag)<<8|slot) && positions[0].count==item.GetCount();
}
bool EmptyDestination(Player& actor,Item& item,uint16_t& destination) {
    for (uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)
        if (ExactEmptyDestination(actor,item,INVENTORY_SLOT_BAG_0,slot)) {
            destination=uint16_t(INVENTORY_SLOT_BAG_0)<<8|slot;return true;
        }
    for (uint8_t bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag) {
        auto* container=static_cast<Bag*>(actor.GetItemByPos(INVENTORY_SLOT_BAG_0,bag));
        if (!container) continue;
        for (uint8_t slot=0;slot<container->GetBagSize();++slot)
            if (ExactEmptyDestination(actor,item,bag,slot)) {destination=uint16_t(bag)<<8|slot;return true;}
    }
    return false;
}
bool Destination(Player& actor,Item& item,uint16_t& destination,uint32_t& mergeGuid,uint32_t& mergeCount) {
    mergeGuid=mergeCount=0;
    ItemPosCountVec positions;uint8_t affected=0;
    if (actor.CanStoreItem(NULL_BAG,NULL_SLOT,positions,&item,affected,false)==EQUIP_ERR_OK &&
        positions.size()==1 && positions[0].count==item.GetCount() && Player::IsInventoryPos(positions[0].pos)) {
        if (const auto* target=actor.GetItemByPos(positions[0].pos)) {
            const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
            if (!view || !view->ready || target->GetEntry()!=item.GetEntry() || target->GetGUIDLow()==item.GetGUIDLow() ||
                view->HasUncertainItem(actor.GetGUIDLow(),item.GetEntry()) ||
                view->ProtectedItem(target->GetGUIDLow())>target->GetCount()) return false;
            mergeGuid=target->GetGUIDLow();mergeCount=target->GetCount();
        }
        destination=positions[0].pos;return true;
    }
    return EmptyDestination(actor,item,destination);
}
bool ExactDestination(Player& actor,Item& item,const NativeBankQuote& quote) {
    if (!quote.mergeGuid) return ExactEmptyDestination(actor,item,uint8_t(quote.to>>8),uint8_t(quote.to));
    const auto* target=actor.GetItemByPos(quote.to);
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if (!target || target->GetGUIDLow()!=quote.mergeGuid || target->GetEntry()!=quote.entry ||
        target->GetCount()!=quote.mergeCount || !view || !view->ready ||
        view->HasUncertainItem(quote.actor,quote.entry) || view->ProtectedItem(quote.mergeGuid)>quote.mergeCount) return false;
    ItemPosCountVec positions;uint8_t affected=0;
    return Player::IsInventoryPos(quote.to) &&
        actor.CanStoreItem(uint8_t(quote.to>>8),uint8_t(quote.to),positions,&item,affected,false)==EQUIP_ERR_OK &&
        positions.size()==1 && positions[0].pos==quote.to && positions[0].count==quote.quantity;
}
NativeItemStack Stack(Player& actor,const Item* item) {
    if (!item) return {};
    return {actor.GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount(),
        item->GetContainer()?item->GetContainer()->GetGUIDLow():0,item->GetSlot()};
}
bool ProtectedLegacy(Player& actor,const Item& item) {
    return sPlayerbotActionBroker.IsItemReserved(item.GetGUIDLow()) ||
        sGuildSupplies.ReservedEntry(actor.GetGUIDLow(),item.GetEntry()) ||
        ai::ItemUsageValue::IsNeededForQuest(&actor,item.GetEntry(),true);
}
}
uint64_t NativeNearbyBanker(Player& actor) {
    if (!SafeBankActor(actor)) return 0;
    auto* ai=actor.GetPlayerbotAI();
    for (const auto guid : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get())
        if (actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_BANKER)) return guid.GetRawValue();
    return 0;
}
std::string EncodeNativeBankQuote(const NativeBankQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"guid\":"+std::to_string(q.guid)+",\"entry\":"+
        std::to_string(q.entry)+",\"quantity\":"+std::to_string(q.quantity)+",\"banker\":"+std::to_string(q.banker)+
        ",\"banker_entry\":"+std::to_string(q.bankerEntry)+",\"from\":"+std::to_string(q.from)+",\"to\":"+
        std::to_string(q.to)+",\"bag_before\":"+std::to_string(q.bagBefore)+",\"total_before\":"+std::to_string(q.totalBefore)+
        (q.mergeGuid ? ",\"merge_guid\":"+std::to_string(q.mergeGuid)+",\"merge_count\":"+std::to_string(q.mergeCount) : "")+'}';
}
bool DecodeNativeBankQuote(const std::string& value,NativeBankQuote& q) {
    q={};
    try {
        boost::property_tree::ptree p;std::istringstream input(value);boost::property_tree::read_json(input,p);
        q.actor=p.get<uint32_t>("actor");q.guid=p.get<uint32_t>("guid");q.entry=p.get<uint32_t>("entry");
        q.quantity=p.get<uint32_t>("quantity");q.banker=p.get<uint64_t>("banker");q.bankerEntry=p.get<uint32_t>("banker_entry");
        q.from=p.get<uint16_t>("from");q.to=p.get<uint16_t>("to");q.bagBefore=p.get<uint32_t>("bag_before");
        q.totalBefore=p.get<uint32_t>("total_before");
        q.mergeGuid=p.get<uint32_t>("merge_guid",0);q.mergeCount=p.get<uint32_t>("merge_count",0);
        return value==EncodeNativeBankQuote(q) && q.actor && q.guid && q.entry && q.quantity && q.banker &&
            bool(q.mergeGuid)==bool(q.mergeCount) && q.mergeGuid!=q.guid && uint64_t(q.mergeCount)+q.quantity<=UINT32_MAX;
    } catch (...) {q={};return false;}
}
bool PlanNativeBankWithdrawal(Player& actor,const Task& task,const ProfessionReagent& need,
    NativeBankQuote& q,std::string& blocker) {
    q={};auto reject=[&](const char* why){blocker=why;return false;};
    if (!sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=task.actor || !SafeBankActor(actor))
        return reject("profession_bank_safety_pause");
    const auto banker=NativeNearbyBanker(actor);
    if (!banker) return reject("profession_banker_travel_required");
    ProfessionJob job;
    if (!DecodeProfessionJob(task.checkpoint.data,job,blocker)) return false;
    const auto reagent=std::find_if(job.reagents.begin(),job.reagents.end(),[&](const auto& r){return r.entry==need.entry;});
    if (reagent==job.reagents.end() || !need.perAttempt || need.perAttempt>reagent->perAttempt)
        return reject("profession_bank_exact_demand_required");
    for (auto* item : actor.GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BANK)) {
        if (!item || item->GetEntry()!=need.entry || ProtectedLegacy(actor,*item)) continue;
        uint32_t available=0;
        if (!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bank"},available,blocker)) return false;
        if (available!=item->GetCount() || !available) continue;
        uint16_t to=0;
        if (!Destination(actor,*item,to,q.mergeGuid,q.mergeCount)) return reject("profession_bank_single_destination_required");
        q.actor=task.actor;q.guid=item->GetGUIDLow();q.entry=item->GetEntry();q.quantity=item->GetCount();
        q.banker=banker;q.bankerEntry=actor.GetNPCIfCanInteractWith(ObjectGuid(banker),UNIT_NPC_FLAG_BANKER)->GetEntry();
        q.from=item->GetPos();q.to=to;q.bagBefore=actor.GetItemCount(q.entry,false);q.totalBefore=actor.GetItemCount(q.entry,true);
        blocker.clear();return true;
    }
    return reject("profession_bank_uncommitted_stack_unavailable");
}
bool NativeBankWithdrawal::ValidateNative(Player& actor,const OperationRequest& r,std::string& blocker) {
    auto reject=[&](const char* why){blocker=why;return false;};
    const auto& c=r.itemTransfer;
    if (!SafeBankActor(actor)) return reject("profession_bank_safety_pause");
    if (!ValidBankTransfer(c) || c.actor!=actor.GetGUIDLow() || c.actor!=quote.actor ||
        c.task!=r.transition.task.root || c.itemGuid!=quote.guid || c.itemEntry!=quote.entry || c.quantity!=quote.quantity ||
        r.beforeState!=EncodeNativeBankQuote(quote)) return reject("profession_bank_claim_quote_mismatch");
    auto* banker=actor.GetNPCIfCanInteractWith(ObjectGuid(quote.banker),UNIT_NPC_FLAG_BANKER);
    if (!banker || banker->GetEntry()!=quote.bankerEntry) return reject("profession_banker_travel_required");
    auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.guid));
    if (!item || item->GetOwnerGuid()!=actor.GetObjectGuid() || item->GetEntry()!=quote.entry || item->GetPos()!=quote.from ||
        !Player::IsBankPos(item->GetBagSlot(),item->GetSlot()) || item->GetCount()!=quote.quantity ||
        ProtectedLegacy(actor,*item)) return reject("profession_banked_stack_changed");
    if (actor.GetItemCount(quote.entry,false)!=quote.bagBefore || actor.GetItemCount(quote.entry,true)!=quote.totalBefore ||
        !ExactDestination(actor,*item,quote))
        return reject("profession_bank_destination_changed");
    blocker.clear();return true;
}
NativeObservation NativeBankWithdrawal::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string blocker;
    if (!ValidateNative(actor,request,blocker)) {out.state=OperationState::Rejected;out.evidence=blocker;return out;}
    auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.guid));
    ItemPosCountVec positions;uint8_t affected=0;
    if (actor.CanStoreItem(uint8_t(quote.to>>8),uint8_t(quote.to),positions,item,affected,false)!=EQUIP_ERR_OK) {
        out.state=OperationState::Rejected;out.evidence="profession_bank_native_capacity_rejected";return out;
    }
    const auto source=Stack(actor,item),destinationBefore=Stack(actor,actor.GetItemByPos(quote.to));
    actor.RemoveItem(item->GetBagSlot(),item->GetSlot(),true);
    actor.StoreItem(positions,item,true);
    const auto survivingGuid=quote.mergeGuid ? quote.mergeGuid : quote.guid;
    auto* moved=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,survivingGuid));
    const auto destinationAfter=Stack(actor,moved);
    const bool identityVerified=VerifyWholeStackTransfer(source,destinationBefore,destinationAfter,
        actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.guid))!=nullptr,blocker);
    out.nativeReference="bank_item:"+std::to_string(quote.guid);
    out.afterState="{\"guid\":"+std::to_string(quote.guid)+",\"to\":"+std::to_string(quote.to)+
        ",\"bag\":"+std::to_string(moved && moved->GetContainer() ? moved->GetContainer()->GetGUIDLow() : 0)+
        ",\"slot\":"+std::to_string(moved ? moved->GetSlot() : 0)+
        ",\"bag_count\":"+std::to_string(actor.GetItemCount(quote.entry,false))+
        ",\"total_count\":"+std::to_string(actor.GetItemCount(quote.entry,true))+
        ",\"surviving_guid\":"+std::to_string(destinationAfter.guid)+",\"surviving_count\":"+std::to_string(destinationAfter.count)+'}';
    if (moved && moved->GetPos()==quote.to && identityVerified &&
        moved->GetOwnerGuid()==actor.GetObjectGuid() && actor.GetItemCount(quote.entry,true)==quote.totalBefore &&
        uint64_t(actor.GetItemCount(quote.entry,false))==uint64_t(quote.bagBefore)+quote.quantity) {
        out.state=OperationState::Verified;out.evidence="native_bank_stack_relocated";
        out.transferredItem={quote.actor,survivingGuid,quote.entry,moved->GetCount(),0,"bags"};
    } else out.evidence="native_bank_transfer_requires_reconciliation";
    // Invalidate item-list/value caches after the real move, not the whole AI.
    auto* context=actor.GetPlayerbotAI()->GetAiObjectContext();
    for (const auto& name : context->GetValues()) {
        const auto base=name.substr(0,name.find("::"));
        if (base=="bag space" || base=="bank space" || base=="item usage" || base=="item count" ||
            base=="bank item count" || base=="inventory items" || base=="inventory item ids" || base=="bank items")
            if (auto* value=context->GetUntypedValue(name)) value->Reset();
    }
    return out;
}
std::string NativeBankWithdrawal::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& outcome) const {
    // Check the surviving identity and consumed merge source in the SAME save.
    const auto survivingGuid=quote.mergeGuid ? quote.mergeGuid : quote.guid;
    auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,survivingGuid));
    if (!item) return {};
    const auto bag=item->GetContainer() ? item->GetContainer()->GetGUIDLow() : 0;
    return "SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
        " FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+std::to_string(quote.actor)+
        " AND v.item="+std::to_string(survivingGuid)+" AND v.item_template="+std::to_string(quote.entry)+
        " AND v.bag="+std::to_string(bag)+" AND v.slot="+std::to_string(item->GetSlot())+
        " AND i.owner_guid="+std::to_string(quote.actor)+" AND i.itemEntry="+std::to_string(quote.entry)+
        " AND i.count="+std::to_string(uint64_t(quote.quantity)+quote.mergeCount)+
        (quote.mergeGuid ? " AND NOT EXISTS(SELECT 1 FROM item_instance removed WHERE removed.guid="+std::to_string(quote.guid)+')' : "");
}
}
