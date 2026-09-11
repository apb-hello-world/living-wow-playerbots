#include "botpch.h"
#include "LivingProfessionDemand.h"
#include "LivingActivityCoordinator.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "Mails/Mail.h"
#include <algorithm>
#include <map>
#include <limits>
#include <mutex>
#include <set>

namespace LivingActivity {
    bool InspectNativeProfessionDemand(Player& actor,const Task& saved,NativeProfessionDemand& demand) {
        demand={};
        auto reject=[&](const char* why,uint64_t reference=0){demand.blocker=why; demand.nativeReference=reference; return false;};
        if (!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.IsInWorld() ||
            actor.IsBeingTeleported() || actor.GetGUIDLow()!=saved.actor || !IsProfessionJob(saved))
            return reject("profession_demand_native_context_unavailable");
        ProfessionJob job;
        if (!DecodeProfessionJob(saved.checkpoint.data,job,demand.blocker)) return false;
        std::map<uint32_t,ProfessionStock> stock;
        for (const auto& reagent : job.reagents) stock.emplace(reagent.entry,ProfessionStock{reagent.entry});
        unsigned scanned=0; std::set<uint32_t> seen;
        for (unsigned bank=0;bank!=2;++bank) {
            for (Item* item : actor.GetPlayerbotAI()->InventoryParseItems("inventory",bank ?
                IterateItemsMask::ITERATE_ITEMS_IN_BANK : IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
                if (++scanned>256) return reject("profession_inventory_snapshot_limit");
                if (!item || !stock.count(item->GetEntry())) continue;
                if (item->GetOwnerGuid()!=actor.GetObjectGuid() || !seen.insert(item->GetGUIDLow()).second)
                    return reject("profession_inventory_identity_unresolved",item->GetGUIDLow());
                // Existing reservations stay protected during migration. They
                // cannot be reclassified as missing stock and purchased again.
                if (sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
                    sGuildSupplies.ReservedEntry(saved.actor,item->GetEntry()) ||
                    ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))
                    return reject("profession_material_has_legacy_commitment",item->GetGUIDLow());
                uint32_t available=0;
                if (!sLivingActivityCoordinator.TaskResourceAvailability(saved.id,saved.revision,
                    {saved.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,bank ? "bank" : "bags"},available,demand.blocker))
                    return false;
                auto& count=bank ? stock.at(item->GetEntry()).bank : stock.at(item->GetEntry()).bag;
                if (uint64_t(count)+available>std::numeric_limits<uint32_t>::max()) return reject("profession_stock_overflow");
                count+=available;
            }
        }
        // The pinned core loads mail and attachment metadata before completing
        // login. Undelivered native attachments are present, not bag contents.
        if (actor.GetMailSize()>256) return reject("profession_mail_snapshot_limit");
        for (auto it=actor.GetMailBegin();it!=actor.GetMailEnd();++it) {
            const auto* mail=*it;
            if (!mail || mail->state==MAIL_STATE_DELETED || mail->expire_time<=time(nullptr)) continue;
            for (const auto& attachment : mail->items) if (stock.count(attachment.item_template))
                return reject(mail->COD ? "profession_cod_material_requires_explicit_acceptance" :
                    "profession_incoming_material_requires_reconciliation",mail->messageID);
        }
        // Native standing bids are an existing commitment too. This bounded
        // inspection uses the same market mutex; it does not scan the DB or
        // turn an unwon auction into received materials.
        std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex,std::try_to_lock);
        if (!lock.owns_lock()) return reject("profession_purchase_market_snapshot_busy");
        scanned=0;
        for (unsigned house=0;house<MAX_AUCTION_HOUSE_TYPE;++house) {
            for (const auto& entry : sAuctionMgr.GetAuctionsMap(AuctionHouseType(house))->GetAuctions()) {
                if (++scanned>4096) return reject("profession_auction_snapshot_limit");
                const auto* auction=entry.second;
                if (auction && auction->bidder==saved.actor && stock.count(auction->itemTemplate) && auction->expireTime>time(nullptr))
                    return reject("profession_material_bid_pending",auction->Id);
            }
        }
        for (const auto& row : stock) demand.stock.push_back(row.second);
        return true;
    }
    bool NativeProfessionPurchasePrerequisites::ValidateCommittedDemandAndBudget(Player& actor,
        const OperationRequest& request,const NativeVendorQuote& quote,std::string& blocker) const {
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
        auto reject=[&](const char* why){blocker=why; return false;};
        if (!saved || saved->actor!=actor.GetGUIDLow() || saved->checkpoint.data!=request.transition.task.checkpoint.data ||
            (saved->revision!=request.transition.expectedRevision && saved->revision!=request.transition.task.revision))
            return reject("profession_purchase_saved_intent_changed");
        ProfessionJob job;
        if (!DecodeProfessionJob(saved->checkpoint.data,job,blocker)) return false;
        if (job.purpose==ProfessionPurpose::SkillGain && actor.GetSkillValuePure(job.skill)>=job.targetSkill)
            return reject("profession_target_met_requires_settlement");
        NativeProfessionDemand demand;
        if (!InspectNativeProfessionDemand(actor,*saved,demand)) {blocker=demand.blocker; return false;}
        for (size_t i=0;i<job.reagents.size();++i) {
            if (demand.stock[i].bank && demand.stock[i].bag<job.reagents[i].perAttempt)
                return reject("profession_banked_material_requires_collection");
        }
        const auto reagent=std::find_if(job.reagents.begin(),job.reagents.end(),[&](const auto& r){return r.entry==quote.entry;});
        if (reagent==job.reagents.end()) return reject("profession_purchase_material_not_required");
        const auto& have=demand.stock[size_t(reagent-job.reagents.begin())];
        const auto* proto=sObjectMgr.GetItemPrototype(quote.entry);
        if (!proto) return reject("profession_purchase_native_item_unavailable");
        // Buy only the next attempt's unmet demand, rounded once for a real
        // native vendor bundle. Any surplus remains claimed to this same job.
        uint32_t rounded=0;
        if (!RequiredProfessionVendorQuantity(*reagent,have,proto->BuyCount,rounded,blocker)) return false;
        if (quote.quantity!=rounded) return reject("profession_purchase_unpaid_quantity_changed");
        return ValidateNativeProfessionBudget(actor,*saved,request.transition.receipt,quote.copper,blocker);
    }
}
