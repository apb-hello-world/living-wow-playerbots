#include "LivingCommissionTradeSettlement.h"
#include "LivingActivityOperations.h"
#include "LivingActivityJournal.h"
#include "LivingActivityTransfer.h"

namespace LivingActivity {
namespace {
using Tree=boost::property_tree::ptree;
void Require(bool ok,const char* why){if(!ok)throw std::invalid_argument(why);}
Tree Parse(const std::string& text) {
    Require(!text.empty() && text.size()<=8192,"commission_trade_receipt_bound");
    Tree p;std::istringstream input(text);boost::property_tree::read_json(input,p);return p;
}
uint64_t Number(const Tree& p,const char* key) {
    const auto& child=p.get_child(key);const auto& s=child.data();
    Require(child.empty() && !s.empty() && s.size()<=20 && (s.size()==1 || s[0]!='0') &&
        s.find_first_not_of("0123456789")==std::string::npos,"commission_trade_receipt_number");
    return std::stoull(s);
}
bool ReadPartition(const Task& task,const StoredCraftOperation& row,CommissionPartitionQuote& q) {
    CommissionJob job;std::string why;const auto& op=row.receipt;
    if(!row.acknowledged || !DecodeCommissionJob(task.checkpoint.data,job,why) ||
        row.journalDigest.size()!=64 || row.journalDigest.find_first_not_of("0123456789abcdef")!=std::string::npos ||
        !IsUuid(op.id) || op.task!=task.id || op.kind!="commission_output_partition" ||
        op.taskRevision<=job.craftFinishedRevision || op.taskRevision>task.revision)return false;
    const auto p=Parse(row.beforeState);EnchantCodec::Object(p,{"effects","persistence","native"});
    return Number(p,"effects")==Mask(Effect::Inventory) && Number(p,"persistence")==unsigned(NativePersistence::Inventory) &&
        DecodeCommissionPartition(EnchantCodec::Json(p.get_child("native")),q) && MatchesCommissionPartition(task,q);
}
std::string PartitionJournalGuard(const StoredCraftOperation& row) {
    const auto& op=row.receipt;
    return " AND EXISTS(SELECT 1 FROM living_activity_operation p WHERE p.operation_id="+SqlValue(op.id)+
        " AND p.task_id=living_activity_task.task_id AND p.task_revision="+std::to_string(op.taskRevision)+
        " AND p.kind='commission_output_partition' AND p.state="+SqlValue(op.state==OperationState::Verified?"verified":op.state==OperationState::Intent?"intent":"reconciling")+
        " AND SHA2(CONCAT(p.before_state,'|',p.after_state),256)="+SqlValue(row.journalDigest)+')';
}
}
bool DecodeInterruptedCommissionPartition(const Task& task,const StoredCraftOperation& row,CommissionPartitionQuote& q) {
    q={};try {return (row.receipt.state==OperationState::Intent || row.receipt.state==OperationState::Reconciling) && ReadPartition(task,row,q);}
    catch(const std::exception&){return false;}
}
bool DecodeStoredCommissionPartition(const Task& task,const StoredCraftOperation& row,CommissionPartitionQuote& q,
    uint32_t& surplus,std::string& why) {
    q={};surplus=0;why="commission_partition_saved_proof_required";
    try {
        if(!ReadPartition(task,row,q) || row.receipt.state!=OperationState::Verified ||
            row.receipt.taskRevision>=task.revision || row.receipt.evidence!="native_commission_partition_observed")return false;
        const auto after=Parse(row.afterState);
        EnchantCodec::Object(after,{"claimed_item","claimed_count","surplus_item","surplus_count","money","claims_unchanged"});
        const auto extra=Number(after,"surplus_item");
        if(!extra || extra>UINT32_MAX || extra==q.item || Number(after,"claimed_item")!=q.item ||
            Number(after,"claimed_count")!=q.quantity || Number(after,"surplus_count")!=q.count-q.quantity ||
            Number(after,"money")!=q.money || after.get<std::string>("claims_unchanged")!="true" ||
            row.receipt.nativeReference!="item:"+std::to_string(q.item)+":surplus:"+std::to_string(extra))return false;
        surplus=uint32_t(extra);why.clear();return true;
    }catch(const std::exception&){why="commission_partition_saved_proof_malformed";return false;}
}
bool DecodeStoredCommissionTrade(const Task& task,const StoredCraftOperation& row,CommissionTradeQuote& result,std::string& why) {
    result={};why.clear();
    try {
        CommissionJob job;const auto& r=row.receipt;
        Require(Validate(task,why) && IsCommissionJob(task) && ValidateCommissionTask(task,why) &&
            DecodeCommissionJob(task.checkpoint.data,job,why) && task.accepted && task.mode==Mode::Active &&
            job.craftFinishedRevision && (job.agreement.delivery=="direct" || job.agreement.delivery=="meeting"),
            "commission_trade_receipt_task_invalid");
        Require(row.acknowledged && row.journalDigest.size()==64 &&
            row.journalDigest.find_first_not_of("0123456789abcdef")==std::string::npos && IsUuid(r.id) && r.task==task.id &&
            r.taskRevision>job.craftFinishedRevision && r.taskRevision<task.revision && r.kind=="commission_trade" &&
            r.state==OperationState::Verified && r.nativeReference=="trade:"+r.id &&
            r.evidence=="native_commission_trade_and_fee_observed","commission_trade_receipt_not_verified");
        const auto before=Parse(row.beforeState),after=Parse(row.afterState);
        EnchantCodec::Object(before,{"effects","persistence","native"});
        EnchantCodec::Object(before.get_child("native"),{"native","claimed_consumption"});
        EnchantCodec::Object(after,{"native","claimed_consumption"});
        CommissionTradeQuote quote;
        Require(Number(before,"effects")==(Mask(Effect::Inventory)|Mask(Effect::Money)) &&
            Number(before,"persistence")==unsigned(NativePersistence::Inventory) &&
            DecodeCommissionTradeQuote(EnchantCodec::Json(before.get_child("native.native")),quote),
            "commission_trade_saved_quote_invalid");
        const auto& inputs=before.get_child("native.claimed_consumption");
        Require(inputs.data().empty() && !inputs.empty() && inputs.size()<=16 &&
            EnchantCodec::Json(inputs)==EnchantCodec::Json(after.get_child("claimed_consumption")),
            "commission_trade_saved_claims_changed");
        std::vector<ClaimConsumption> uses;
        for(const auto& field:inputs) {
            Require(field.first.empty(),"commission_trade_claim_array_invalid");const auto& p=field.second;
            EnchantCodec::Object(p,{"claim","task","actor","revision","item_guid","item_entry","quantity","copper","location","used"});
            ResourceClaim c;c.id=p.get<std::string>("claim");c.task=p.get<std::string>("task");c.state="held";
            c.actor=EnchantCodec::Number(p.get_child("actor"));c.itemGuid=EnchantCodec::Number(p.get_child("item_guid"));
            c.itemEntry=EnchantCodec::Number(p.get_child("item_entry"));c.revision=Number(p,"revision");
            c.quantity=Number(p,"quantity");c.copper=Number(p,"copper");c.location=p.get<std::string>("location");
            uses.push_back({c,EnchantCodec::Number(p.get_child("used"))});
        }
        // A historical read uses a nonterminal local projection solely for
        // validation; it never restores execution ownership on the real task.
        auto historical=task;historical.phase=Phase::Verifying;
        Require(ExactCommissionTradeConsumption(historical,quote,uses),"commission_trade_saved_claims_invalid");
        CommissionTradeEvidence evidence;
        Require(DecodeCommissionTradeEvidence(quote,EnchantCodec::Json(after.get_child("native")),evidence,why),
            "commission_trade_saved_native_evidence_invalid");
        result=std::move(quote);return true;
    }catch(const std::invalid_argument& error){why=error.what();}
     catch(const std::exception&){why="commission_trade_saved_receipt_malformed";}
    return false;
}
bool PrepareCommissionTradeReadyRestore(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,const std::vector<NativeResourceBalance>& native,
    uint64_t now,const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};why="commission_trade_ready_restore_proof_required";CommissionJob job;ProfessionJob recipe;
    if(!Validate(saved,why) || !IsCommissionJob(saved) || !ValidateCommissionTask(saved,why) ||
        !DecodeCommissionJob(saved.checkpoint.data,job,why) || !DecodeProfessionIntent(job.craft,recipe,why) ||
        !job.craftFinishedRevision || job.agreement.delivery=="mail" || !saved.accepted || saved.mode!=Mode::Active ||
        (saved.phase!=Phase::Preparing && saved.phase!=Phase::WaitingExternal && saved.phase!=Phase::Reconciling &&
            !(saved.phase==Phase::Verifying && saved.checkpoint.step=="commission_output_partition")) ||
        (saved.checkpoint.step!="commission_craft_ready" && saved.checkpoint.step!="commission_trade_prepare" &&
            saved.checkpoint.step!="commission_trade_wait" && saved.checkpoint.step!="commission_output_partition") ||
        saved.context==current || current.actor!=saved.actor || !IsUuid(current.boot) ||
        !current.actorGeneration || !current.mapGeneration || !current.policyRevision ||
        current.session.size()>120 || current.session.empty()!=(current.sessionRevision==0) ||
        !history.complete || history.task!=saved.id || history.revision!=saved.revision || history.unresolvedOperation ||
        !history.commissionMail.empty() || !history.commissionTrade.empty() || history.attempts.empty() ||
        !claims.complete || !claims.bookRevision || claims.claims.empty() || claims.claims.size()>16 ||
        native.empty() || native.size()>16 || now<saved.updatedAtMs || !IsUuid(receipt) || saved.revision>=UINT64_MAX-1)return false;
    std::string partitionGuard;
    for(const auto& row:history.commissionPartitions) {
        CommissionPartitionQuote q;uint32_t surplus=0;
        if(!DecodeStoredCommissionPartition(saved,row,q,surplus,why))return false;
        for(const auto& c:q.claims)if(std::none_of(claims.claims.begin(),claims.claims.end(),
            [&](const auto& held){return SameResourceClaim(c,held);}))return false;
        partitionGuard+=PartitionJournalGuard(row);
    }
    if(saved.checkpoint.step=="commission_output_partition" && history.commissionPartitions.empty())return false;
    std::vector<ClaimConsumption> coverage;std::map<uint32_t,uint64_t> quantities;
    for(const auto& c:claims.claims) {
        if(c.quantity>UINT32_MAX || c.itemEntry!=recipe.outputEntry)return false;
        coverage.push_back({c,uint32_t(c.quantity)});quantities[c.itemGuid]+=c.quantity;
    }
    CommissionTradeQuote quote{saved.actor,job.agreement.recipient,recipe.outputEntry,job.agreement.feeCopper,0,job.agreement.feeCopper,{}};
    for(const auto& row:quantities) {
        if(row.second>UINT32_MAX)return false;
        quote.items.push_back({row.first,uint32_t(row.second)});
    }
    if(!ExactCommissionTradeConsumption(saved,quote,coverage) || native.size()!=quantities.size())return false;
    std::set<uint32_t> seen;
    for(const auto& balance:native) {
        if(balance.actor!=saved.actor || balance.itemEntry!=recipe.outputEntry || balance.location!="bags" ||
            balance.copper || balance.nativeReference || !seen.insert(balance.itemGuid).second ||
            !quantities.count(balance.itemGuid) || quantities.at(balance.itemGuid)>balance.quantity)return false;
    }
    auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;
    next.phase=Phase::Preparing;next.checkpoint.step="commission_trade_prepare";next.checkpoint.blocker.clear();next.retryAtMs=0;
    auto plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"commission_trade_ready_restored",job.agreement.id);
    auto& sql=plan.statements.front();
    sql+=partitionGuard+" AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task a ON a.task_id=o.task_id"
        " WHERE a.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.kind IN ('commission_trade','commission_mail_send') AND o.state<>'rejected')"
        " AND EXISTS(SELECT 1 FROM living_activity_transition t WHERE t.task_id=living_activity_task.task_id AND t.task_revision="+
        std::to_string(job.craftFinishedRevision)+" AND t.code='commission_craft_verified')"
        " AND EXISTS(SELECT 1 FROM organic_economy_commission c WHERE c.commission_id="+SqlValue(job.agreement.id)+
        " AND c.authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+
        " AND c.bot_guid=living_activity_task.actor_guid AND c.state IN ('crafting','ready','traveling'))"
        " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('consumed','released'))="+std::to_string(claims.claims.size());
    for(const auto& c:claims.claims)sql+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.claim_id="+SqlValue(c.id)+" AND c.revision="+std::to_string(c.revision)+
        " AND c.item_guid="+std::to_string(c.itemGuid)+" AND c.quantity="+std::to_string(c.quantity)+" AND c.state='held' AND c.location='bags')";
    for(const auto& balance:native)sql+=" AND EXISTS(SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid"
        " WHERE i.guid="+std::to_string(balance.itemGuid)+" AND i.owner_guid="+std::to_string(saved.actor)+
        " AND v.guid=i.owner_guid AND i.itemEntry="+std::to_string(balance.itemEntry)+" AND i.count="+std::to_string(balance.quantity)+")";
    result.task=std::move(next);result.plan=std::move(plan);why.clear();return true;
}
bool PrepareInterruptedCommissionPartition(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,const std::vector<NativeResourceBalance>& native,const CommissionPartitionRestoreState& physical,
    uint64_t now,const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};why="commission_partition_restart_proof_required";
    try {
        CommissionJob job;std::string reason;
        Require(Validate(saved,reason) && ValidateCommissionTask(saved,reason) && DecodeCommissionJob(saved.checkpoint.data,job,reason) &&
            saved.accepted && saved.mode==Mode::Active && job.craftFinishedRevision && saved.context.boot.empty() &&
            !saved.context.actorGeneration && !saved.context.mapGeneration &&
            (saved.phase==Phase::Executing || saved.phase==Phase::Reconciling) && saved.checkpoint.step=="commission_output_partition" &&
            current.actor==saved.actor && IsUuid(current.boot) && current.actorGeneration && current.mapGeneration && current.policyRevision &&
            current.session.size()<=120 && current.session.empty()==(current.sessionRevision==0) &&
            now>=saved.updatedAtMs && saved.revision<UINT64_MAX-1 && IsUuid(receipt),"commission_partition_restored_context_required");
        Require(history.complete && history.task==saved.id && history.revision==saved.revision && history.unresolvedOperation &&
            !history.attempts.empty() && history.commissionMail.empty() && history.commissionTrade.empty() &&
            !history.interruptedCommissionOffer && !history.commissionPartitions.empty(),"commission_partition_restart_history_required");
        const StoredCraftOperation* pending=nullptr;
        for(const auto& row:history.commissionPartitions) {
            CommissionPartitionQuote q;uint32_t surplus=0;
            if(row.receipt.state==OperationState::Verified) {
                Require(DecodeStoredCommissionPartition(saved,row,q,surplus,reason),"commission_partition_prior_proof_invalid");continue;
            }
            Require(!pending,"commission_partition_multiple_pending");pending=&row;
        }
        Require(pending,"commission_partition_pending_receipt_required");const auto& op=pending->receipt;CommissionPartitionQuote q;
        Require(ReadPartition(saved,*pending,q) && (op.state==OperationState::Intent || op.state==OperationState::Reconciling) &&
            saved.revision-op.taskRevision<=1,"commission_partition_restart_intent_required");
        const auto& source=physical.source;
        Require(physical.money==q.money && physical.destinationEmpty && physical.destinationBag==q.destinationBag &&
            source.actor==q.actor && source.guid==q.item && source.entry==q.entry && source.count==q.count &&
            source.bagGuid==q.sourceBag && source.slot==(q.position&255),"commission_partition_original_native_state_required");
        Require(claims.complete && claims.bookRevision && !claims.claims.empty() && claims.claims.size()<=16 &&
            !native.empty() && native.size()<=16,"commission_partition_restart_claims_required");
        for(const auto& expected:q.claims)Require(std::any_of(claims.claims.begin(),claims.claims.end(),
            [&](const auto& held){return SameResourceClaim(expected,held);}),"commission_partition_restart_claim_changed");
        std::map<uint32_t,uint64_t> quantities;std::vector<ClaimConsumption> coverage;
        for(const auto& c:claims.claims) {
            Require(c.quantity<=UINT32_MAX,"commission_partition_claim_quantity");coverage.push_back({c,uint32_t(c.quantity)});
            quantities[c.itemGuid]+=c.quantity;
        }
        CommissionTradeQuote output{q.actor,q.recipient,q.entry,job.agreement.feeCopper,0,job.agreement.feeCopper,{}};
        for(const auto& item:quantities) {Require(item.second<=UINT32_MAX,"commission_partition_claim_quantity");output.items.push_back({item.first,uint32_t(item.second)});}
        Require(ExactCommissionTradeConsumption(saved,output,coverage) && native.size()==quantities.size(),"commission_partition_complete_output_required");
        std::set<uint32_t> seen;std::string guard;
        for(const auto& b:native) {
            Require(ValidNativeResourceBalance(b) && b.actor==q.actor && b.itemEntry==q.entry && b.location=="bags" &&
                !b.copper && !b.nativeReference && seen.insert(b.itemGuid).second && quantities.count(b.itemGuid) &&
                b.quantity>=quantities.at(b.itemGuid) && (b.itemGuid!=q.item || b.quantity==q.count),"commission_partition_output_custody_changed");
            guard+=" AND EXISTS(SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid WHERE i.guid="+
                std::to_string(b.itemGuid)+" AND i.owner_guid="+std::to_string(q.actor)+" AND v.guid=i.owner_guid AND i.itemEntry="+
                std::to_string(q.entry)+" AND i.count="+std::to_string(b.quantity)+')';
        }
        auto n=[](uint64_t value){return std::to_string(value);};
        guard+=" AND EXISTS(SELECT 1 FROM characters a WHERE a.guid="+n(q.actor)+" AND a.money="+n(q.money)+')'+
            " AND EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid="+n(q.actor)+" AND v.item="+n(q.item)+
            " AND v.bag="+n(q.sourceBag)+" AND v.slot="+n(q.position&255)+')'+
            " AND NOT EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid="+n(q.actor)+" AND v.bag="+n(q.destinationBag)+" AND v.slot="+n(q.destination&255)+')'+
            " AND (SELECT COUNT(*) FROM character_inventory v WHERE v.item="+n(q.item)+")=1"+
            " AND NOT EXISTS(SELECT 1 FROM mail_items m WHERE m.item_guid="+n(q.item)+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item g WHERE g.item_guid="+n(q.item)+')'+
            " AND NOT EXISTS(SELECT 1 FROM auction a WHERE a.itemguid="+n(q.item)+')';
        for(const auto& c:claims.claims)guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+TransferClaimPredicate(c)+')';
        guard+=" AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.actor_guid="+n(q.actor)+" AND c.item_guid="+n(q.item)+" AND c.state='held')="+n(q.claims.size());
        auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;next.phase=Phase::Verifying;
        next.checkpoint.blocker.clear();next.retryAtMs=0;
        auto outcome=op;outcome.state=OperationState::Rejected;outcome.evidence="commission_partition_unchanged_on_restart";
        auto plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,EncodeCommissionPartition(q));
        plan.statements.front()+=" AND phase="+SqlValue(Name(saved.phase))+" AND checkpoint="+SqlValue(saved.checkpoint.data)+
            PartitionJournalGuard(*pending)+guard+
            " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"
            " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id AND c.state NOT IN ('consumed','released'))="+n(claims.claims.size())+
            " AND EXISTS(SELECT 1 FROM living_activity_transition t WHERE t.task_id=living_activity_task.task_id AND t.task_revision="+n(job.craftFinishedRevision)+" AND t.code='commission_craft_verified')";
        result.task=std::move(next);result.plan=std::move(plan);why.clear();return true;
    }catch(const std::invalid_argument& error){why=error.what();}
     catch(const std::exception&){why="commission_partition_restart_receipt_malformed";}
    return false;
}
bool PrepareInterruptedCommissionOffer(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,const std::vector<NativeResourceBalance>& native,
    uint64_t now,const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};why="commission_offer_restart_proof_required";
    try {
        CommissionJob job;
        Require(Validate(saved,why) && ValidateCommissionTask(saved,why) && IsCommissionJob(saved) &&
            DecodeCommissionJob(saved.checkpoint.data,job,why) && job.craftFinishedRevision &&
            saved.accepted && saved.mode==Mode::Active && saved.context.boot.empty() &&
            !saved.context.actorGeneration && !saved.context.mapGeneration &&
            (saved.phase==Phase::Executing || saved.phase==Phase::Reconciling) &&
            saved.checkpoint.step=="commission_trade_offer" && current.actor==saved.actor && IsUuid(current.boot) &&
            current.actorGeneration && current.mapGeneration && current.policyRevision &&
            current.session.size()<=120 && current.session.empty()==(current.sessionRevision==0) &&
            now>=saved.updatedAtMs && saved.revision<UINT64_MAX-1 && IsUuid(receipt),"commission_offer_restored_context_required");
        Require(history.complete && history.task==saved.id && history.revision==saved.revision &&
            history.unresolvedOperation && history.interruptedCommissionOffer && !history.attempts.empty() &&
            history.commissionMail.empty() && history.commissionTrade.empty(),"commission_offer_restart_history_required");
        const auto& row=*history.interruptedCommissionOffer;const auto& op=row.receipt;
        Require(row.acknowledged && row.journalDigest.size()==64 &&
            row.journalDigest.find_first_not_of("0123456789abcdef")==std::string::npos &&
            IsUuid(op.id) && op.task==saved.id && op.kind=="commission_trade_offer" &&
            op.taskRevision>job.craftFinishedRevision && op.taskRevision<=saved.revision &&
            saved.revision-op.taskRevision<=1 &&
            (op.state==OperationState::Intent || op.state==OperationState::Reconciling),"commission_offer_restart_intent_required");
        const auto before=Parse(row.beforeState);EnchantCodec::Object(before,{"effects","persistence","native"});
        CommissionTradeQuote quote;
        Require(Number(before,"effects")==Mask(Effect::Inventory) &&
            Number(before,"persistence")==unsigned(NativePersistence::JournalOnly) &&
            DecodeCommissionTradeQuote(EnchantCodec::Json(before.get_child("native")),quote),"commission_offer_restart_contract_required");
        Require(claims.complete && claims.bookRevision && !claims.claims.empty() && claims.claims.size()<=16 &&
            native.size()==quote.items.size(),"commission_offer_restart_custody_required");
        std::vector<ClaimConsumption> coverage;
        for(const auto& c:claims.claims) {Require(c.quantity<=UINT32_MAX,"commission_offer_claim_quantity");coverage.push_back({c,uint32_t(c.quantity)});}
        Require(ExactCommissionTradeConsumption(saved,quote,coverage),"commission_offer_restart_claims_changed");
        std::set<uint32_t> seen;std::string guard;
        for(const auto& b:native) {
            Require(ValidNativeResourceBalance(b) && b.actor==saved.actor && b.itemEntry==quote.entry && b.location=="bags" &&
                !b.copper && !b.nativeReference && seen.insert(b.itemGuid).second &&
                std::any_of(quote.items.begin(),quote.items.end(),[&](const auto& i){return i.item==b.itemGuid && i.quantity==b.quantity;}),
                "commission_offer_restart_native_custody_changed");
            guard+=" AND EXISTS(SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid"
                " WHERE i.guid="+std::to_string(b.itemGuid)+" AND i.owner_guid="+std::to_string(saved.actor)+
                " AND v.guid=i.owner_guid AND i.itemEntry="+std::to_string(b.itemEntry)+" AND i.count="+std::to_string(b.quantity)+")";
        }
        for(const auto& c:claims.claims)guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(c.id)+
            " AND c.task_id=living_activity_task.task_id AND c.actor_guid="+std::to_string(saved.actor)+
            " AND c.item_guid="+std::to_string(c.itemGuid)+" AND c.item_entry="+std::to_string(c.itemEntry)+
            " AND c.quantity="+std::to_string(c.quantity)+" AND c.revision="+std::to_string(c.revision)+
            " AND c.copper=0 AND c.native_reference=0 AND c.state='held' AND c.location='bags')";
        auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;next.phase=Phase::Verifying;
        next.checkpoint.blocker.clear();next.retryAtMs=0;
        auto outcome=op;outcome.state=OperationState::Rejected;outcome.evidence="commission_offer_cleared_on_restart";
        // JSON uses the same exact quote as native custody evidence. No claim,
        // inventory or money row is changed by this receipt-only reconciliation.
        auto plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,EncodeCommissionTradeQuote(quote));
        plan.statements.front()+=" AND phase="+SqlValue(Name(saved.phase))+" AND checkpoint="+SqlValue(saved.checkpoint.data)+
            " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(op.id)+
            " AND o.task_id=living_activity_task.task_id AND o.kind='commission_trade_offer' AND o.state="+SqlValue(op.state==OperationState::Intent?"intent":"reconciling")+
            " AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+")"
            " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind IN ('commission_trade','commission_mail_send') AND o.state<>'rejected')"
            " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state NOT IN ('consumed','released'))="+std::to_string(claims.claims.size())+
            " AND EXISTS(SELECT 1 FROM living_activity_transition t WHERE t.task_id=living_activity_task.task_id"
            " AND t.task_revision="+std::to_string(job.craftFinishedRevision)+" AND t.code='commission_craft_verified')"+guard;
        result.task=std::move(next);result.plan=std::move(plan);why.clear();return true;
    }catch(const std::invalid_argument& error){why=error.what();}
     catch(const std::exception&){why="commission_offer_restart_receipt_malformed";}
    return false;
}
bool PrepareCommissionTradeSettlement(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};why="commission_trade_settlement_unresolved";
    if(!history.complete || history.task!=saved.id || history.revision!=saved.revision || history.unresolvedOperation ||
        !history.commissionMail.empty() || history.commissionTrade.size()!=1 || !claims.complete || !claims.bookRevision ||
        !claims.claims.empty())return false;
    CommissionTradeQuote quote;const auto& stored=history.commissionTrade.front();
    if(!DecodeStoredCommissionTrade(saved,stored,quote,why))return false;
    if((saved.phase!=Phase::Verifying && saved.phase!=Phase::Reconciling && saved.phase!=Phase::WaitingExternal) ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration || !current.mapGeneration ||
        !current.policyRevision || current.session.size()>120 || current.session.empty()!=(current.sessionRevision==0)) {
        why="commission_trade_settlement_context_invalid";return false;
    }
    CommissionJob job;if(!DecodeCommissionJob(saved.checkpoint.data,job,why))return false;
    const auto& operation=stored.receipt;auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;
    next.phase=Phase::Completed;next.checkpoint.step="commission_completed";next.checkpoint.blocker.clear();next.retryAtMs=0;
    auto plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"commission_delivery_completed",operation.id+':'+stored.journalDigest);
    plan.statements.front()+=" AND mode='active' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id AND c.state NOT IN ('consumed','released'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation u JOIN living_activity_task a ON a.task_id=u.task_id"
        " WHERE a.actor_guid=living_activity_task.actor_guid AND u.state IN ('intent','reconciling'))"
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(operation.id)+
        " AND o.task_id=living_activity_task.task_id AND o.task_revision="+std::to_string(operation.taskRevision)+
        " AND o.kind='commission_trade' AND o.state='verified' AND o.native_reference="+SqlValue(operation.nativeReference)+
        " AND o.evidence_code="+SqlValue(operation.evidence)+" AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+
        SqlValue(stored.journalDigest)+") AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.kind IN ('commission_trade','commission_mail_send') AND o.state<>'rejected')=1"
        " AND EXISTS(SELECT 1 FROM living_activity_transition t WHERE t.task_id=living_activity_task.task_id AND t.task_revision="+
        std::to_string(job.craftFinishedRevision)+" AND t.code='commission_craft_verified')"
        " AND EXISTS(SELECT 1 FROM organic_economy_commission c WHERE c.commission_id="+SqlValue(job.agreement.id)+
        " AND c.bot_guid=living_activity_task.actor_guid AND c.player_guid="+std::to_string(job.agreement.recipient)+
        " AND c.authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+" AND c.state IN ('crafting','ready','traveling'))";
    plan.statements.push_back("UPDATE organic_economy_commission SET state='completed',failure_reason='' WHERE commission_id="+
        SqlValue(job.agreement.id)+" AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+
        " AND state IN ('crafting','ready','traveling') AND EXISTS("+plan.receiptQuery+")");
    result.task=std::move(next);result.plan=std::move(plan);why.clear();return true;
}
}
