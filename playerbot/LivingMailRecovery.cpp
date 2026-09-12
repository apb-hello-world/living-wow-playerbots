#include "LivingMailRecovery.h"
#include "LivingActivityTransfer.h"
#include <algorithm>
namespace LivingActivity {
namespace {
const std::string prefix="{\"effects\":4,\"persistence\":1,\"native\":{\"native\":";
}
bool DecodeInterruptedMailQuote(const Task& task,const StoredCraftOperation& row,NativeMailQuote& q,std::string& why) {
    q={};auto reject=[&](const char* s){why=s;return false;};
    if(!IsProfessionJob(task) || task.mode!=Mode::Active || !task.accepted || task.id!=task.root ||
        task.phase!=Phase::Executing || task.checkpoint.step!="profession_mail_collect" ||
        !row.acknowledged || row.receipt.task!=task.id || !IsUuid(row.receipt.id) ||
        row.receipt.taskRevision!=task.revision || row.receipt.kind!="mail_collect" ||
        row.receipt.state!=OperationState::Intent || !row.receipt.evidence.empty() ||
        !row.receipt.nativeReference.empty() || row.afterState!="{}" || row.beforeState.size()>8192 ||
        row.beforeState.compare(0,prefix.size(),prefix)!=0) return reject("interrupted_mail_exact_intent_required");
    const auto end=row.beforeState.find(",\"transfer\":",prefix.size());
    if(end==std::string::npos || !DecodeNativeMailQuote(row.beforeState.substr(prefix.size(),end-prefix.size()),q) ||
        q.actor!=task.actor) return reject("interrupted_mail_quote_invalid");
    why.clear();return true;
}
bool PrepareInterruptedMail(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& batch,const MailRecoverySnapshot& native,uint64_t now,const std::string& receipt,
    ProfessionPreparation& result,std::string& why) {
    result={};auto reject=[&](const char* s){why=s;return false;};
    if(!saved.context.boot.empty() || saved.context.actorGeneration || saved.context.mapGeneration ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration ||
        !current.mapGeneration || !current.policyRevision || current.session.size()>120 ||
        (current.session.empty()!=(current.sessionRevision==0)) || !IsUuid(receipt) ||
        now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("interrupted_mail_restored_context_required");
    if(!history.complete || history.task!=saved.id || history.revision!=saved.revision ||
        !history.unresolvedOperation || !history.interruptedMail || history.interruptedCraft ||
        !batch.complete || !batch.bookRevision || batch.claims.empty() || batch.claims.size()>16)
        return reject("interrupted_mail_history_incomplete");
    const auto& row=*history.interruptedMail;NativeMailQuote q;
    if(!DecodeInterruptedMailQuote(saved,row,q,why))return false;
    const ResourceClaim* claim=nullptr;
    for(const auto& c:batch.claims) if(c.itemGuid==q.guid) {
        if(claim || !ValidMailTransfer(c) || c.task!=saved.id || c.actor!=q.actor || c.itemEntry!=q.entry ||
            c.quantity!=q.quantity || c.nativeReference!=q.mail) return reject("interrupted_mail_claim_changed");
        claim=&c;
    }
    if(!claim || row.beforeState!=prefix+EncodeNativeMailQuote(q)+",\"transfer\":"+ItemTransferIdentity(*claim)+"}}")
        return reject("interrupted_mail_claim_changed");
    if(EncodeNativeMailQuote(native.unchanged)!=EncodeNativeMailQuote(q))
        return reject("interrupted_mail_native_before_changed");
    const auto& d=native.destination;
    if(q.mergeGuid ? d.actor!=q.actor || d.guid!=q.mergeGuid || d.entry!=q.entry || d.count!=q.mergeCount ||
        d.slot!=uint8_t(q.to) : d.guid!=0)
        return reject("interrupted_mail_destination_changed");
    auto n=[](uint64_t v){return std::to_string(v);};
    std::string guard=" AND EXISTS(SELECT 1 FROM mail_items mi JOIN item_instance i ON i.guid=mi.item_guid WHERE mi.mail_id="+
        n(q.mail)+" AND mi.item_guid="+n(q.guid)+" AND mi.receiver="+n(q.actor)+" AND mi.item_template="+n(q.entry)+
        " AND i.owner_guid="+n(q.actor)+" AND i.itemEntry="+n(q.entry)+" AND i.count="+n(q.quantity)+')'+
        " AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+n(q.guid)+')'+
        " AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+n(q.mail)+" AND m.receiver="+n(q.actor)+
        " AND m.cod=0 AND m.money="+n(q.mailMoney)+" AND m.deliver_time="+n(q.deliveredAt)+" AND m.expire_time="+n(q.expiresAt)+')'+
        " AND (SELECT COUNT(*) FROM mail_items WHERE mail_id="+n(q.mail)+")="+n(q.attachmentsBefore)+
        " AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(q.actor)+" AND money="+n(q.moneyBefore)+')'+
        " AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+TransferClaimPredicate(*claim)+')';
    if(q.mergeGuid)guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
        n(q.actor)+" AND v.item="+n(d.guid)+" AND v.item_template="+n(d.entry)+" AND v.bag="+n(d.bagGuid)+
        " AND v.slot="+n(d.slot)+" AND i.owner_guid="+n(q.actor)+" AND i.itemEntry="+n(q.entry)+" AND i.count="+n(d.count)+')';
    else guard+=" AND NOT EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid="+n(q.actor)+
        " AND v.slot="+n(uint8_t(q.to))+" AND v.bag="+n(d.bagGuid)+')';
    ProfessionPreparation prepared;prepared.task=saved;auto& next=prepared.task;
    next.context=current;++next.revision;next.phase=Phase::Verifying;next.updatedAtMs=now;
    next.checkpoint.step="profession_prepare";next.checkpoint.blocker.clear();
    auto outcome=row.receipt;outcome.state=OperationState::Rejected;outcome.evidence="native_mail_intent_not_committed";
    outcome.nativeReference="mail:"+n(q.mail)+":item:"+n(q.guid);
    const auto after="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        current.boot+"\"},\"unchanged\":"+EncodeNativeMailQuote(q)+'}';
    prepared.plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,after);
    prepared.plan.statements.front()+=" AND phase='executing' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(outcome.id)+
        " AND o.state='intent' AND o.kind='mail_collect' AND o.before_state="+SqlValue(row.beforeState)+
        " AND o.after_state='{}' AND o.evidence_code='' AND o.native_reference='')"+
        " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task owner ON owner.task_id=o.task_id"
        " WHERE owner.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"+guard;
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    result=std::move(prepared);why.clear();return true;
}
}
