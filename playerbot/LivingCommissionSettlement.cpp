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
        Require(inputs.data().empty() && inputs.size()>=2 && inputs.size()<=16 &&
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
        if(q.count>q.quantity) {
            EnchantCodec::Object(native,{"mail","item","receiver","cod","delivered_at","expires_at","postage","customer_received","fee_paid","surplus_item","surplus_count"});
            const auto surplus=Number(native,"surplus_item");
            Require(surplus && surplus<=UINT32_MAX && surplus!=q.item && Number(native,"surplus_count")==q.count-q.quantity,
                "commission_surplus_receipt_invalid");
        } else EnchantCodec::Object(native,{"mail","item","receiver","cod","delivered_at","expires_at","postage","customer_received","fee_paid"});
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
                proof.returned=r.id;proof.returnedMail=uint32_t(eventMail);
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
bool ReturnedCommissionClaim(const Task& task,const ProfessionHistory& history,ResourceClaim& result,std::string& why) {
    result={};CommissionDeliveryProof proof;
    if(!InspectCommissionDelivery(task,history,proof,why))return false;
    if(proof.state!=CommissionDeliveryState::Returned || !proof.returnedMail) {why="commission_verified_return_required";return false;}
    ResourceClaim c;c.id=proof.returned;c.task=task.id;c.actor=task.actor;
    c.itemGuid=proof.quote.item;c.itemEntry=proof.quote.entry;c.quantity=proof.quote.quantity;
    c.location="mail";c.nativeReference=proof.returnedMail;c.state="held";
    if(!ValidResourceClaim(c)){why="commission_return_claim_invalid";return false;}
    result=std::move(c);why.clear();return true;
}
std::string ReturnedCommissionClaimGuard(const Task& task,const ProfessionHistory& history,const ResourceClaim& claim) {
    ResourceClaim exact;std::string why;
    if(!ReturnedCommissionClaim(task,history,exact,why) || !SameResourceClaim(exact,claim))return " AND 1=0";
    CommissionDeliveryProof proof;if(!InspectCommissionDelivery(task,history,proof,why))return " AND 1=0";
    auto n=[](uint64_t v){return std::to_string(v);};
    std::string guard=" AND EXISTS(SELECT 1 FROM mail m JOIN mail_items a ON a.mail_id=m.id JOIN item_instance i ON i.guid=a.item_guid"
        " WHERE m.id="+n(claim.nativeReference)+" AND m.messageType=0 AND m.receiver="+n(task.actor)+
        " AND m.sender="+n(proof.quote.receiver)+" AND m.money=0 AND m.cod=0 AND (m.checked&2)=2 AND m.subject="+
        SqlValue(CommissionMailSubject(proof.send))+" AND a.receiver=m.receiver AND a.item_guid="+n(claim.itemGuid)+
        " AND a.item_template="+n(claim.itemEntry)+" AND i.owner_guid=m.receiver AND i.itemEntry=a.item_template AND i.count="+n(claim.quantity)+')'+
        " AND (SELECT COUNT(*) FROM mail_items WHERE mail_id="+n(claim.nativeReference)+")=1"
        " AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+n(claim.itemGuid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(claim.itemGuid)+')';
    for(const auto& row:history.commissionMail)guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
        SqlValue(row.receipt.id)+" AND o.task_id=living_activity_task.task_id AND o.state='verified' AND o.kind="+
        SqlValue(row.receipt.kind)+" AND o.native_reference="+SqlValue(row.receipt.nativeReference)+
        " AND o.evidence_code="+SqlValue(row.receipt.evidence)+" AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+')';
    return guard;
}
bool PrepareCommissionReturnResume(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,uint64_t now,
    const std::string& receipt,ProfessionPreparation& out,std::string& why) {
    out={};ResourceClaim parcel;
    if(!ReturnedCommissionClaim(saved,history,parcel,why))return false;
    auto reject=[&](const char* code){why=code;return false;};
    if(!saved.context.boot.empty() || saved.context.actorGeneration || saved.context.mapGeneration ||
        Terminal(saved.phase) || saved.phase==Phase::Executing || !saved.accepted || saved.mode!=Mode::Active ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration || !current.mapGeneration || !current.policyRevision ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 || !batch.complete || !batch.bookRevision ||
        batch.claims.size()>16 || balances.size()>16)return reject("commission_return_resume_context_invalid");
    auto n=[](uint64_t v){return std::to_string(v);};
    std::string guard=ReturnedCommissionClaimGuard(saved,history,parcel),fingerprint;
    std::set<std::string> ids;std::set<uint32_t> items;
    bool parcelClaim=false;
    for(const auto& c:batch.claims) {
        if(!ValidResourceClaim(c) || c.actor!=saved.actor || c.task!=saved.id || c.state!="held" ||
            !ids.insert(c.id).second || !items.insert(c.itemGuid).second)return reject("commission_return_resume_claim_invalid");
        if(c.id==parcel.id) {
            if(!SameResourceClaim(c,parcel))return reject("commission_return_resume_custody_changed");
            parcelClaim=true;
        } else {
            if(c.location!="bank" || c.nativeReference || c.copper || c.itemEntry==parcel.itemEntry)
                return reject("commission_return_resume_claim_invalid");
            const NativeResourceBalance* owned=nullptr;
            for(const auto& b:balances)if(b.itemGuid==c.itemGuid) {if(owned)return reject("commission_return_resume_stock_ambiguous");owned=&b;}
            if(!owned || !ValidNativeResourceBalance(*owned) || owned->actor!=saved.actor || owned->location!="bank" ||
                owned->nativeReference || owned->itemEntry!=c.itemEntry || owned->quantity<c.quantity)
                return reject("commission_return_resume_stock_changed");
            guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item"
                " WHERE v.guid="+n(saved.actor)+" AND i.owner_guid=v.guid AND i.guid="+n(c.itemGuid)+
                " AND i.itemEntry="+n(c.itemEntry)+" AND i.count="+n(owned->quantity)+')'+
                " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(c.itemGuid)+')'+
                " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(c.itemGuid)+')';
        }
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+SettlementClaimWhere(c)+')';
        fingerprint+=SettlementClaimWhere(c);
    }
    if(!batch.claims.empty() && !parcelClaim)return reject("commission_return_resume_parcel_claim_missing");
    guard+=" AND (SELECT COUNT(*) FROM living_activity_claim WHERE task_id=living_activity_task.task_id"
        " AND state NOT IN ('released','consumed'))="+n(batch.claims.size())+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))";
    CommissionJob job;DecodeCommissionJob(saved.checkpoint.data,job,why);
    guard+=" AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE commission_id="+SqlValue(job.agreement.id)+
        " AND bot_guid="+n(saved.actor)+" AND player_guid="+n(job.agreement.recipient)+
        " AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+" AND state IN ('crafting','ready','traveling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation WHERE task_id=living_activity_task.task_id"
        " AND kind IN ('commission_customer_received','commission_fee_collected'))";
    auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;next.phase=Phase::Preparing;
    next.checkpoint.step="commission_return_collect";next.checkpoint.blocker.clear();
    // Keep any due time/backoff; a restart is not permission to retry faster.
    if(!Validate(next,why) || !ValidateCommissionTask(next,why))return false;
    auto plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"commission_return_resumed",fingerprint);
    plan.statements.front()+=" AND checkpoint="+SqlValue(saved.checkpoint.data)+" AND accepted=1 AND mode='active'"+guard;
    plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    out.task=std::move(next);out.plan=std::move(plan);why.clear();return true;
}
bool PrepareCommissionParcelClaims(const Task& saved,const WorldContext& current,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,uint64_t now,const std::string& receipt,
    CommissionReturnClosure& out,std::string& why) {
    out={};CommissionJob job;ProfessionJob recipe;
    if(!IsCommissionJob(saved) || !ValidateCommissionTask(saved,why) || !DecodeCommissionJob(saved.checkpoint.data,job,why) ||
        !job.craftFinishedRevision || job.agreement.delivery!="mail" || !DecodeProfessionIntent(job.craft,recipe,why))return false;
    auto reject=[&](const char* code){why=code;return false;};
    // Native teleport/group lifecycle changes invalidate grants during the
    // same process too. Rebind the durable obligation only after full native
    // claim/consent reconciliation below; this transition grants no authority.
    const bool rebind=!(saved.context==current);
    if(Terminal(saved.phase) || saved.phase==Phase::Executing || saved.phase==Phase::Reconciling || !saved.accepted || saved.mode!=Mode::Active ||
        current.actor!=saved.actor || !IsUuid(current.boot) ||
        !current.actorGeneration || !current.mapGeneration || !current.policyRevision || !IsUuid(receipt) ||
        now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 || !batch.complete || !batch.bookRevision)
        return reject("commission_parcel_claim_context_invalid");
    PersonalResourceSettlement resources;
    if(!PreparePersonalResourceSettlement(saved,batch,balances,resources,why,"commission_parcel_"))return false;
    UnsettledClaimBatch release=batch;release.claims.clear();
    std::vector<ClaimReceiptChange> changes;uint64_t output=0;bool postage=false;
    for(size_t i=0;i<batch.claims.size();++i) {
        const auto& c=batch.claims[i];
        if(c.state!="held" || c.nativeReference)return reject("commission_parcel_claim_unreconciled");
        if(c.location=="bags" && c.itemEntry==recipe.outputEntry)output+=c.quantity;
        else if(c.location=="money" && c.copper==30 && !postage)postage=true;
        else if((c.location=="bags" || c.location=="bank") && c.itemEntry!=recipe.outputEntry && !c.copper) {
            release.claims.push_back(c);changes.push_back(resources.claims[i]);
        } else return reject("commission_parcel_claim_unreconciled");
    }
    if(output!=recipe.outputQuantity)return reject("commission_parcel_exact_output_claims_required");
    if(release.claims.empty() && !rebind)return reject("commission_parcel_claims_already_ready");
    resources.claims=changes; // Keep full-batch guards, release only auxiliaries.
    const auto n=[](uint64_t v){return std::to_string(v);};
    auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;next.phase=Phase::Preparing;
    next.checkpoint.step="commission_mail_prepare";next.checkpoint.blocker.clear();
    if(!Validate(next,why) || !ValidateCommissionTask(next,why))return false;
    auto plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"commission_parcel_claims_reconciled",resources.fingerprint);
    auto& guard=plan.statements.front();
    guard+=" AND accepted=1 AND mode='active' AND checkpoint="+SqlValue(saved.checkpoint.data)+resources.guards+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation WHERE task_id=living_activity_task.task_id"
        " AND kind='commission_mail_send' AND state<>'rejected')"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))"
        " AND EXISTS(SELECT 1 FROM living_activity_transition WHERE task_id=living_activity_task.task_id AND task_revision="+
        n(job.craftFinishedRevision)+" AND code='commission_craft_verified')"
        " AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE commission_id="+SqlValue(job.agreement.id)+
        " AND bot_guid="+n(saved.actor)+" AND player_guid="+n(job.agreement.recipient)+
        " AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+" AND state IN ('crafting','ready','traveling'))";
    for(const auto& b:balances) {
        if(b.location=="money") {
            guard+=" AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(saved.actor)+" AND money="+n(b.copper)+')';continue;
        }
        guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+n(saved.actor)+
            " AND i.owner_guid=v.guid AND i.guid="+n(b.itemGuid)+" AND i.itemEntry="+n(b.itemEntry)+" AND i.count="+n(b.quantity)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(b.itemGuid)+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(b.itemGuid)+')';
    }
    for(const auto& c:release.claims)if(c.location=="bank")guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o"
        " WHERE o.task_id=living_activity_task.task_id AND o.kind='bank_deposit' AND o.state='verified'"
        " AND o.evidence_code='native_bank_stack_deposited' AND o.task_revision<="+n(saved.revision)+
        " AND JSON_EXTRACT(o.before_state,'$.native.native.guid')="+n(c.itemGuid)+
        " AND JSON_EXTRACT(o.before_state,'$.native.native.entry')="+n(c.itemEntry)+
        " AND JSON_EXTRACT(o.before_state,'$.native.native.quantity')="+n(c.quantity)+')';
    AppendPersonalResourceSettlement(plan,resources,release,now,receipt);
    plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    out.task=std::move(next);out.plan=std::move(plan);out.claims=std::move(changes);why.clear();return true;
}
bool PrepareCommissionReturnClosure(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,uint64_t now,
    const std::string& receipt,CommissionReturnClosure& out,std::string& why) {
    out={};ResourceClaim original;CommissionDeliveryProof proof;
    if(!ReturnedCommissionClaim(saved,history,original,why) || !InspectCommissionDelivery(saved,history,proof,why))return false;
    auto reject=[&](const char* code){why=code;return false;};
    const bool restored=saved.context.boot.empty() && !saved.context.actorGeneration && !saved.context.mapGeneration;
    if(Terminal(saved.phase) || saved.phase==Phase::Executing || !saved.accepted || saved.mode!=Mode::Active ||
        (!(saved.context==current) && !restored) || current.actor!=saved.actor || !IsUuid(current.boot) ||
        !current.actorGeneration || !current.mapGeneration || !current.policyRevision ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 || !batch.complete || !batch.bookRevision)
        return reject("commission_return_closure_context_invalid");
    const ResourceClaim* collected=nullptr;
    for(const auto& c:batch.claims) {
        if(c.id==original.id) {
            if(c.state!="held" || c.location!="bags" || c.nativeReference || !c.itemGuid ||
                c.itemEntry!=original.itemEntry || c.quantity!=original.quantity || c.copper || c.revision<=original.revision)
                return reject("commission_return_collection_unverified");
            collected=&c;
        } else if(c.state!="held" || c.location!="bank" || c.nativeReference || c.copper || c.itemEntry==original.itemEntry)
            return reject("commission_return_other_claim_unreconciled");
    }
    if(!collected)return reject("commission_return_collection_claim_missing");
    PersonalResourceSettlement resources;
    if(!PreparePersonalResourceSettlement(saved,batch,balances,resources,why,"commission_return_"))return false;
    const auto n=[](uint64_t v){return std::to_string(v);};
    const auto number=[](const char* key){return "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native."+
        std::string(key)+"')) AS UNSIGNED),0)";};
    const std::string reason="commission_parcel_returned_items_preserved";
    auto next=saved;next.context=current;++next.revision;next.phase=Phase::Failed;next.updatedAtMs=now;
    next.checkpoint.step="commission_returned";next.checkpoint.blocker=reason;next.retryAtMs=0;
    if(!Validate(next,why) || !ValidateCommissionTask(next,why))return false;
    auto plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,reason,resources.fingerprint+'|'+proof.returned);
    auto& guard=plan.statements.front();
    guard+=" AND accepted=1 AND mode='active' AND checkpoint="+SqlValue(saved.checkpoint.data)+resources.guards+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    for(const auto& row:history.commissionMail)guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
        SqlValue(row.receipt.id)+" AND o.task_id=living_activity_task.task_id AND o.state='verified' AND o.kind="+
        SqlValue(row.receipt.kind)+" AND o.task_revision="+n(row.receipt.taskRevision)+
        " AND o.evidence_code="+SqlValue(row.receipt.evidence)+" AND o.native_reference="+SqlValue(row.receipt.nativeReference)+
        " AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+')';
    guard+=" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND "
        "((o.kind='commission_mail_send' AND o.state<>'rejected') OR o.kind IN ('commission_customer_received','commission_fee_collected','commission_parcel_returned')))="+
        n(history.commissionMail.size());
    // Paid reagent collections earlier in this same job are not return proof.
    // Match the exact returned envelope and original attachment, then follow
    // the native surviving GUID when TakeItem merged it into an existing stack.
    const auto collection="o.task_id=living_activity_task.task_id AND o.kind='mail_collect' AND o.state='verified' AND "+
        number("actor")+'='+n(saved.actor)+" AND "+number("mail")+'='+n(original.nativeReference)+
        " AND "+number("guid")+'='+n(original.itemGuid)+" AND "+number("entry")+'='+n(original.itemEntry)+
        " AND "+number("quantity")+'='+n(original.quantity);
    uint64_t returnRevision=0;
    for(const auto& row:history.commissionMail)if(row.receipt.id==proof.returned)returnRevision=row.receipt.taskRevision;
    guard+=" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE "+collection+")=1"
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE "+collection+
        " AND o.evidence_code='native_mail_attachment_collected' AND o.task_revision>"+n(returnRevision)+
        " AND o.task_revision<="+n(saved.revision)+" AND o.native_reference="+
        SqlValue("mail:"+n(original.nativeReference)+":item:"+n(original.itemGuid))+
        " AND JSON_EXTRACT(o.after_state,'$.native.mail')="+n(original.nativeReference)+
        " AND JSON_EXTRACT(o.after_state,'$.native.guid')="+n(original.itemGuid)+
        " AND JSON_EXTRACT(o.after_state,'$.native.surviving_guid')="+n(collected->itemGuid)+')';
    // Remaining predicates are built below, without querying or mutating mail.
    CommissionJob job;DecodeCommissionJob(saved.checkpoint.data,job,why);
    guard+=" AND NOT EXISTS(SELECT 1 FROM mail_items WHERE mail_id="+n(original.nativeReference)+')';
    for(const auto& b:balances)guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item"
        " WHERE v.guid="+n(saved.actor)+" AND i.owner_guid=v.guid AND i.guid="+n(b.itemGuid)+
        " AND v.item_template="+n(b.itemEntry)+" AND i.itemEntry=v.item_template AND i.count="+n(b.quantity)+')'+
        " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(b.itemGuid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(b.itemGuid)+')';
    for(const auto& c:batch.claims)if(c.location=="bank")guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o"
        " WHERE o.task_id=living_activity_task.task_id AND o.kind='bank_deposit' AND o.state='verified'"
        " AND o.evidence_code='native_bank_stack_deposited' AND "+number("guid")+'='+n(c.itemGuid)+
        " AND "+number("entry")+'='+n(c.itemEntry)+" AND "+number("quantity")+'='+n(c.quantity)+')';
    const auto agreement="commission_id="+SqlValue(job.agreement.id)+" AND bot_guid="+n(saved.actor)+
        " AND player_guid="+n(job.agreement.recipient)+" AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement));
    guard+=" AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE "+agreement+" AND state IN ('crafting','ready','traveling'))"
        " AND EXISTS(SELECT 1 FROM living_activity_transition WHERE task_id=living_activity_task.task_id AND task_revision="+
        n(job.craftFinishedRevision)+" AND code='commission_craft_verified')";
    plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    AppendPersonalResourceSettlement(plan,resources,batch,now,receipt);
    plan.statements.push_back("UPDATE organic_economy_commission SET state='failed',failure_reason="+SqlValue(reason)+
        " WHERE "+agreement+" AND state IN ('crafting','ready','traveling') AND EXISTS("+plan.receiptQuery+')');
    plan.receiptQuery+=" AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE "+agreement+
        " AND state='failed' AND failure_reason="+SqlValue(reason)+')';
    out.task=std::move(next);out.plan=std::move(plan);out.claims=std::move(resources.claims);why.clear();return true;
}
bool DecodeUnsentCommission(const Task& task,const ProfessionHistory& history,const UnsettledClaimBatch& claims,
    CommissionMailQuote& quote,std::string& why) {
    quote={};why="commission_unsent_exact_intent_required";
    try {
        std::string validation;
        if(!ValidateCommissionTask(task,validation) || !IsCommissionJob(task) || !task.accepted || task.mode!=Mode::Active ||
            task.phase!=Phase::Executing || task.checkpoint.step!="commission_mail_send" ||
            !history.complete || history.task!=task.id || history.revision!=task.revision || !history.unresolvedOperation ||
            history.commissionMail.size()!=1 || !claims.complete || !claims.bookRevision || claims.claims.size()<2 || claims.claims.size()>16)return false;
        const auto& row=history.commissionMail.front();const auto& r=row.receipt;
        if(!row.acknowledged || !Digest(row.journalDigest) || !IsUuid(r.id) || r.task!=task.id || r.taskRevision!=task.revision ||
            r.kind!="commission_mail_send" || r.state!=OperationState::Intent || !r.evidence.empty() ||
            !r.nativeReference.empty() || row.afterState!="{}")return false;
        const auto before=Parse(row.beforeState);
        if(!DecodeCommissionMailQuote(EnchantCodec::Json(before.get_child("native.native")),quote))return false;
        std::vector<ClaimConsumption> uses;
        for(const auto& c:claims.claims)uses.push_back({c,c.copper?30u:uint32_t(c.quantity)});
        if(!ExactCommissionMailConsumption(task,quote,uses))return false;
        // The shared encoder canonicalizes claim ordering before comparison.
        if(row.beforeState!="{\"effects\":12,\"persistence\":1,\"native\":"+
            ClaimedNativeState(EncodeCommissionMailQuote(quote),uses,8192)+'}')return false;
        why.clear();return true;
    } catch(const std::exception&) {why="commission_unsent_intent_malformed";return false;}
}
bool DecodeInterruptedCommission(const Task& saved,const ProfessionHistory& history,const UnsettledClaimBatch& claims,
    CommissionMailQuote& q,AuctionMail& captured,std::string& why) {
    captured={};
    if(DecodeUnsentCommission(saved,history,claims,q,why))return true;
    if(saved.phase!=Phase::Reconciling || saved.revision<2 || history.commissionMail.size()!=1)
        return false;
    const auto& row=history.commissionMail.front();const auto& r=row.receipt;
    if(r.state!=OperationState::Reconciling || r.taskRevision!=saved.revision-1 ||
        (r.evidence!="native_save_capture_requires_reconciliation" && r.evidence!="claim_outcome_requires_reconciliation" &&
         r.evidence!="commission_mail_native_custody_uncertain" && r.evidence!="commission_mail_native_split_only" &&
         r.evidence!="commission_mail_split_requires_reconciliation" &&
         r.evidence!="native_consumption_delta_mismatch")) {why="commission_interrupted_observation_invalid";return false;}
    // Reuse the exact intent/claims decoder; this read-only normalization is
    // never persisted. Recovery SQL compares the ORIGINAL uncertain record.
    auto intentTask=saved;intentTask.phase=Phase::Executing;
    auto intentHistory=history;auto& intent=intentHistory.commissionMail.front();
    intent.receipt.state=OperationState::Intent;intent.receipt.taskRevision=saved.revision;
    intent.receipt.evidence.clear();intent.receipt.nativeReference.clear();intent.afterState="{}";
    if(!DecodeUnsentCommission(intentTask,intentHistory,claims,q,why))return false;
    if(row.afterState=="{}" && r.nativeReference.empty()){why.clear();return true;}
    try {
        const auto observation=Parse(row.afterState);
        if(observation.get_optional<std::string>("mail_not_sent")) {
            EnchantCodec::Object(observation,{"mail_not_sent","surplus_item","surplus_count"});
            const auto extra=Number(observation,"surplus_item");
            Require(q.count>q.quantity && observation.get<std::string>("mail_not_sent")=="true" && extra && extra<=UINT32_MAX &&
                extra!=q.item && Number(observation,"surplus_count")==q.count-q.quantity &&
                r.nativeReference=="item:"+std::to_string(q.item),"commission_split_only_observation_invalid");
            why.clear();return true;
        }
        // The capture-failure path retains the native observation, without the
        // successful consumed-claims envelope. Validate it with the same send
        // receipt decoder used after ordinary execution, not a looser schema.
        auto verified=history;verified.unresolvedOperation=false;
        auto& sent=verified.commissionMail.front();sent.receipt.state=OperationState::Verified;
        sent.receipt.evidence="native_commission_parcel_and_postage_observed";
        std::vector<ClaimConsumption> uses;for(const auto& c:claims.claims)uses.push_back({c,c.copper?30u:uint32_t(c.quantity)});
        sent.afterState=ClaimedNativeState(row.afterState,uses,8192);
        CommissionDeliveryProof proof;
        if(!InspectCommissionDelivery(saved,verified,proof,why))return false;
        const auto p=Parse(row.afterState);
        captured.id=proof.mail;captured.sender=q.sender;captured.receiver=q.receiver;captured.cod=q.cod;
        captured.itemGuid=q.item;captured.itemEntry=q.entry;captured.quantity=q.quantity;captured.attachments=1;
        captured.deliveredAt=Number(p,"delivered_at");captured.expiresAt=Number(p,"expires_at");
        captured.subject=CommissionMailSubject(r.id);why.clear();return true;
    } catch(const std::exception&) {why="commission_interrupted_observation_malformed";return false;}
}
namespace {
std::string InterruptedCommissionGuard(const Task& saved,const ProfessionHistory& history,size_t claimCount) {
    const auto& row=history.commissionMail.front();const auto& r=row.receipt;
    CommissionJob job;std::string why;DecodeCommissionJob(saved.checkpoint.data,job,why);
    const auto n=[](uint64_t v){return std::to_string(v);};
    return " AND phase="+SqlValue(Name(saved.phase))+" AND accepted=1 AND mode='active' AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(r.id)+
        " AND o.task_id=living_activity_task.task_id AND o.task_revision="+n(r.taskRevision)+
        " AND o.kind='commission_mail_send' AND o.state="+SqlValue(r.state==OperationState::Intent?"intent":"reconciling")+
        " AND o.before_state="+SqlValue(row.beforeState)+" AND o.after_state="+SqlValue(row.afterState)+
        " AND o.evidence_code="+SqlValue(r.evidence)+" AND o.native_reference="+SqlValue(r.nativeReference)+
        " AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+')'+
        " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"
        " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND "
        "((o.kind='commission_mail_send' AND o.state<>'rejected') OR o.kind IN ('commission_customer_received','commission_fee_collected','commission_parcel_returned')))=1"
        " AND (SELECT COUNT(*) FROM living_activity_claim WHERE task_id=living_activity_task.task_id AND state NOT IN ('consumed','released'))="+n(claimCount)+
        " AND EXISTS(SELECT 1 FROM living_activity_transition WHERE task_id=living_activity_task.task_id AND task_revision="+
        n(job.craftFinishedRevision)+" AND code='commission_craft_verified')"
        " AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE commission_id="+SqlValue(job.agreement.id)+
        " AND bot_guid=living_activity_task.actor_guid AND player_guid="+n(job.agreement.recipient)+
        " AND authoritative_payload="+SqlValue(EncodeCommissionContract(job.agreement))+" AND state IN ('crafting','ready','traveling'))";
}
bool RestoredSendContext(const Task& saved,const WorldContext& current,uint64_t now,const std::string& receipt) {
    return saved.context.boot.empty() && !saved.context.actorGeneration && !saved.context.mapGeneration &&
        current.actor==saved.actor && IsUuid(current.boot) && current.actorGeneration && current.mapGeneration &&
        current.policyRevision && current.session.size()<=120 && current.session.empty()==(current.sessionRevision==0) &&
        IsUuid(receipt) && now>=saved.updatedAtMs && saved.revision<UINT64_MAX-1;
}
}
bool PrepareCapturedCommissionSend(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,CommissionSendRecovery& result,std::string& why) {
    result={};CommissionMailQuote q;AuctionMail captured;
    if(!DecodeInterruptedCommission(saved,history,claims,q,captured,why))return false;
    if(!captured.id || !RestoredSendContext(saved,current,now,receipt)) {why="commission_captured_send_restored_proof_required";return false;}
    auto next=saved;next.context=current;++next.revision;next.updatedAtMs=now;next.phase=Phase::Verifying;
    next.checkpoint.step="commission_mail_send";next.checkpoint.blocker.clear();next.retryAtMs=0;
    const auto& row=history.commissionMail.front();auto outcome=row.receipt;outcome.state=OperationState::Verified;
    outcome.evidence="native_commission_parcel_and_postage_observed";
    std::vector<ClaimConsumption> uses;for(const auto& c:claims.claims)uses.push_back({c,c.copper?30u:uint32_t(c.quantity)});
    auto consumed=ConsumedOperationWrite(next,saved.revision,outcome,receipt,row.afterState,uses);
    const auto surplus=q.count>q.quantity?uint32_t(Number(Parse(row.afterState),"surplus_item")):0;
    consumed.journal.statements.front()+=InterruptedCommissionGuard(saved,history,claims.claims.size())+
        " AND EXISTS("+CommissionMailSentProof(next,q,captured,outcome.id,surplus)+')'+
        " AND (SELECT COUNT(*) FROM mail WHERE subject="+SqlValue(captured.subject)+")=1";
    consumed.journal.statements.insert(consumed.journal.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
    result.task=std::move(next);result.plan=std::move(consumed.journal);result.claims=std::move(consumed.changes);
    why.clear();return true;
}
bool PrepareUnsentCommission(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,const CommissionMailQuote& native,uint32_t bagGuid,uint64_t now,
    const std::string& receipt,ProfessionPreparation& result,std::string& why) {
    result={};CommissionMailQuote q;AuctionMail captured;
    if(!DecodeInterruptedCommission(saved,history,claims,q,captured,why))return false;
    why="commission_unsent_restored_context_required";
    if(!RestoredSendContext(saved,current,now,receipt))return false;
    const auto& row=history.commissionMail.front();
    const bool splitOnly=q.count>q.quantity && native.count==q.quantity &&
        Parse(row.afterState).get_optional<std::string>("mail_not_sent").is_initialized();
    auto compared=native;if(splitOnly)compared.count=q.count;
    if(!ValidCommissionMailQuote(compared) || EncodeCommissionMailQuote(compared)!=EncodeCommissionMailQuote(q)) {
        why="commission_unsent_native_state_changed";return false;
    }
    auto n=[](uint64_t value){return std::to_string(value);};
    std::string guard=InterruptedCommissionGuard(saved,history,claims.claims.size())+
        " AND NOT EXISTS(SELECT 1 FROM mail WHERE subject="+SqlValue(CommissionMailSubject(row.receipt.id))+')'+
        " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(q.item)+')'+
        " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(q.item)+')'+
        " AND EXISTS(SELECT 1 FROM characters c JOIN character_inventory v ON v.guid=c.guid JOIN item_instance i ON i.guid=v.item"
        " WHERE c.guid="+n(q.sender)+" AND c.money="+n(q.moneyBefore)+" AND i.guid="+n(q.item)+
        " AND i.owner_guid=c.guid AND i.itemEntry="+n(q.entry)+" AND i.count="+n(native.count)+
        " AND v.bag="+n(bagGuid)+" AND v.slot="+n(uint8_t(q.position))+')'+
        " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id AND c.state NOT IN ('consumed','released'))="+n(claims.claims.size());
    if(q.count>q.quantity) {
        if(splitOnly) {
            const auto extra=Number(Parse(row.afterState),"surplus_item");
            guard+=" AND EXISTS(SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid WHERE i.guid="+n(extra)+
                " AND i.owner_guid="+n(q.sender)+" AND v.guid="+n(q.sender)+" AND i.itemEntry="+n(q.entry)+
                " AND i.count="+n(q.count-q.quantity)+" AND v.bag="+n(q.splitBagGuid)+" AND v.slot="+n(q.splitPosition&255)+')'+
                " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(extra)+')'+
                " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(extra)+')';
        } else guard+=" AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE guid="+n(q.sender)+
            " AND bag="+n(q.splitBagGuid)+" AND slot="+n(q.splitPosition&255)+')';
    }
    for(const auto& c:claims.claims)guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+ConsumptionClaimPredicate(c)+')';
    auto next=saved;next.context=current;++next.revision;next.phase=Phase::Verifying;next.updatedAtMs=now;
    next.checkpoint.step="commission_mail_prepare";next.checkpoint.blocker.clear();next.retryAtMs=0;
    auto outcome=row.receipt;outcome.state=OperationState::Rejected;outcome.evidence="native_commission_send_not_committed";
    outcome.nativeReference="item:"+n(q.item);
    auto plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,"{\"recovery\":\"atomic_send_absent\",\"unchanged\":"+
        EncodeCommissionMailQuote(q)+",\"prior_observation\":"+row.afterState+'}');
    plan.statements.front()+=guard;
    plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    result.task=std::move(next);result.plan=std::move(plan);why.clear();return true;
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
