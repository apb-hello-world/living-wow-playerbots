#include "botpch.h"
#include "LivingNativeCommissionTrade.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"

namespace LivingActivity {
bool PlanNativeCommissionTrade(Player& actor,const Task& task,CommissionTradeQuote& q,
    std::vector<ClaimConsumption>& uses,std::string& why) {
    q={};uses.clear();auto reject=[&](const char* code){why=code;return false;};
    CommissionJob job;ProfessionJob recipe;
    if(!sLivingActivityCoordinator.OnWorldThread() || !Validate(task,why) || !IsCommissionJob(task) ||
        !ValidateCommissionTask(task,why) || task.actor!=actor.GetGUIDLow() || !task.accepted || task.mode!=Mode::Active || Terminal(task.phase) ||
        !DecodeCommissionJob(task.checkpoint.data,job,why) || !job.craftFinishedRevision ||
        (job.agreement.delivery!="direct" && job.agreement.delivery!="meeting") ||
        !DecodeProfessionIntent(job.craft,recipe,why))return reject("commission_trade_task_invalid");
    auto* customer=actor.GetTrader();
    if(!actor.GetPlayerbotAI() || !customer || customer->GetGUIDLow()!=job.agreement.recipient ||
        customer->GetTrader()!=&actor || !actor.GetTradeData() || !customer->GetTradeData())
        return reject("commission_trade_recipient_window_required");
    for(auto* person:{&actor,customer}) {
        if(!person->GetSession() || !person->IsInWorld() || !person->GetMap())return reject("commission_trade_actor_unavailable");
        const auto safety=ReadNativeSafety(*person,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
        if(safety)return reject(NativeSafetyReason(safety));
        if(person->GetMap()->IsDungeon())return reject("commission_trade_dungeon_deferred");
        if(!person->IsStopped() || person->IsNonMeleeSpellCasted(false))return reject("commission_trade_actor_busy");
    }
    if(actor.GetTeam()!=customer->GetTeam() || !actor.IsWithinDistInMap(customer,TRADE_DISTANCE,false))
        return reject("commission_trade_recipient_out_of_reach");
    auto* offered=actor.GetTradeData();auto* payment=customer->GetTradeData();
    if(offered->IsAccepted() || !payment->IsAccepted())return reject("commission_trade_customer_acceptance_required");
    if(offered->GetMoney() || payment->GetMoney()!=job.agreement.feeCopper ||
        offered->GetSpell() || payment->GetSpell() || offered->GetItem(TRADE_SLOT_NONTRADED))
        return reject("commission_trade_offer_changed");
    for(uint8_t slot=0;slot<TRADE_SLOT_COUNT;++slot)
        if(payment->GetItem(TradeSlots(slot)))return reject("commission_trade_unexpected_customer_item");
    const auto protection=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!protection || !protection->ready ||
        protection->UnreservedMoney(customer->GetGUIDLow(),customer->GetMoney())<job.agreement.feeCopper ||
        uint64_t(job.agreement.feeCopper)+sPlayerbotActionBroker.ReservedCopper(customer->GetGUIDLow())>customer->GetMoney() ||
        uint64_t(actor.GetMoney())+job.agreement.feeCopper>MAX_MONEY_AMOUNT)
        return reject("commission_trade_fee_unavailable");
    // Durable customer agreement and actual craft proof, not a checkpoint flag.
    auto agreement=CharacterDatabase.PQuery("SELECT c.authoritative_payload FROM organic_economy_commission c "
        "WHERE c.commission_id='%s' AND c.bot_guid=%u AND c.player_guid=%u AND c.state IN ('crafting','ready','traveling') "
        "AND EXISTS(SELECT 1 FROM living_activity_transition t WHERE t.task_id='%s' AND t.task_revision=%llu "
        "AND t.code='commission_craft_verified') AND NOT EXISTS(SELECT 1 FROM living_activity_operation o "
        "WHERE o.task_id='%s' AND o.kind IN ('commission_trade','commission_mail_send') AND o.state='verified')",
        job.agreement.id.c_str(),task.actor,job.agreement.recipient,task.id.c_str(),
        (unsigned long long)job.craftFinishedRevision,task.id.c_str());
    CommissionContract accepted;
    if(!agreement || !DecodeCommissionContract(agreement->Fetch()[0].GetCppString(),accepted,why) ||
        EncodeCommissionContract(accepted)!=EncodeCommissionContract(job.agreement))
        return reject("commission_trade_agreement_or_craft_changed");
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,why))return false;
    if(!claims.complete)return reject("commission_trade_claims_incomplete");
    for(const auto& c:claims.claims) {
        if(c.quantity>UINT32_MAX)return reject("commission_trade_claim_quantity_invalid");
        uses.push_back({c,uint32_t(c.quantity)});
    }
    q={task.actor,job.agreement.recipient,recipe.outputEntry,job.agreement.feeCopper,actor.GetMoney(),customer->GetMoney(),{}};
    const auto privateItems=sPlayerbotActionBroker.ReservedItemsView();
    for(uint8_t slot=0;slot<TRADE_SLOT_TRADED_COUNT;++slot) {
        const auto* item=offered->GetItem(TradeSlots(slot));if(!item)continue;
        if(item->GetOwnerGuid()!=actor.GetObjectGuid() || !Player::IsInventoryPos(item->GetPos()) ||
            item->GetEntry()!=recipe.outputEntry || !item->CanBeTraded() || item->HasGeneratedLoot() ||
            item->IsConjuredConsumable() || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
            sGuildSupplies.Reserved(item->GetGUIDLow()) || !privateItems || privateItems->Item(item->GetGUIDLow()) ||
            ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))
            return reject("commission_trade_claimed_output_unavailable");
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bags"},available,why))return false;
        if(available!=item->GetCount())return reject("commission_trade_output_reserved_elsewhere");
        q.items.push_back({item->GetGUIDLow(),item->GetCount()});
    }
    std::sort(q.items.begin(),q.items.end(),[](const auto& a,const auto& b){return a.item<b.item;});
    // Whole, exact claimed stacks only. Never split or replace an offer after
    // the customer accepted it. Partial stacks require the preparation step.
    if(!ExactCommissionTradeConsumption(task,q,uses))return reject("commission_trade_exact_output_preparation_required");
    why.clear();return true;
}
bool NativeCommissionTrade::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    CommissionTradeQuote fresh;std::vector<ClaimConsumption> uses;
    if(!saved || request.beforeState!=EncodeCommissionTradeQuote(quote) ||
        !ExactCommissionTradeConsumption(*saved,quote,request.consumption) ||
        !PlanNativeCommissionTrade(actor,*saved,fresh,uses,why))return false;
    if(EncodeCommissionTradeQuote(fresh)!=EncodeCommissionTradeQuote(quote)) {
        why="commission_trade_native_quote_changed";return false;
    }
    return true;
}
NativeObservation NativeCommissionTrade::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string why;verified=unchanged=false;delivered.clear();
    if(!ValidateNative(actor,request,why)) {
        out.state=OperationState::Rejected;out.evidence=why.empty()?"commission_trade_validation_rejected":why;return out;
    }
    if(!CharacterDatabase.HasOpenTransaction()) {
        out.state=OperationState::Rejected;out.evidence="commission_trade_native_transaction_required";return out;
    }
    auto* customer=actor.GetTrader();
    NativeTradeCapture capture;WorldPacket accept(CMSG_ACCEPT_TRADE,4);accept<<uint32_t(0);
    actor.GetSession()->HandleAcceptTradeOpcode(accept);
    unchanged=capture.Valid() && !capture.Completed() && capture.Rows().empty() &&
        actor.GetMoney()==quote.actorMoney && customer->GetMoney()==quote.recipientMoney;
    for(const auto& part:quote.items) {
        const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,part.item));
        unchanged=unchanged && item && item->GetOwnerGuid()==actor.GetObjectGuid() &&
            item->GetEntry()==quote.entry && item->GetCount()==part.quantity;
    }
    if(unchanged) {out.state=OperationState::Rejected;out.evidence="commission_trade_native_handler_rejected";return out;}
    if(!VerifyCommissionTrade(quote,capture,why) || actor.GetTradeData() || customer->GetTradeData() ||
        !CharacterDatabase.HasOpenTransaction()) {out.evidence="commission_trade_native_custody_uncertain";return out;}
    std::map<uint16_t,TradeDestination> final;
    for(const auto& row:capture.Rows())for(const auto& d:row.destinations) {
        final[d.position]=d;
    }
    for(const auto& row:final) {
        const auto& d=row.second;const auto* item=customer->GetItemByPos(d.position);
        if(!item || item->GetGUIDLow()!=d.afterItem || item->GetOwnerGuid()!=customer->GetObjectGuid() ||
            item->GetEntry()!=quote.entry || item->GetCount()!=d.afterCount) {
            out.evidence="commission_trade_destination_changed";return out;
        }
        delivered.push_back({item->GetGUIDLow(),item->GetCount(),item->GetContainer()?item->GetContainer()->GetGUIDLow():0,item->GetSlot()});
    }
    try {out.afterState=EncodeCommissionTradeEvidence(quote,{capture.Rows(),capture.Completion()});}
    catch(const std::exception&){out.evidence="commission_trade_evidence_capacity";return out;}
    verified=true;out.state=OperationState::Verified;out.evidence="native_commission_trade_and_fee_observed";
    out.nativeReference="trade:"+request.transition.receipt;return out;
}
std::string NativeCommissionTrade::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& task) const {
    if(!verified && !unchanged)return {};
    std::string sql="SELECT "+SqlValue(task.id)+','+std::to_string(task.revision)+
        " FROM characters a JOIN characters b ON b.guid="+std::to_string(quote.recipient)+
        " WHERE a.guid="+std::to_string(quote.actor)+" AND a.money="+
        std::to_string(quote.actorMoney+(verified?quote.fee:0))+" AND b.money="+
        std::to_string(quote.recipientMoney-(verified?quote.fee:0));
    auto itemProof=[&](uint32_t owner,const Location& item) {
        return " AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
            std::to_string(owner)+" AND i.owner_guid=v.guid AND i.guid="+std::to_string(item.item)+
            " AND i.itemEntry="+std::to_string(quote.entry)+" AND i.count="+std::to_string(item.count)+
            " AND v.bag="+std::to_string(item.bag)+" AND v.slot="+std::to_string(item.slot)+")"+
            " AND (SELECT COUNT(*) FROM character_inventory v WHERE v.item="+std::to_string(item.item)+")=1"+
            " AND NOT EXISTS(SELECT 1 FROM mail_items m WHERE m.item_guid="+std::to_string(item.item)+")"+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item g WHERE g.item_guid="+std::to_string(item.item)+")"+
            " AND NOT EXISTS(SELECT 1 FROM auction x WHERE x.itemguid="+std::to_string(item.item)+")";
    };
    if(verified) {
        for(const auto& item:delivered)sql+=itemProof(quote.recipient,item);
        for(const auto& source:quote.items) {
            const bool survives=std::any_of(delivered.begin(),delivered.end(),[&](const auto& d){return d.item==source.item;});
            if(!survives)sql+=" AND NOT EXISTS(SELECT 1 FROM item_instance i WHERE i.guid="+std::to_string(source.item)+")";
            sql+=" AND NOT EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid=a.guid AND v.item="+
                std::to_string(source.item)+") AND NOT EXISTS(SELECT 1 FROM mail_items m WHERE m.item_guid="+
                std::to_string(source.item)+") AND NOT EXISTS(SELECT 1 FROM guild_bank_item g WHERE g.item_guid="+
                std::to_string(source.item)+") AND NOT EXISTS(SELECT 1 FROM auction x WHERE x.itemguid="+
                std::to_string(source.item)+")";
        }
    } else for(const auto& source:quote.items) {
        const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,source.item));if(!item)return {};
        sql+=itemProof(quote.actor,{source.item,source.quantity,item->GetContainer()?item->GetContainer()->GetGUIDLow():0,item->GetSlot()});
    }
    return sql;
}
}
