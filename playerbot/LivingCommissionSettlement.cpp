#include "LivingCommissionSettlement.h"
#include "LivingActivityJournal.h"
#include "LivingActivityOperations.h"
#include <set>

namespace LivingActivity {
namespace {
using Tree=boost::property_tree::ptree;
void Require(bool ok,const char* why) {if(!ok)throw std::invalid_argument(why);}
Tree Parse(const std::string& text) {
    Require(!text.empty() && text.size()<=8192,"commission_receipt_bound");
    Tree p;std::istringstream in(text);boost::property_tree::read_json(in,p);return p;
}
uint64_t Number(const Tree& p,const char* key) {
    const auto& child=p.get_child(key);const auto& s=child.data();
    Require(child.empty() && !s.empty() && s.size()<=20 && (s.size()==1 || s[0]!='0') &&
        s.find_first_not_of("0123456789")==std::string::npos,"commission_receipt_number_invalid");
    return std::stoull(s);
}
bool Digest(const std::string& value) {return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos;}
const char* Blocker(CommissionDeliveryState state) {
    switch(state) {
        case CommissionDeliveryState::WaitingCustomer:return "commission_customer_collection_pending";
        case CommissionDeliveryState::WaitingFee:return "commission_fee_collection_pending";
        case CommissionDeliveryState::Returned:return "commission_returned_parcel_reconciliation_required";
        case CommissionDeliveryState::Complete:return "";
    }
    return "commission_delivery_invalid";
}
}
bool InspectCommissionDelivery(const Task& task,const ProfessionHistory& history,CommissionDeliveryProof& result,std::string& why) {
    result={};why.clear();
    try {
        CommissionJob job;ProfessionJob recipe;
        Require(IsCommissionJob(task) && ValidateCommissionTask(task,why) && DecodeCommissionJob(task.checkpoint.data,job,why) &&
            job.craftFinishedRevision && job.agreement.delivery=="mail" && DecodeProfessionIntent(job.craft,recipe,why),"commission_settlement_task_invalid");
        Require(history.complete && history.task==task.id && history.revision==task.revision && !history.unresolvedOperation,
            "commission_delivery_history_unresolved");
        Require(!history.commissionMail.empty() && history.commissionMail.size()<=4,"commission_verified_send_required");
        std::set<std::string> kinds;
        const StoredCraftOperation* send=nullptr;
        for(const auto& row:history.commissionMail) {
            const auto& r=row.receipt;
            Require(row.acknowledged && Digest(row.journalDigest) && IsUuid(r.id) && r.task==task.id && r.taskRevision<=task.revision &&
                r.taskRevision>job.craftFinishedRevision && r.state==OperationState::Verified && kinds.insert(r.kind).second,
                "commission_delivery_receipt_invalid");
            if(r.kind=="commission_mail_send")send=&row;
        }
        Require(send,"commission_verified_send_required");
        CommissionDeliveryProof proof;proof.send=send->receipt.id;
        const auto before=Parse(send->beforeState),after=Parse(send->afterState);
        EnchantCodec::Object(before,{"effects","persistence","native"});
        EnchantCodec::Object(before.get_child("native"),{"native","claimed_consumption"});
        EnchantCodec::Object(after,{"native","claimed_consumption"});
        Require(Number(before,"effects")== (Mask(Effect::Inventory)|Mask(Effect::Money)) &&
            Number(before,"persistence")==unsigned(NativePersistence::Inventory) &&
            DecodeCommissionMailQuote(EnchantCodec::Json(before.get_child("native.native")),proof.quote),"commission_sent_quote_invalid");
        const auto& q=proof.quote;
        std::vector<ClaimConsumption> uses;
        const auto& inputs=before.get_child("native.claimed_consumption");
        Require(inputs.data().empty() && inputs.size()==2 &&
            EnchantCodec::Json(inputs)==EnchantCodec::Json(after.get_child("claimed_consumption")),"commission_send_claims_changed");
        for(const auto& field:inputs) {
            Require(field.first.empty(),"commission_send_claim_array_invalid");
            const auto& p=field.second;
            EnchantCodec::Object(p,{"claim","task","actor","revision","item_guid","item_entry","quantity","copper","location","used"});
            ResourceClaim c;c.id=p.get<std::string>("claim");c.task=p.get<std::string>("task");c.state="held";
            c.actor=EnchantCodec::Number(p.get_child("actor"));c.itemGuid=EnchantCodec::Number(p.get_child("item_guid"));
            c.itemEntry=EnchantCodec::Number(p.get_child("item_entry"));c.revision=Number(p,"revision");
            c.quantity=Number(p,"quantity");c.copper=Number(p,"copper");c.location=p.get<std::string>("location");
            uses.push_back({c,EnchantCodec::Number(p.get_child("used"))});
        }
        Require(ExactCommissionMailConsumption(task,q,uses),"commission_send_claims_invalid");
        Require(q.sender==task.actor && q.receiver==job.agreement.recipient && q.commission==job.agreement.id &&
            q.cod==job.agreement.feeCopper && q.entry==recipe.outputEntry && q.quantity==recipe.outputQuantity,
            "commission_sent_agreement_mismatch");
        const auto& native=after.get_child("native");
        EnchantCodec::Object(native,{"mail","item","receiver","cod","delivered_at","expires_at","postage","customer_received","fee_paid"});
        Require(Number(native,"delivered_at") && Number(native,"expires_at")>Number(native,"delivered_at") &&
            native.get<std::string>("customer_received")=="false" && native.get<std::string>("fee_paid")=="false", "commission_send_not_receipt");
        const auto mail=Number(native,"mail");Require(mail && mail<=UINT32_MAX,"commission_sent_mail_invalid");proof.mail=uint32_t(mail);
        Require(send->receipt.evidence=="native_commission_parcel_and_postage_observed" &&
            send->receipt.nativeReference=="mail:"+std::to_string(proof.mail)+":item:"+std::to_string(q.item) &&
            Number(native,"item")==q.item && Number(native,"receiver")==q.receiver && Number(native,"cod")==q.cod &&
            Number(native,"postage")==30,"commission_sent_native_identity_mismatch");
        for(const auto& row:history.commissionMail) {
            if(&row==send)continue;
            const auto& r=row.receipt;auto p=Parse(row.afterState);
            EnchantCodec::Object(p,{"version","send_operation","mail","sender","receiver","item","entry","quantity","copper",
                "money_before","money_after","inventory_before","inventory_after","payment_mail","observed_at_ms"});
            Require(r.taskRevision>=send->receipt.taskRevision && r.evidence=="native_mail_transaction_observed" && row.beforeState=="{}" &&
                Number(p,"version")==1 && p.get<std::string>("send_operation")==proof.send && Number(p,"observed_at_ms")>0,
                "commission_delivery_event_invalid");
            const auto eventMail=Number(p,"mail"),payment=Number(p,"payment_mail"),copper=Number(p,"copper"),
                moneyBefore=Number(p,"money_before"),moneyAfter=Number(p,"money_after"),
                inventoryBefore=Number(p,"inventory_before"),inventoryAfter=Number(p,"inventory_after");
            Require(eventMail && eventMail<=UINT32_MAX && payment<=UINT32_MAX && moneyBefore<=UINT32_MAX && moneyAfter<=UINT32_MAX &&
                inventoryBefore<=UINT32_MAX && inventoryAfter<=UINT32_MAX && r.nativeReference=="mail:"+std::to_string(eventMail),
                "commission_delivery_event_amount_invalid");
            if(r.kind=="commission_customer_received") {
                Require(r.id==CommissionMailReceiptId(proof.send,CommissionMailEvent::CustomerReceived) && eventMail==proof.mail &&
                    Number(p,"sender")==q.sender && Number(p,"receiver")==q.receiver && Number(p,"item")==q.item &&
                    Number(p,"entry")==q.entry && Number(p,"quantity")==q.quantity && copper==q.cod &&
                    moneyBefore>=copper && moneyAfter==moneyBefore-copper && inventoryAfter==inventoryBefore+q.quantity &&
                    bool(payment)==bool(q.cod),"commission_customer_receipt_mismatch");
                proof.received=r.id;proof.paymentMail=uint32_t(payment);
            } else if(r.kind=="commission_fee_collected") {
                Require(r.id==CommissionMailReceiptId(proof.send,CommissionMailEvent::FeeCollected) && q.cod && copper==q.cod &&
                    Number(p,"sender")==q.receiver && Number(p,"receiver")==q.sender && moneyAfter==moneyBefore+copper &&
                    !Number(p,"item") && !Number(p,"entry") && !Number(p,"quantity") && !payment && !inventoryBefore && !inventoryAfter,
                    "commission_fee_receipt_mismatch");
                proof.fee=r.id;
            } else if(r.kind=="commission_parcel_returned") {
                Require(r.id==CommissionMailReceiptId(proof.send,CommissionMailEvent::ParcelReturned) && eventMail!=proof.mail &&
                    Number(p,"sender")==q.receiver && Number(p,"receiver")==q.sender && Number(p,"item")==q.item &&
                    Number(p,"entry")==q.entry && Number(p,"quantity")==q.quantity && !copper && !payment,
                    "commission_return_receipt_mismatch");
                proof.returned=r.id;
            } else throw std::invalid_argument("commission_delivery_unknown_event");
        }
        Require(proof.returned.empty() || (proof.received.empty() && proof.fee.empty()),"commission_conflicting_custody_receipts");
        if(!proof.fee.empty()) {
            Require(!proof.received.empty(),"commission_fee_without_customer_receipt");
            for(const auto& row:history.commissionMail)if(row.receipt.id==proof.fee)
                Require(Number(Parse(row.afterState),"mail")==proof.paymentMail,"commission_fee_envelope_mismatch");
        }
        proof.state=!proof.returned.empty()?CommissionDeliveryState::Returned:proof.received.empty()?CommissionDeliveryState::WaitingCustomer:
            q.cod && proof.fee.empty()?CommissionDeliveryState::WaitingFee:CommissionDeliveryState::Complete;
        result=std::move(proof);why=Blocker(result.state);return true;
    } catch(const std::invalid_argument& e) {why=e.what();}
      catch(const std::exception&) {why="commission_delivery_receipt_malformed";}
    return false;
}
bool PrepareCommissionSettlement(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};CommissionDeliveryProof proof;
    if(!InspectCommissionDelivery(saved,history,proof,why))return false;
    if(!claims.complete || !claims.bookRevision || !claims.claims.empty()) {why="commission_settlement_claims_unresolved";return false;}
    if(Terminal(saved.phase) || (saved.phase!=Phase::Verifying && saved.phase!=Phase::WaitingExternal && saved.phase!=Phase::Reconciling) ||
        !saved.accepted || saved.mode!=Mode::Active || !IsUuid(receipt) || now<saved.updatedAtMs || now>UINT64_MAX-60000 || saved.revision>=UINT64_MAX-1 ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration || !current.mapGeneration ||
        !current.policyRevision || current.session.size()>120 || current.session.empty()!=(current.sessionRevision==0)) {
        why="commission_settlement_context_invalid";return false;
    }
    const bool complete=proof.state==CommissionDeliveryState::Complete;
    const bool returned=proof.state==CommissionDeliveryState::Returned;
    const auto phase=complete?Phase::Completed:returned?Phase::Reconciling:Phase::WaitingExternal;
    if(!complete && saved.context==current && saved.phase==phase && saved.checkpoint.blocker==why && saved.retryAtMs>now)return false;
    auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;
    next.phase=phase;
    next.checkpoint.step=complete?"commission_completed":"commission_mail_wait";next.checkpoint.blocker=why;
    next.retryAtMs=complete?0:now+60000;
    std::string fingerprint,guard=" AND mode='active' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id AND c.state NOT IN ('consumed','released'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation u JOIN living_activity_task a ON a.task_id=u.task_id"
        " WHERE a.actor_guid=living_activity_task.actor_guid AND u.state IN ('intent','reconciling'))";
    for(const auto& row:history.commissionMail) {
        fingerprint+=row.receipt.id+':'+row.journalDigest+';';
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(row.receipt.id)+
            " AND o.task_id=living_activity_task.task_id AND o.task_revision="+std::to_string(row.receipt.taskRevision)+
            " AND o.kind="+SqlValue(row.receipt.kind)+" AND o.state='verified' AND o.evidence_code="+SqlValue(row.receipt.evidence)+
            " AND o.native_reference="+SqlValue(row.receipt.nativeReference)+
            " AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+')';
    }
    guard+=" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND "
        "((o.kind='commission_mail_send' AND o.state<>'rejected') OR o.kind IN ('commission_customer_received','commission_fee_collected','commission_parcel_returned')))="+
        std::to_string(history.commissionMail.size());
    CommissionJob job;DecodeCommissionJob(saved.checkpoint.data,job,why);
    guard+=" AND EXISTS(SELECT 1 FROM living_activity_transition r WHERE r.task_id=living_activity_task.task_id AND r.task_revision="+
        std::to_string(job.craftFinishedRevision)+" AND r.code='commission_craft_verified')";
    guard+=" AND EXISTS(SELECT 1 FROM organic_economy_commission c WHERE c.commission_id="+SqlValue(job.agreement.id)+
        " AND c.bot_guid=living_activity_task.actor_guid AND c.player_guid="+std::to_string(job.agreement.recipient)+
        " AND c.authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+
        " AND c.state IN ('crafting','ready','traveling'))";
    result.task=next;result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,
        complete?"commission_delivery_completed":returned?"commission_delivery_returned":"commission_delivery_waiting",fingerprint);
    result.plan.statements.front()+=guard;
    if(complete)result.plan.statements.push_back("UPDATE organic_economy_commission SET state='completed',failure_reason='' WHERE commission_id="+
        SqlValue(job.agreement.id)+" AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+
        " AND state IN ('crafting','ready','traveling') AND EXISTS("+result.plan.receiptQuery+")");
    why=Blocker(proof.state);return true;
}
}
