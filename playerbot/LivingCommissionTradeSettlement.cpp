#include "LivingCommissionTradeSettlement.h"
#include "LivingActivityOperations.h"
#include "LivingActivityJournal.h"

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
