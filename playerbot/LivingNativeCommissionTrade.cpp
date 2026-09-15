#include "botpch.h"
#include "LivingNativeCommissionTrade.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityScope.h"
#include "LivingActivityNativeContext.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"
#include "LivingNativeParcelSlot.h"
#include "LivingActivityTransfer.h"

namespace LivingActivity {
bool PlanNativeCommissionPartition(Player& actor,const Task& task,CommissionPartitionQuote& quote,std::string& why,bool capacityInspection) {
    quote={};auto reject=[&](const char* code){why=code;return false;};
    CommissionJob job;ProfessionJob recipe;
    if(!sLivingActivityCoordinator.OnWorldThread() || task.actor!=actor.GetGUIDLow() || !actor.GetPlayerbotAI() ||
        !ValidateCommissionTask(task,why) || !DecodeCommissionJob(task.checkpoint.data,job,why) ||
        !job.craftFinishedRevision || job.agreement.delivery=="mail" || !DecodeProfessionIntent(job.craft,recipe,why) ||
        (task.phase!=Phase::Preparing && task.phase!=Phase::Executing))return reject("commission_partition_task_required");
    if(!actor.IsInWorld() || !actor.GetMap() || actor.GetMap()->IsDungeon() || !actor.GetSession() ||
        ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) ||
        !actor.IsStopped() || actor.IsNonMeleeSpellCasted(false))return reject("commission_partition_safety_pause");
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,why))return false;
    if(!claims.complete || claims.claims.empty() || claims.claims.size()>16)return reject("commission_partition_claims_required");
    std::vector<ClaimConsumption> coverage;std::map<uint32_t,uint64_t> quantities;
    for(const auto& c:claims.claims) {
        if(c.itemEntry!=recipe.outputEntry) {
            if(!capacityInspection)return reject("commission_partition_preparation_claims_pending");
            continue;
        }
        if(c.quantity>UINT32_MAX)return reject("commission_partition_quantity_invalid");
        coverage.push_back({c,uint32_t(c.quantity)});quantities[c.itemGuid]+=c.quantity;
    }
    CommissionTradeQuote output{task.actor,job.agreement.recipient,recipe.outputEntry,job.agreement.feeCopper,0,job.agreement.feeCopper,{}};
    for(const auto& row:quantities) {
        if(row.second>UINT32_MAX)return reject("commission_partition_quantity_invalid");
        output.items.push_back({row.first,uint32_t(row.second)});
    }
    if(!ExactCommissionTradeConsumption(task,output,coverage))return reject("commission_partition_exact_output_claims_required");
    const auto privateItems=sPlayerbotActionBroker.ReservedItemsView();
    for(const auto& row:quantities) {
        auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,row.first));
        if(!item || item->GetOwnerGuid()!=actor.GetObjectGuid() || !Player::IsInventoryPos(item->GetPos()) ||
            item->GetEntry()!=recipe.outputEntry || item->GetCount()<row.second)return reject("commission_partition_source_missing");
        if(item->GetCount()==row.second)continue;
        if(auto* offered=actor.GetTradeData()) {
            auto* customer=actor.GetTrader();
            if(!customer || customer->GetGUIDLow()!=job.agreement.recipient || !customer->GetTradeData() ||
                offered->IsAccepted() || customer->GetTradeData()->IsAccepted())return reject("commission_partition_accepted_window_preserved");
            for(uint8_t slot=0;slot<TRADE_SLOT_COUNT;++slot)
                if(offered->GetItem(TradeSlots(slot)))return reject("commission_partition_existing_offer_preserved");
        }
        if(!item->CanBeTraded() || item->IsInTrade() || item->HasGeneratedLoot() || item->IsConjuredConsumable() ||
            item->GetUInt32Value(ITEM_FIELD_DURATION) || !privateItems || privateItems->Item(item->GetGUIDLow()) ||
            sGuildSupplies.Reserved(item->GetGUIDLow()) || ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))
            return reject("commission_partition_source_protected");
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bags"},available,why))return false;
        if(available!=item->GetCount())return reject("commission_partition_other_commitment");
        quote.actor=task.actor;quote.recipient=job.agreement.recipient;quote.item=item->GetGUIDLow();
        quote.entry=item->GetEntry();quote.count=item->GetCount();quote.quantity=uint32_t(row.second);
        quote.money=actor.GetMoney();quote.position=item->GetPos();quote.sourceBag=item->GetContainer()?item->GetContainer()->GetGUIDLow():0;
        if(!EmptyNativeParcelSlot(actor,*item,quote.count-quote.quantity,quote.destination))
            return reject("commission_partition_empty_slot_required");
        auto* container=actor.GetItemByPos(INVENTORY_SLOT_BAG_0,uint8_t(quote.destination>>8));
        quote.destinationBag=(quote.destination>>8)==INVENTORY_SLOT_BAG_0?0:container?container->GetGUIDLow():0;
        for(const auto& c:claims.claims)if(c.itemGuid==quote.item)quote.claims.push_back(c);
        std::sort(quote.claims.begin(),quote.claims.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        if(!MatchesCommissionPartition(task,quote))return reject("commission_partition_quote_invalid");
        try {(void)EncodeCommissionPartition(quote);}catch(const std::exception&){return reject("commission_partition_quote_bound");}
        why.clear();return true;
    }
    return reject("commission_partition_not_needed");
}
bool NativeCommissionPartition::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);CommissionPartitionQuote fresh;
    if(!saved || request.beforeState!=EncodeCommissionPartition(quote) ||
        !PlanNativeCommissionPartition(actor,*saved,fresh,why))return false;
    if(EncodeCommissionPartition(fresh)!=EncodeCommissionPartition(quote)) {why="commission_partition_quote_changed";return false;}
    return true;
}
NativeObservation NativeCommissionPartition::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string why;surplusItem=0;unchanged=false;
    if(!ValidateNative(actor,request,why)) {out.state=OperationState::Rejected;out.evidence=why.empty()?"commission_partition_rejected":why;return out;}
    if(!CharacterDatabase.HasOpenTransaction()) {out.state=OperationState::Rejected;out.evidence="commission_partition_native_transaction_required";return out;}
    const auto total=actor.GetItemCount(quote.entry,false);
    actor.SplitItem(quote.position,quote.destination,quote.count-quote.quantity);
    const auto* source=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));const auto* extra=actor.GetItemByPos(quote.destination);
    unchanged=source && source->GetOwnerGuid()==actor.GetObjectGuid() && source->GetEntry()==quote.entry &&
        source->GetPos()==quote.position && source->GetCount()==quote.count && !extra && actor.GetMoney()==quote.money &&
        actor.GetItemCount(quote.entry,false)==total;
    if(unchanged) {out.state=OperationState::Rejected;out.evidence="commission_partition_native_handler_rejected";return out;}
    if(!source || !extra || source->GetOwnerGuid()!=actor.GetObjectGuid() || extra->GetOwnerGuid()!=actor.GetObjectGuid() ||
        extra->GetGUIDLow()==quote.item || source->GetEntry()!=quote.entry || extra->GetEntry()!=quote.entry ||
        source->GetPos()!=quote.position || source->GetCount()!=quote.quantity || extra->GetCount()!=quote.count-quote.quantity ||
        actor.GetMoney()!=quote.money || actor.GetItemCount(quote.entry,false)!=total) {
        out.evidence="commission_partition_native_custody_uncertain";return out;
    }
    surplusItem=extra->GetGUIDLow();out.state=OperationState::Verified;out.evidence="native_commission_partition_observed";
    out.nativeReference="item:"+std::to_string(quote.item)+":surplus:"+std::to_string(surplusItem);
    out.retainedSplit={quote.actor,surplusItem,quote.entry,quote.count-quote.quantity,0,"bags"};
    out.afterState="{\"claimed_item\":"+std::to_string(quote.item)+",\"claimed_count\":"+std::to_string(quote.quantity)+
        ",\"surplus_item\":"+std::to_string(surplusItem)+",\"surplus_count\":"+std::to_string(quote.count-quote.quantity)+
        ",\"money\":"+std::to_string(quote.money)+",\"claims_unchanged\":true}";
    return out;
}
std::string NativeCommissionPartition::PersistedNativeProof(Player&,const OperationRequest&,const Task& task) const {
    if(!surplusItem && !unchanged)return {};
    auto n=[](uint64_t value){return std::to_string(value);};
    std::string sql="SELECT "+SqlValue(task.id)+','+n(task.revision)+" FROM characters a WHERE a.guid="+n(quote.actor)+" AND a.money="+n(quote.money);
    auto item=[&](uint32_t guid,uint32_t count,uint32_t bag,uint16_t position) {
        return " AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid=a.guid AND i.owner_guid=a.guid"
            " AND i.guid="+n(guid)+" AND i.itemEntry="+n(quote.entry)+" AND i.count="+n(count)+" AND v.bag="+n(bag)+" AND v.slot="+n(position&255)+')'+
            " AND (SELECT COUNT(*) FROM character_inventory v WHERE v.item="+n(guid)+")=1"+
            " AND NOT EXISTS(SELECT 1 FROM mail_items m WHERE m.item_guid="+n(guid)+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item g WHERE g.item_guid="+n(guid)+')'+
            " AND NOT EXISTS(SELECT 1 FROM auction x WHERE x.itemguid="+n(guid)+')';
    };
    sql+=item(quote.item,surplusItem?quote.quantity:quote.count,quote.sourceBag,quote.position);
    if(surplusItem)sql+=item(surplusItem,quote.count-quote.quantity,quote.destinationBag,quote.destination);
    else sql+=" AND NOT EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid=a.guid AND v.bag="+n(quote.destinationBag)+" AND v.slot="+n(quote.destination&255)+')';
    for(const auto& c:quote.claims)sql+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+TransferClaimPredicate(c)+')';
    sql+=" AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.actor_guid=a.guid AND c.item_guid="+n(quote.item)+" AND c.state='held')="+n(quote.claims.size());
    return sql;
}
bool PlanNativeCommissionTradeOffer(Player& actor,const Task& task,CommissionTradeQuote& quote,std::string& why) {
    quote={};
    auto reject=[&](const char* code){why=code;return false;};
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(task.id);
    CommissionJob job;ProfessionJob recipe;
    if(!sLivingActivityCoordinator.OnWorldThread() || !saved || saved->revision!=task.revision ||
        saved->actor!=actor.GetGUIDLow() || (saved->phase!=Phase::Preparing && saved->phase!=Phase::Executing) ||
        !saved->accepted || saved->mode!=Mode::Active ||
        !ValidateCommissionTask(*saved,why) || !DecodeCommissionJob(saved->checkpoint.data,job,why) ||
        !job.craftFinishedRevision || job.agreement.delivery=="mail" || !DecodeProfessionIntent(job.craft,recipe,why) ||
        !actor.GetPlayerbotAI())return reject("commission_trade_offer_saved_task_required");
    auto* customer=actor.GetTrader();auto* offered=actor.GetTradeData();
    if(!customer || customer->GetGUIDLow()!=job.agreement.recipient || customer->GetTrader()!=&actor ||
        !offered || !customer->GetTradeData())return reject("commission_trade_recipient_window_required");
    if(offered->IsAccepted() || customer->GetTradeData()->IsAccepted())
        return reject("commission_trade_accepted_offer_immutable");
    for(auto* person:{&actor,customer}) {
        if(!person->IsInWorld() || !person->GetSession() || !person->GetMap() || person->GetMap()->IsDungeon() ||
            ReadNativeSafety(*person,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || !person->IsStopped() ||
            person->IsNonMeleeSpellCasted(false))return reject("commission_trade_offer_safety_pause");
    }
    if(actor.GetTeam()!=customer->GetTeam() || !actor.IsWithinDistInMap(customer,TRADE_DISTANCE,false))
        return reject("commission_trade_recipient_out_of_reach");
    if(offered->GetMoney() || offered->GetSpell() || customer->GetTradeData()->GetSpell())
        return reject("commission_trade_existing_offer_preserved");
    for(uint8_t slot=0;slot<TRADE_SLOT_COUNT;++slot)
        if(offered->GetItem(TradeSlots(slot)) || customer->GetTradeData()->GetItem(TradeSlots(slot)))
            return reject("commission_trade_existing_offer_preserved");
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,why))return false;
    if(!claims.complete || claims.claims.empty() || claims.claims.size()>16)return reject("commission_trade_claims_incomplete");
    quote={task.actor,job.agreement.recipient,recipe.outputEntry,job.agreement.feeCopper,
        actor.GetMoney(),customer->GetMoney(),{}};
    std::vector<ClaimConsumption> uses;std::map<uint32_t,Item*> items;
    const auto privateItems=sPlayerbotActionBroker.ReservedItemsView();
    for(const auto& claim:claims.claims) {
        if(claim.quantity>UINT32_MAX)return reject("commission_trade_claim_quantity_invalid");
        uses.push_back({claim,uint32_t(claim.quantity)});
        auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,claim.itemGuid));
        if(!item || item->GetOwnerGuid()!=actor.GetObjectGuid() || !Player::IsInventoryPos(item->GetPos()) ||
            item->GetEntry()!=recipe.outputEntry || !item->CanBeTraded() || item->IsInTrade() || item->HasGeneratedLoot() ||
            item->IsConjuredConsumable() || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
            sGuildSupplies.Reserved(item->GetGUIDLow()) || !privateItems || privateItems->Item(item->GetGUIDLow()) ||
            ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))
            return reject("commission_trade_claimed_output_unavailable");
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bags"},available,why))return false;
        if(available!=item->GetCount())return reject("commission_trade_output_reserved_elsewhere");
        items.emplace(item->GetGUIDLow(),item);
    }
    for(const auto& item:items)quote.items.push_back({item.first,item.second->GetCount()});
    if(!ExactCommissionTradeConsumption(task,quote,uses))return reject("commission_trade_exact_output_preparation_required");
    why.clear();return true;
}
bool NativeCommissionOffer::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);CommissionTradeQuote fresh;
    if(!saved || request.beforeState!=EncodeCommissionTradeQuote(quote) ||
        !PlanNativeCommissionTradeOffer(actor,*saved,fresh,why))return false;
    if(EncodeCommissionTradeQuote(fresh)!=EncodeCommissionTradeQuote(quote)) {
        why="commission_trade_offer_quote_changed";return false;
    }
    return true;
}
NativeObservation NativeCommissionOffer::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string why;
    if(!ValidateNative(actor,request,why) || !ExecutionScope::OwnsNativeOperation(actor.GetGUIDLow()) ||
        !sLivingActivityCoordinator.PermitEffects(*actor.GetPlayerbotAI(),{Mask(Effect::Inventory),Lane::Managed,true},
            "commission trade offer")) {
        out.state=OperationState::Rejected;out.evidence=why.empty()?"commission_trade_offer_journal_required":why;return out;
    }
    // All prerequisites are checked before the first transient UI mutation.
    // The item stays owned and claimed; this is never delivery or fee proof.
    auto* customer=actor.GetTrader();auto* offered=actor.GetTradeData();uint8_t slot=0;
    for(const auto& part:quote.items)
        offered->SetItem(TradeSlots(slot++),actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,part.item)));
    customer->GetSession()->SendUpdateTrade(true);
    if(actor.GetMoney()!=quote.actorMoney || customer->GetMoney()!=quote.recipientMoney ||
        offered->IsAccepted() || customer->GetTradeData()->IsAccepted()) {
        out.evidence="commission_trade_offer_custody_uncertain";return out;
    }
    slot=0;
    for(const auto& part:quote.items) {
        const auto* item=offered->GetItem(TradeSlots(slot++));
        if(!item || item->GetGUIDLow()!=part.item || item->GetOwnerGuid()!=actor.GetObjectGuid() ||
            item->GetCount()!=part.quantity || item->GetEntry()!=quote.entry) {
            out.evidence="commission_trade_offer_custody_uncertain";return out;
        }
    }
    out.state=OperationState::Verified;out.evidence="native_commission_offer_observed";
    out.nativeReference="trade_offer:"+request.transition.receipt;out.afterState=EncodeCommissionTradeQuote(quote);return out;
}
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
