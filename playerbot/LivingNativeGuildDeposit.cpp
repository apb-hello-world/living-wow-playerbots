#include "botpch.h"
#include "LivingNativeGuildDeposit.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingServiceExecution.h"
#include "PlayerbotGuildSupplies.h"
#include "PlayerbotGuildGovernance.h"
#include "PlayerbotActionBroker.h"
#include "Guilds/GuildMgr.h"
#include "strategy/values/ItemUsageValue.h"

namespace LivingActivity {
namespace {
bool Safe(Player& actor) {
    return actor.GetPlayerbotAI() && actor.GetSession() && actor.IsInWorld() && actor.GetMap() && actor.IsAlive() &&
        !actor.IsBeingTeleported() && !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        actor.IsStopped() && !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor) && !actor.GetTradeData();
}
bool FreshGoal(const GuildDepositQuote& q) {
    // Indexed transition-time reads (intent and actual dispatch), never a
    // per-tick scan. The same predicates are repeated by the transaction proof.
    auto result=CharacterDatabase.PQuery("SELECT d.deposited_quantity FROM guild_society_supply_delivery d "
        "JOIN guild_society_supply_goal g ON g.goal_id=d.goal_id AND g.guild_id=d.guild_id "
        "JOIN guild_society_supply_execution e ON e.guild_id=d.guild_id WHERE d.delivery_id=%llu "
        "AND d.guild_id=%u AND d.goal_id='%s' AND d.donor_guid=%u AND d.carrier_guid=%u AND d.item_entry=%u "
        "AND d.quantity=%u AND d.mail_id=%u AND d.phase='carried' AND g.state='active' AND g.request_kind='item' "
        "AND g.item_entry=%u AND g.required_quantity=%u AND g.reserved_quantity=%u AND e.enabled=1",
        (unsigned long long)q.job.delivery,q.job.guild,q.job.goal.c_str(),q.job.donor,q.actor,q.job.entry,
        q.job.quantity,q.job.incomingMail,q.job.entry,q.goalTarget,q.goalReserved);
    return result && result->Fetch()[0].GetUInt32()==q.deposited;
}
uint32_t GuildCount(Guild& guild,uint32_t entry) {
    const auto counts=guild.GetBankItemCounts();const auto found=counts.find(entry);
    return found==counts.end()?0:found->second;
}
}
bool PlanNativeGuildDeposit(Player& actor,const Task& task,GuildDepositQuote& q,ResourceClaim& held,std::string& blocker) {
    q={};held={};auto reject=[&](const char* why){blocker=why;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || task.actor!=actor.GetGUIDLow() || !Safe(actor))
        return reject("guild_delivery_safety_or_arrival_wait");
    if(!sGuildSupplies.ReadManagedDeposit(task,q,blocker))return false;
    if(sLivingActivityCoordinator.DefersGuildMutation(q.job.guild,q.actor))return reject("guild_bank_native_save_pending");
    auto* guild=sGuildMgr.GetGuildById(q.job.guild);
    if(!guild || actor.GetGuildId()!=q.job.guild)return reject("guild_delivery_membership_changed");
    UnsettledClaimBatch batch;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,batch,blocker))return false;
    auto items=actor.GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    items.sort([](const Item* a,const Item* b){return a->GetGUIDLow()<b->GetGUIDLow();});
    Item* selected=nullptr;const auto maximum=q.amount;
    const auto tradeReservations=sPlayerbotActionBroker.ReservedItemsView();
    const char* unavailable="guild_delivery_reserved_stack_unavailable";
    for(auto* item:items) {
        if(!item || item->GetEntry()!=q.job.entry || (!q.job.incomingMail && item->GetGUIDLow()!=q.item) ||
            !item->CanBeTraded() || item->IsConjuredConsumable() ||
            ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))continue;
        if(!tradeReservations || tradeReservations->Item(item->GetGUIDLow())) {
            unavailable="guild_delivery_private_trade_reserved";continue;
        }
        // Inspect the exact saved delivery before its first claim exists. The
        // broker's aggregate IsItemReserved also includes this delivery's own
        // legacy protection and would otherwise exclude its assigned stack.
        ResourceClaim proposed;proposed.task=task.id;proposed.actor=task.actor;
        proposed.itemGuid=item->GetGUIDLow();proposed.itemEntry=item->GetEntry();
        proposed.quantity=std::min(maximum,item->GetCount());proposed.state="held";proposed.location="bags";
        if(!sGuildSupplies.AllowsManagedClaim(proposed)) {
            unavailable="guild_delivery_reservation_owner_mismatch";continue;
        }
        ResourceClaim own;
        for(const auto& claim:batch.claims) if(claim.itemGuid==item->GetGUIDLow()) {
            if(!own.id.empty() || !sGuildSupplies.AllowsManagedClaim(claim))return reject("guild_delivery_claim_requires_reconciliation");
            own=claim;
        }
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bags"},available,blocker))return false;
        if(!available || (!own.id.empty() && own.quantity>available))continue;
        if(!selected || !own.id.empty()) {
            selected=item;held=own;q.amount=std::min(maximum,available);
            if(!own.id.empty())break;
        }
    }
    if(!selected)return reject(unavailable);
    q.item=selected->GetGUIDLow();q.itemCount=selected->GetCount();q.position=selected->GetPos();
    q.bagCount=actor.GetItemCount(q.job.entry,false);q.money=actor.GetMoney();
    if(!held.id.empty())q.amount=std::min(q.amount,uint32_t(held.quantity));
    const auto tab=guild->FindSupplyDepositTab(q.actor,selected,q.amount);
    if(tab<0)return reject("guild_delivery_bank_full_or_permission_denied");q.tab=uint8_t(tab);
    for(const auto guid:actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest game objects no los")->Get())
        if(actor.GetGameObjectIfCanInteractWith(guid,GAMEOBJECT_TYPE_GUILD_BANK)){q.bank=guid.GetRawValue();break;}
    if(!q.bank)return reject("guild_bank_travel_required");
    if(!ValidGuildDepositQuote(q))return reject("guild_delivery_quote_invalid");
    blocker.clear();return true;
}
bool NativeGuildDeliveryReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    GuildDepositQuote q;ResourceClaim held;
    if(!saved || saved->revision!=request.transition.expectedRevision || request.changes.size()!=1 ||
        request.changes[0].expectedRevision || request.changes[0].after.revision!=1 ||
        !PlanNativeGuildDeposit(actor,*saved,q,held,blocker) || !held.id.empty() ||
        !ExactGuildDepositClaim(q,request.changes[0].after,saved->root) || request.changes[0].after.quantity!=q.amount) {
        if(blocker.empty())blocker="guild_delivery_exact_reservation_required";return false;
    }
    return FreshGoal(q) ? true : (blocker="guild_delivery_goal_changed",false);
}
bool NativeGuildDeposit::ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    GuildDepositQuote current;ResourceClaim held;
    if(!saved || request.beforeState!=EncodeGuildDepositQuote(quote) || request.consumption.size()!=1 ||
        !request.itemGain.Empty() || !request.itemTransfer.id.empty() || !request.mailGain.Empty() ||
        !PlanNativeGuildDeposit(actor,*saved,current,held,blocker) ||
        EncodeGuildDepositQuote(current)!=EncodeGuildDepositQuote(quote) ||
        !ExactGuildDepositClaim(quote,held,saved->root) || !SameResourceClaim(held,request.consumption.front().before) ||
        request.consumption.front().used!=quote.amount || !FreshGoal(quote)) {
        if(blocker.empty())blocker="guild_delivery_native_quote_changed";return false;
    }
    blocker.clear();return true;
}
NativeObservation NativeGuildDeposit::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string blocker;
    if(!ValidateNative(actor,request,blocker)){out.state=OperationState::Rejected;out.evidence=blocker;return out;}
    if(!CharacterDatabase.HasOpenTransaction() || !sGuildSupplies.BeginManagedDeposit(quote)) {
        out.state=OperationState::Rejected;out.evidence="guild_delivery_native_transaction_required";return out;
    }
    struct End {~End(){sGuildSupplies.EndManagedDeposit();}} end;
    auto* guild=sGuildMgr.GetGuildById(quote.job.guild);
    guild->MoveFromCharToBank(&actor,uint8_t(quote.position>>8),uint8_t(quote.position),quote.tab,255,quote.amount);
    const auto* source=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    const uint32_t sourceAfter=source?source->GetCount():0,bagsAfter=actor.GetItemCount(quote.job.entry,false);
    const auto bankAfter=GuildCount(*guild,quote.job.entry);
    out.nativeReference="guild_delivery:"+std::to_string(quote.job.delivery);
    out.afterState="{\"source_count\":"+std::to_string(sourceAfter)+",\"bag_count\":"+std::to_string(bagsAfter)+
        ",\"bank_count\":"+std::to_string(bankAfter)+",\"money\":"+std::to_string(actor.GetMoney())+'}';
    if(VerifyGuildDeposit(quote,sourceAfter,bagsAfter,bankAfter,actor.GetMoney())) {
        out.state=OperationState::Verified;out.evidence="native_guild_deposit_items_and_stock_observed";
    } else if(sourceAfter==quote.itemCount && bagsAfter==quote.bagCount && bankAfter==quote.bankCount && actor.GetMoney()==quote.money) {
        out.state=OperationState::Rejected;out.evidence="native_guild_deposit_rejected_without_effect";
    } else out.evidence="native_guild_deposit_requires_reconciliation";
    return out;
}
std::string NativeGuildDeposit::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& outcome) const {
    auto* guild=sGuildMgr.GetGuildById(quote.job.guild);
    const auto bankAfter=guild?GuildCount(*guild,quote.job.entry):UINT32_MAX;
    const auto* source=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    const uint32_t count=source?source->GetCount():0;
    return GuildDepositNativeProof(quote,outcome,count,actor.GetItemCount(quote.job.entry,false),bankAfter,actor.GetMoney());
}
}
