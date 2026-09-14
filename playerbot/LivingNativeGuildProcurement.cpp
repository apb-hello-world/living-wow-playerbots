#include "botpch.h"
#include "LivingNativeGuildProcurement.h"
#include "LivingActivityCoordinator.h"
#include "LivingNativeBankWithdrawal.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "PlayerbotInventoryPressure.h"
#include "strategy/values/ItemUsageValue.h"
#include "strategy/values/BudgetValues.h"
#include "LivingProfessionVendor.h"
#include "LivingProfessionNative.h"
#include "strategy/actions/AhAction.h"
#include "LivingNativeAuctionPurchase.h"
#include "TravelMgr.h"
#include "LivingNativeGathering.h"

namespace LivingActivity {
bool ReadNativeGuildCraftSource(Player& actor,uint32_t entry,uint32_t maximum,
    NativeGuildProcurementSource& out,std::string& why) {
    out={};std::vector<uint32_t> recipes;
    if(!NativeRequestedItemRecipes(entry,recipes,why) || !actor.GetPlayerbotAI())return false;
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!view || !view->ready){why="guild_craft_reservations_unavailable";return false;}
    struct Stock {uint64_t quantity=0;bool protectedStock=false;};
    std::map<uint32_t,Stock> stock;unsigned scanned=0;
    for(const bool bank:{false,true})for(auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",
        bank?IterateItemsMask::ITERATE_ITEMS_IN_BANK:IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
        if(++scanned>256){why="guild_craft_inventory_snapshot_bound";return false;}
        if(!item)continue;
        auto& have=stock[item->GetEntry()];have.quantity+=item->GetCount();
        have.protectedStock|=item->GetOwnerGuid()!=actor.GetObjectGuid() || item->IsInTrade() ||
            sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
            view->UnreservedItem(actor.GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount())!=item->GetCount();
    }
    const uint32_t money=std::min(actor.GetMoney(),actor.GetPlayerbotAI()->GetAiObjectContext()
        ->GetValue<uint32_t>("free money for",uint32_t(ai::NeedMoneyFor::tradeskill))->Get());
    std::string failure="guild_craft_recipe_not_known";
    bool knownCandidate=false;
    for(const auto recipe:recipes) {
        ProfessionJob job;std::string blocker;
        if(!BuildNativeRequestedItemJob(actor,recipe,entry,maximum,job,blocker)) {
            if(!knownCandidate && blocker!="profession_recipe_not_known")failure=blocker;
            continue;
        }
        knownCandidate=true;
        uint64_t cost=0;bool ready=true;
        for(const auto& r:job.reagents) {
            const auto have=stock[r.entry];
            if(have.protectedStock || view->HasUncertainItem(actor.GetGUIDLow(),r.entry) ||
                sGuildSupplies.ReservedEntry(actor.GetGUIDLow(),r.entry) || ai::ItemUsageValue::IsNeededForQuest(&actor,r.entry,true) ||
                ai::AhBidAction::HasPendingMaterial(&actor,r.entry)) {
                failure="guild_craft_material_protected_or_incoming:"+std::to_string(r.entry);ready=false;break;
            }
            if(have.quantity>=r.perAttempt)continue;
            const uint32_t missing=r.perAttempt-uint32_t(have.quantity);
            const auto* proto=sObjectMgr.GetItemPrototype(r.entry);
            uint64_t price=UINT64_MAX;std::vector<int32_t> vendors;
            if(proto && proto->BuyCount && proto->BuyPrice) {
                const uint64_t units=(uint64_t(missing)+proto->BuyCount-1)/proto->BuyCount;
                if(units<=255 && units*proto->BuyCount<=10000 &&
                    NativeProfessionVendorSources(actor,r.entry,uint32_t(units*proto->BuyCount),vendors,blocker) &&
                    NativePurchaseServiceAvailable(actor,uint32_t(ai::TravelDestinationPurpose::Vendor),vendors))price=units*proto->BuyPrice;
            }
            std::vector<NativeAuctionOffer> offers;
            AuctionMaterialBasket basket;
            if(NativeAuctionOffers(actor,r.entry,missing,offers,blocker) &&
                NativePurchaseServiceAvailable(actor,uint32_t(ai::TravelDestinationPurpose::AH),{}) &&
                ExactAuctionMaterialBasket(offers,r.entry,missing,money,basket))
                price=std::min(price,uint64_t(basket.copper));
            if(price>money || cost>money-price){
                failure=(price==UINT64_MAX?"guild_craft_material_no_feasible_source:":"guild_craft_material_budget_protected:")+
                    std::to_string(r.entry);ready=false;break;
            }cost+=price;
        }
        if(!ready || (!out.craft.empty() && (out.estimatedCopper<cost ||
            (out.estimatedCopper==cost && out.reference<recipe))))continue;
        out={job.outputQuantity,recipe,uint32_t(cost),"craft",EncodeProfessionJob(job)};
    }
    if(out.craft.empty()){why=failure;return false;}
    why.clear();return true;
}
bool NativeGuildProcurementItemUsable(Player& actor,uint32_t entry,Item* item,bool alreadyClaimed) {
    if(!item || item->GetEntry()!=entry || item->GetOwnerGuid()!=actor.GetObjectGuid() ||
        !item->CanBeTraded() || item->IsConjuredConsumable() || item->IsInTrade() || item->IsSoulBound() ||
        sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
        sGuildSupplies.ReservedEntry(actor.GetGUIDLow(),entry) || ai::ItemUsageValue::IsNeededForQuest(&actor,entry,true))return false;
    if(alreadyClaimed)return true;
    bool reserved=false;const auto disposition=sPlayerbotInventoryPressure.Classify(&actor,item,&reserved);
    return !reserved && (disposition==LivingWowItemDisposition::Vendor || disposition==LivingWowItemDisposition::Auction);
}
bool ReadNativeGuildProcurementSource(Player& actor,uint32_t entry,uint32_t maximum,
    NativeGuildProcurementSource& out,std::string& why) {
    out={};auto reject=[&](const char* code){why=code;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.IsInWorld() || !maximum)
        return reject("guild_procurement_source_snapshot_unavailable");
    const auto* proto=sObjectMgr.GetItemPrototype(entry);
    if(!proto)return reject("guild_procurement_item_unavailable");
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!view || !view->ready)return reject("guild_procurement_reservations_unavailable");
    const uint32_t batch=std::min(maximum,std::min(64u,std::max<uint32_t>(1u,proto->Stackable)));
    unsigned scanned=0;
    for(bool bank:{false,true}) {
        for(auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",bank?IterateItemsMask::ITERATE_ITEMS_IN_BANK:IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
            if(++scanned>256)return reject("guild_procurement_inventory_snapshot_limit");
            if(!NativeGuildProcurementItemUsable(actor,entry,item,false) ||
                view->UnreservedItem(actor.GetGUIDLow(),item->GetGUIDLow(),entry,item->GetCount())!=item->GetCount())continue;
            out={std::min(batch,item->GetCount()),item->GetGUIDLow(),0,bank?"bank":"owned"};why.clear();return true;
        }
    }
    const uint32_t money=std::min(actor.GetMoney(),actor.GetPlayerbotAI()->GetAiObjectContext()
        ->GetValue<uint32_t>("free money for",uint32_t(ai::NeedMoneyFor::tradeskill))->Get());
    NativeGuildProcurementSource crafted;std::string craftBlocker;
    ReadNativeGuildCraftSource(actor,entry,batch,crafted,craftBlocker);
    if(!crafted.craft.empty() && !crafted.estimatedCopper){out=crafted;why.clear();return true;}
    std::vector<int32_t> nodes;uint32_t purpose=0;std::string gatherBlocker;
    if(NativeGatherSources(actor,entry,nodes,purpose,gatherBlocker) &&
        !sTravelMgr.GetDestinations(ai::PlayerTravelInfo(&actor),purpose,nodes,false,0,true).empty()) {
        // A loot table proves a possible source, not its rolled quantity. Only
        // assign one needed unit; the real indivisible slot may yield surplus.
        out={1,uint32_t(-nodes.front()),0,"gather"};why.clear();return true;
    }
    if(!money)return reject("guild_procurement_personal_budget_protected");
    std::string vendorBlocker;std::vector<int32_t> vendors;
    if(proto->BuyCount && proto->BuyPrice) {
        const auto units=std::min((uint64_t(batch)+proto->BuyCount-1)/proto->BuyCount,uint64_t(money/proto->BuyPrice));
        const auto quantity=units*proto->BuyCount,price=units*proto->BuyPrice;
        if(quantity && quantity<=UINT32_MAX && price<=money && NativeProfessionVendorSources(actor,entry,uint32_t(quantity),vendors,vendorBlocker) &&
            NativePurchaseServiceAvailable(actor,uint32_t(ai::TravelDestinationPurpose::Vendor),vendors)) {
            const auto selected=NearestNativePurchaseEntries(actor,uint32_t(ai::TravelDestinationPurpose::Vendor),vendors);
            out={std::min(batch,uint32_t(quantity)),uint32_t(selected.front()),uint32_t(price),"vendor"};why.clear();return true;
        }
    }
    std::vector<NativeAuctionOffer> offers;
    if(NativeAuctionOffers(actor,entry,batch,offers,why)) {
        for(const auto& offer:offers)if(offer.copper<=money &&
            NativePurchaseServiceAvailable(actor,uint32_t(ai::TravelDestinationPurpose::AH),{})) {
            out={offer.quantity,offer.id,offer.copper,"auction"};why.clear();return true;
        }
    }
    if(!crafted.craft.empty()){out=crafted;why.clear();return true;}
    if(why=="profession_purchase_market_snapshot_busy" || vendorBlocker=="profession_vendor_catalog_not_ready")return false;
    if(!craftBlocker.empty() && craftBlocker!="guild_craft_no_item_recipe") {why=craftBlocker;return false;}
    return reject("guild_procurement_no_obtainable_source_within_budget");
}
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
        const bool alreadyClaimed=std::any_of(claims.claims.begin(),claims.claims.end(),[&](const auto& c){
            return c.itemGuid==item->GetGUIDLow() && c.itemEntry==job.entry && c.state=="held";
        });
        if(!NativeGuildProcurementItemUsable(actor,job.entry,item,alreadyClaimed))continue;
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
