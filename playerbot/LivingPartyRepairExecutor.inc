// A finite maintenance root on the existing due queue. Party permission is
// transient; the accepted obligation and repair escrow survive recall/restart.
std::optional<LivingActivity::PartyServiceBinding> LivingActivityCoordinator::PreparePartyRepairService(
    uint32_t actor,const std::string& sourceKey) {
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !ProfessionStoreReady() ||
        !state->resources.Protection().ready || !IsSourceKey(sourceKey) ||
        !sPlayerbotRendezvousManager.IsPartyFreeTime(actor))return {};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetGroup())return {};
    auto binding=[](const Task& task) {
        PartyServiceBinding result;result.root=task.id;result.actor=task.actor;
        result.acceptedRevision=task.revision;result.service=PartyServiceBinding::Service::Repair;
        return result;
    };
    const Task* chosen=nullptr;
    const auto indexed=state->cachedByActor.find(actor);
    if(indexed!=state->cachedByActor.end())for(const auto& id:indexed->second) {
        const auto& task=state->cache.at(id);
        if(IsPartyRepairTask(task) && task.accepted && task.mode==Mode::Active && !Terminal(task.phase) &&
            (!chosen || Before(task,*chosen)))chosen=&task;
    }
    if(chosen)return binding(*chosen);
    for(const auto& write:state->pending)if(write.task.actor==actor && IsPartyRepairTask(write.task) &&
        !Terminal(write.task.phase))return binding(write.task);
    if(NativeSafety(bot) || !HasNativeDamagedEquipment(*bot))return {};
    TaskRequest request;auto& task=request.task;
    task.id=task.root=SourceId("party_repair",sourceKey);task.source="party_repair";task.sourceKey=sourceKey;
    task.actor=actor;task.kind=Kind::PartyErrand;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.priority=Priority::Preparation;task.accepted=true;
    task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
    task.createdAtMs=task.updatedAtMs=NowMs();task.checkpoint.step="maintenance_repair_prepare";
    task.checkpoint.data="{\"workflow\":\"party_repair_v1\"}";request.receipt=NewId();
    const auto admitted=SubmitTask(request);
    if(admitted.code!=AdmissionCode::Pending && admitted.code!=AdmissionCode::Saved)return {};
    return binding(task);
}

LivingActivityCoordinator::ProfessionProgress LivingActivityCoordinator::AdvancePartyRepair(
    uint32_t actor,const std::string& id) {
    auto stop=[](const std::string& why){return ProfessionProgress{false,why};};
    const auto saved=ReadSavedTask(id);
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !saved || saved->actor!=actor ||
        !IsPartyRepairTask(*saved) || state->executingTask!=id)return stop("party_repair_saved_task_required");
    if(Terminal(saved->phase))return {saved->phase==Phase::Completed,""};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld())return stop("party_repair_actor_unavailable");
    for(const auto& write:state->pending)if(write.task.actor==actor)return stop("party_repair_transition_pending");
    // All nonrepeatable repair outcomes (including recalled intent) must pass
    // the shared receipt reconciler before another quote can be prepared.
    if(saved->phase==Phase::Queued) {
        const auto party=SavedPartyBlocker(*bot,*saved,Mask(Effect::Equipment));
        if(*party)return stop(party);
        TaskRequest r;r.task=*saved;r.expectedRevision=saved->revision;r.receipt=NewId();
        ++r.task.revision;r.task.updatedAtMs=NowMs();
        r.task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
        r.task.phase=r.task.context==saved->context?Phase::Preparing:Phase::Reconciling;
        return stop(SubmitTask(r).blocker);
    }
    const auto result=AdvanceCriticalPreparation(actor,id);
    return result?*result:stop("party_repair_preparation_unavailable");
}
