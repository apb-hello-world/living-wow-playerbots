#include "LivingGatherRecovery.h"
#include "LivingLootQuote.h"
namespace LivingActivity {
bool DecodeInterruptedGather(const Task& task,const StoredCraftOperation& row,NativeGatherResult& result,std::string& why) {
    result={};auto reject=[&](const char* code){why=code;return false;};
    if(!IsGatheringRecoveryTask(task) || !row.acknowledged || row.receipt.task!=task.id ||
        !IsUuid(row.receipt.id) || task.revision<2 || row.receipt.taskRevision!=task.revision-1 ||
        row.receipt.kind!="gather_open" || row.receipt.state!=OperationState::Reconciling ||
        row.receipt.evidence!="native_gather_outcome_uncertain" || row.beforeState.size()>2048 || row.afterState.size()>4096 ||
        row.journalDigest.size()!=64 || row.journalDigest.find_first_not_of("0123456789abcdef")!=std::string::npos)
        return reject("gather_recovery_exact_journal_required");
    const std::string prefix="{\"effects\":21,\"persistence\":2,\"native\":";
    try {
        if(row.beforeState.compare(0,prefix.size(),prefix)!=0 || row.beforeState.back()!='}')
            return reject("gather_recovery_intent_shape_invalid");
        NativeGatherResult r;
        if(!DecodeNativeGatherQuote(row.beforeState.substr(prefix.size(),row.beforeState.size()-prefix.size()-1),r.before) ||
            row.beforeState!=prefix+EncodeNativeGatherQuote(r.before)+'}' || r.before.actor!=task.actor)
            return reject("gather_recovery_exact_quote_required");
        GuildProcurementJob job;
        if(!DecodeGuildProcurementJob(task.checkpoint.data,job,why) || job.entry!=r.before.entry || !job.craft.empty())
            return reject("gather_recovery_requested_item_changed");
        boost::property_tree::ptree p;std::istringstream input(row.afterState);boost::property_tree::read_json(input,p);
        r.value=LootQuoteDetail::Number(p,"value",65535);r.maximum=LootQuoteDetail::Number(p,"maximum",65535);
        r.money=LootQuoteDetail::Number(p,"money",UINT32_MAX);r.bagCount=LootQuoteDetail::Number(p,"bag_count",UINT32_MAX);
        r.generation=LootQuoteDetail::Number(p,"generation",UINT64_MAX);
        r.started=LootQuoteDetail::Boolean(p,"started");r.effect=LootQuoteDetail::Boolean(p,"effect");
        r.finished=LootQuoteDetail::Boolean(p,"finished");r.succeeded=LootQuoteDetail::Boolean(p,"succeeded");
        r.owned=LootQuoteDetail::Boolean(p,"owned");r.uncertain=LootQuoteDetail::Boolean(p,"uncertain");
        const bool typed=bool(p.get_child_optional("loot_type"));
        if(typed)r.lootType=LootQuoteDetail::Number(p,"loot_type",255);
        auto canonical=EncodeNativeGatherResult(r);
        if(!typed) {canonical.erase(canonical.rfind(",\"loot_type\":"));canonical+='}';}
        if(canonical!=row.afterState || !r.started || !r.effect || !r.finished || !r.succeeded || r.uncertain ||
            !r.generation || r.before.bagCount || r.bagCount || r.before.money!=r.money ||
            r.maximum!=r.before.maximum || r.value<r.before.value || r.value>r.maximum ||
            (typed && !NativeGatherLootType(r.before.skill,r.lootType)) ||
            row.receipt.nativeReference!="gather:"+std::to_string(r.before.source)+":generation:"+std::to_string(r.generation))
            return reject("gather_recovery_completed_zero_item_cast_required");
        result=r;why.clear();return true;
    } catch(const std::exception&) {return reject("gather_recovery_observation_invalid");}
}
bool PrepareInterruptedGather(const Task& saved,const WorldContext& current,const ProfessionHistory& history,
    const UnsettledClaimBatch& claims,const NativeGatherResult& native,uint64_t now,const std::string& receipt,
    GuildProcurementRecovery& out,std::string& why) {
    out={};auto reject=[&](const char* code){why=code;return false;};
    if(!Validate(saved,why) || !ValidateGuildProcurementTask(saved,why) || !IsGatheringRecoveryTask(saved) ||
        saved.mode!=Mode::Active || !saved.accepted || saved.root!=saved.id || !saved.parent.empty() ||
        !saved.context.boot.empty() || saved.context.actorGeneration || saved.context.mapGeneration ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration || !current.mapGeneration ||
        !current.policyRevision || !current.session.empty() || current.sessionRevision ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("gather_recovery_restored_context_required");
    if(!history.complete || history.task!=saved.id || history.revision!=saved.revision ||
        !history.unresolvedOperation || !history.interruptedGather ||
        !claims.complete || !claims.bookRevision || !claims.claims.empty())
        return reject("gather_recovery_empty_claims_and_history_required");
    const auto& row=*history.interruptedGather;NativeGatherResult prior;
    if(!DecodeInterruptedGather(saved,row,prior,why))return false;
    if(!ValidNativeGatherQuote(native.before) || EncodeNativeGatherQuote(native.before)!=EncodeNativeGatherQuote(prior.before) ||
        native.value!=prior.value || native.maximum!=prior.maximum || native.money!=prior.money ||
        native.bagCount || native.generation || native.owned || native.uncertain || native.lootType)
        return reject("gather_recovery_native_state_changed");
    auto n=[](uint64_t value){return std::to_string(value);};
    out.task=saved;auto& next=out.task;next.context=current;++next.revision;next.updatedAtMs=now;
    next.phase=Phase::Verifying;next.checkpoint.step="guild_procurement_prepare";
    next.checkpoint.blocker.clear();next.retryAtMs=0;
    auto outcome=row.receipt;outcome.state=OperationState::Rejected;outcome.evidence="native_gather_loot_abandoned_on_restart";
    const auto json="{\"recovery\":{\"version\":1,\"basis\":\"volatile_loot_lost_no_acquisition\",\"boot\":\""+current.boot+
        "\",\"acquired_quantity\":0},\"prior_observation\":"+row.afterState+'}';
    out.plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,json);
    out.plan.statements.front()+=" AND phase='reconciling' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(outcome.id)+
        " AND o.state='reconciling' AND o.before_state="+SqlValue(row.beforeState)+" AND o.after_state="+SqlValue(row.afterState)+
        " AND o.evidence_code="+SqlValue(row.receipt.evidence)+" AND o.native_reference="+SqlValue(row.receipt.nativeReference)+
        " AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(row.journalDigest)+')'+
        " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim WHERE task_id="+SqlValue(saved.id)+" AND state NOT IN ('consumed','released'))"+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation WHERE task_id="+SqlValue(saved.id)+" AND operation_id<>"+SqlValue(outcome.id)+')'+
        " AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery WHERE source_task_id="+SqlValue(saved.id)+')'+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task WHERE root_task_id="+SqlValue(saved.id)+" AND task_id<>"+SqlValue(saved.id)+
        " AND phase NOT IN ('completed','cancelled','failed'))"+
        " AND NOT EXISTS(SELECT 1 FROM item_instance WHERE owner_guid="+n(saved.actor)+" AND itemEntry="+n(prior.before.entry)+" AND count>0)"+
        " AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(saved.actor)+" AND money="+n(native.money)+')'+
        " AND EXISTS(SELECT 1 FROM character_skills WHERE guid="+n(saved.actor)+" AND skill="+n(prior.before.skill)+
        " AND value="+n(native.value)+" AND max="+n(native.maximum)+')';
    out.plan.statements.insert(out.plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    why.clear();return true;
}
}
