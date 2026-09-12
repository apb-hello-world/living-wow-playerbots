#include "botpch.h"
#include "LivingNativeVendorSale.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingNativeMailCollection.h"
#include "LivingNativeCraftCapture.h"
#include "LivingProfessionNative.h"
#include "LivingServiceExecution.h"
#include "LivingPurchaseBudget.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <sstream>

namespace LivingActivity {
namespace {
bool SafeActor(Player& actor) {
    return actor.GetPlayerbotAI() && actor.GetSession() && actor.IsInWorld() && actor.IsAlive() &&
        !actor.IsBeingTeleported() && !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor);
}
bool NeededCapacity(Player& actor,const Task& task,const ProfessionJob& job,const UnsettledClaimBatch& claims,
    ItemGainSpec& need,std::string& blocker) {
    auto missing=[&](uint32_t entry,uint32_t quantity) {
        ItemPosCountVec positions;
        if (entry && quantity && actor.CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,quantity)==EQUIP_ERR_INVENTORY_FULL) {
            need={entry,quantity};return true;
        }
        return false;
    };
    ItemGainSpec output;
    if (!ReadNativeCraftOutput(actor,job,output,blocker)) return false;
    if (missing(output.entry,output.quantity)) return true;
    // Paid/committed attachments may require space even when the craft output
    // fits an existing stack. Only exact native attachments count as demand.
    for (const auto& c : claims.claims) if(c.location=="mail") {
        NativeResourceBalance balance;
        if (!ReadNativeMailBalance(actor,c,balance)) {blocker="capacity_mail_requires_reconciliation";return false;}
        if (missing(c.itemEntry,balance.quantity)) return true;
    }
    for (const auto& reagent : job.reagents) if(actor.GetItemCount(reagent.entry,false)<reagent.perAttempt)
        for(auto* item : actor.GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BANK)) {
            if (!item || item->GetEntry()!=reagent.entry) continue;
            uint32_t available=0;
            if (!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
                {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bank"},available,blocker)) return false;
            if (available==item->GetCount() && missing(item->GetEntry(),item->GetCount())) return true;
        }
    blocker="capacity_already_available";return false;
}
bool JobProtected(Player& actor,const ProfessionJob& job,Item& item) {
    ItemGainSpec output;std::string blocker;
    if (!ReadNativeCraftOutput(actor,job,output,blocker) || item.GetEntry()==output.entry) return true;
    for (const auto& reagent : job.reagents) if (item.GetEntry()==reagent.entry) return true;
    const auto* spell=sSpellTemplate.LookupEntry<SpellEntry>(job.recipe);
    if (!spell) return true;
    for (const auto tool : spell->Totem) if (tool && item.GetEntry()==uint32_t(tool)) return true;
    // Preserve tools with category requirements conservatively until native
    // tool-category identity can be represented by the common claim adapter.
    for (const auto category : spell->TotemCategory) if (category) return true;
    return false;
}
CapacitySaleFacts Facts(Player& actor,const ProfessionJob& job,Item& item) {
    CapacitySaleFacts f;
    f.actor=actor.GetGUIDLow();f.guid=item.GetGUIDLow();f.entry=item.GetEntry();f.quantity=item.GetCount();f.money=actor.GetMoney();
    f.ownedBag=item.GetOwnerGuid()==actor.GetObjectGuid() && Player::IsInventoryPos(item.GetBagSlot(),item.GetSlot()) && !item.IsBag();
    const auto* proto=item.GetProto();if(!proto)return f;
    f.unitCopper=proto->SellPrice;
    f.legacyProtected=sPlayerbotActionBroker.IsItemReserved(f.guid) || sGuildSupplies.Reserved(f.guid) ||
        sGuildSupplies.ReservedEntry(f.actor,f.entry) || ai::ItemUsageValue::IsNeededForQuest(&actor,f.entry,true) ||
        actor.GetLootGuid()==item.GetObjectGuid() || item.IsInTrade() || JobProtected(actor,job,item);
    for (const auto& spell : proto->Spells) if (spell.SpellId && spell.SpellCharges<0) f.charged=true;
    // Reuse the established gear/profession/quest classification, refreshed
    // for this one item. The activity claim is validated separately below.
    ai::ItemQualifier qualifier(&item);
    auto* value=actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::ItemUsage>("item usage",qualifier.GetQualifier());
    value->Reset();const auto usage=value->Get();
    f.disposable=usage==ai::ItemUsage::ITEM_USAGE_VENDOR || usage==ai::ItemUsage::ITEM_USAGE_BAD_EQUIP || usage==ai::ItemUsage::ITEM_USAGE_FORCE_GREED;
    return f;
}
}
bool NativeCapacityNeed(Player& actor,const Task& task,const ProfessionJob& job,
    const UnsettledClaimBatch& claims,ItemGainSpec& need,std::string& blocker) {
    return NeededCapacity(actor,task,job,claims,need,blocker);
}
bool NativeCapacityItemProtected(Player& actor,const ProfessionJob& job,Item& item) {
    const auto facts=Facts(actor,job,item);
    return !facts.ownedBag || facts.legacyProtected || facts.charged;
}
std::string EncodeNativeSaleQuote(const NativeSaleQuote& q) {
    const auto& f=q.item;
    return "{\"actor\":"+std::to_string(f.actor)+",\"guid\":"+std::to_string(f.guid)+",\"entry\":"+std::to_string(f.entry)+
        ",\"quantity\":"+std::to_string(f.quantity)+",\"unit_copper\":"+std::to_string(f.unitCopper)+",\"money\":"+
        std::to_string(f.money)+",\"copper\":"+std::to_string(q.copper)+",\"count_before\":"+std::to_string(q.countBefore)+
        ",\"vendor\":"+std::to_string(q.vendor)+",\"vendor_entry\":"+std::to_string(q.vendorEntry)+",\"from\":"+
        std::to_string(q.from)+",\"capacity_entry\":"+std::to_string(q.capacityEntry)+",\"capacity_quantity\":"+
        std::to_string(q.capacityQuantity)+'}';
}
bool DecodeNativeSaleQuote(const std::string& value,NativeSaleQuote& q) {
    q={};
    try {
        boost::property_tree::ptree p;std::istringstream input(value);boost::property_tree::read_json(input,p);
        auto& f=q.item;f.actor=p.get<uint32_t>("actor");f.guid=p.get<uint32_t>("guid");f.entry=p.get<uint32_t>("entry");
        f.quantity=p.get<uint32_t>("quantity");f.unitCopper=p.get<uint32_t>("unit_copper");f.money=p.get<uint32_t>("money");
        f.ownedBag=f.disposable=true;q.copper=p.get<uint32_t>("copper");q.countBefore=p.get<uint32_t>("count_before");
        q.vendor=p.get<uint64_t>("vendor");q.vendorEntry=p.get<uint32_t>("vendor_entry");q.from=p.get<uint16_t>("from");
        q.capacityEntry=p.get<uint32_t>("capacity_entry");q.capacityQuantity=p.get<uint32_t>("capacity_quantity");
        uint32_t price=0;
        return EncodeNativeSaleQuote(q)==value && q.vendor && q.vendorEntry && q.capacityEntry && q.capacityQuantity &&
            QuoteCapacitySale(f,price) && price==q.copper && q.countBefore>=f.quantity;
    } catch(...) {q={};return false;}
}
bool PlanNativeCapacitySale(Player& actor,const Task& task,NativeSaleQuote& q,ResourceClaim& held,std::string& blocker) {
    q={};held={};auto reject=[&](const char* why){blocker=why;return false;};
    if (!sLivingActivityCoordinator.OnWorldThread() || !SafeActor(actor) || actor.GetGUIDLow()!=task.actor ||
        !IsProfessionJob(task) || task.mode!=Mode::Active || !task.accepted || task.root!=task.id)
        return reject("capacity_safety_or_task_pause");
    ProfessionJob job;UnsettledClaimBatch claims;ItemGainSpec need;
    if(!DecodeProfessionJob(task.checkpoint.data,job,blocker) ||
        !sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,blocker) ||
        !NeededCapacity(actor,task,job,claims,need,blocker))return false;
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!view || !view->ready)return reject("capacity_reservations_unavailable");
    auto items=actor.GetPlayerbotAI()->InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    items.sort([](const Item* a,const Item* b){return a->GetGUIDLow()<b->GetGUIDLow();});
    Item* selected=nullptr;
    for(auto* item : items) {
        if(!item)continue;
        auto facts=Facts(actor,job,*item);uint32_t price=0;
        if(!QuoteCapacitySale(facts,price) || view->HasUncertainItem(task.actor,facts.entry))continue;
        ResourceClaim own;
        for(const auto& c : claims.claims) if(c.itemGuid==facts.guid) {
            if(!own.id.empty() || !ExactCapacityClaim(c,facts,task.id))return reject("capacity_stack_claim_requires_reconciliation");
            own=c;
        }
        const auto protectedAmount=view->ProtectedItem(facts.guid);
        if(protectedAmount!=(own.id.empty()?0:facts.quantity))continue;
        // Prefer a previously reserved candidate, so restart never abandons
        // one capacity claim to reserve another identical-looking stack.
        if(!selected || !own.id.empty()) {
            selected=item;held=own;q.item=facts;q.copper=price;
            if(!own.id.empty())break;
        }
    }
    if(!selected)return reject("capacity_no_safely_disposable_stack");
    if(!actor.IsStopped())return reject("capacity_vendor_travel_required");
    for(const auto guid : actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get())
        if(auto* vendor=actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_VENDOR)) {
            q.vendor=guid.GetRawValue();q.vendorEntry=vendor->GetEntry();break;
        }
    if(!q.vendor)return reject("capacity_vendor_travel_required");
    q.from=selected->GetPos();q.countBefore=actor.GetItemCount(q.item.entry,false);
    q.capacityEntry=need.entry;q.capacityQuantity=need.quantity;blocker.clear();return true;
}
bool NativeCapacityReservation::ValidatePurpose(Player& actor,const ReservationRequest& r,std::string& blocker) {
    const auto task=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    NativeSaleQuote q;ResourceClaim held;
    if(!task || task->revision!=r.transition.expectedRevision || task->phase!=Phase::Preparing ||
        r.changes.size()!=1 || r.changes[0].expectedRevision || r.changes[0].after.revision!=1 ||
        !PlanNativeCapacitySale(actor,*task,q,held,blocker) || !held.id.empty() ||
        !ExactCapacityClaim(r.changes[0].after,q.item,task->root)) {
        if(blocker.empty())blocker="capacity_exact_reservation_required";return false;
    }
    blocker.clear();return true;
}
bool NativeVendorSale::ValidateNative(Player& actor,const OperationRequest& r,std::string& blocker) {
    const auto task=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    NativeSaleQuote current;ResourceClaim held;
    if(!task || r.consumption.size()!=1 || !r.itemGain.Empty() || !r.itemTransfer.id.empty() ||
        r.beforeState!=EncodeNativeSaleQuote(quote) ||
        !PlanNativeCapacitySale(actor,*task,current,held,blocker) ||
        EncodeNativeSaleQuote(current)!=EncodeNativeSaleQuote(quote) || held.id.empty() ||
        !SameResourceClaim(held,r.consumption.front().before) || r.consumption.front().used!=quote.item.quantity) {
        if(blocker.empty())blocker="capacity_native_quote_changed";return false;
    }
    blocker.clear();return true;
}
NativeObservation NativeVendorSale::ExecuteNative(Player& actor,const OperationRequest& r) {
    NativeObservation out;std::string blocker;
    if(!ValidateNative(actor,r,blocker)){out.state=OperationState::Rejected;out.evidence=blocker;return out;}
    WorldPacket packet;packet<<ObjectGuid(quote.vendor)<<ObjectGuid(HIGHGUID_ITEM,quote.item.guid)<<uint8_t(0);
    actor.GetSession()->HandleSellItemOpcode(packet); // Real whole-stack sale, no cheat-money reset.
    NativePurchaseEpoch().Changed(actor.GetGUIDLow());
    const Item* bought=nullptr;
    for(uint32_t slot=BUYBACK_SLOT_START;slot<BUYBACK_SLOT_END;++slot)
        if(const auto* item=actor.GetItemFromBuyBackSlot(slot))
            if(item->GetGUIDLow()==quote.item.guid){bought=item;break;}
    const auto count=actor.GetItemCount(quote.item.entry,false);
    const bool inBags=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item.guid))!=nullptr;
    const bool empty=actor.GetItemByPos(quote.from)==nullptr;
    out.nativeReference="vendor_sale:"+std::to_string(quote.item.guid);
    out.afterState="{\"guid\":"+std::to_string(quote.item.guid)+",\"entry\":"+std::to_string(quote.item.entry)+
        ",\"quantity_sold\":"+std::to_string(quote.item.quantity)+",\"money\":"+std::to_string(actor.GetMoney())+
        ",\"entry_count\":"+std::to_string(count)+",\"slot_empty\":"+(empty?"true":"false")+
        ",\"buyback_guid\":"+std::to_string(bought?bought->GetGUIDLow():0)+'}';
    if(VerifyCapacitySale(quote.item,actor.GetMoney(),quote.countBefore,count,empty,inBags,
        bought?bought->GetGUIDLow():0,bought?bought->GetEntry():0,bought?bought->GetCount():0)) {
        out.state=OperationState::Verified;out.evidence="native_capacity_sale_money_item_and_slot_observed";
    } else if(inBags && count==quote.countBefore && actor.GetMoney()==quote.item.money) {
        out.state=OperationState::Rejected;out.evidence="native_capacity_sale_rejected_without_effect";
    } else out.evidence="native_capacity_sale_requires_reconciliation";
    auto* context=actor.GetPlayerbotAI()->GetAiObjectContext();
    for(const auto& name : context->GetValues()) {
        const auto base=name.substr(0,name.find("::"));
        if(base=="bag space" || base=="item count" || base=="inventory items" || base=="inventory item ids" || base=="item usage")
            if(auto* value=context->GetUntypedValue(name))value->Reset();
    }
    return out;
}
std::string NativeVendorSale::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& outcome) const {
    std::string proof="SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+" FROM characters WHERE guid="+
        std::to_string(actor.GetGUIDLow())+" AND money="+std::to_string(actor.GetMoney());
    if(const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item.guid)))
        return proof+" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
            std::to_string(actor.GetGUIDLow())+" AND v.item="+std::to_string(quote.item.guid)+" AND i.itemEntry="+
            std::to_string(item->GetEntry())+" AND i.count="+std::to_string(item->GetCount())+')';
    return proof+" AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+std::to_string(quote.item.guid)+")"+
        " AND NOT EXISTS(SELECT 1 FROM item_instance WHERE guid="+std::to_string(quote.item.guid)+")";
}
}
