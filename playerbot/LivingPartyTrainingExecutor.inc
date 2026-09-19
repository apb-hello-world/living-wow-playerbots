// Finite verified lessons on the existing queue. Admission occurs at a real
// trainer; route discovery stays with the existing party service travel.
std::optional<LivingActivity::PartyServiceBinding> LivingActivityCoordinator::PreparePartyTrainingService(
    uint32_t actor,const std::string& sourceKey,uint64_t trainer,std::string& blocker) {
    blocker="party_training_admission_pending";
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !ProfessionStoreReady() ||
        !state->resources.Protection().ready || !IsSourceKey(sourceKey) ||
        !sPlayerbotRendezvousManager.IsPartyFreeTime(actor))return {};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetGroup())return {};
    auto binding=[](const Task& task) {
        PartyServiceBinding b;b.root=task.id;b.actor=task.actor;b.acceptedRevision=task.revision;
        b.service=PartyServiceBinding::Service::Training;return b;
    };
    const Task* chosen=nullptr;const auto indexed=state->cachedByActor.find(actor);
    if(indexed!=state->cachedByActor.end())for(const auto& id:indexed->second) {
        const auto& task=state->cache.at(id);
        if(IsPartyTrainingTask(task) && task.accepted && task.mode==Mode::Active && !Terminal(task.phase) &&
            (!chosen || Before(task,*chosen)))chosen=&task;
    }
    if(chosen)return binding(*chosen);
    for(const auto& write:state->pending)if(write.task.actor==actor && IsPartyTrainingTask(write.task) &&
        !Terminal(write.task.phase))return binding(write.task);
    if(!trainer){blocker="party_training_service_arrival_required";return {};}
    PartyTrainingJob job;if(!PlanNativePartyTraining(*bot,ObjectGuid(trainer),job,blocker))return {};
    TaskRequest r;auto& task=r.task;task.id=task.root=SourceId("party_training",sourceKey);
    task.source="party_training";task.sourceKey=sourceKey;task.actor=actor;
    task.kind=Kind::PartyErrand;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.priority=Priority::Delivery;task.accepted=true;
    task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
    task.createdAtMs=task.updatedAtMs=NowMs();task.checkpoint.step="party_training_prepare";
    task.checkpoint.data=EncodePartyTrainingJob(job);r.receipt=NewId();
    const auto admitted=SubmitTask(r);blocker=admitted.blocker;
    if(admitted.code!=AdmissionCode::Pending && admitted.code!=AdmissionCode::Saved)return {};
    return binding(task);
}
LivingActivityCoordinator::ProfessionProgress LivingActivityCoordinator::AdvancePartyTraining(uint32_t actor,const std::string& id) {
    auto stop=[](const std::string& why){return ProfessionProgress{false,why};};
    const auto saved=ReadSavedTask(id);std::string blocker;
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !saved || saved->actor!=actor ||
        state->executingTask!=id || !IsPartyTrainingTask(*saved) || !ValidatePartyTrainingTask(*saved,blocker))
        return stop("party_training_saved_task_required");
    if(Terminal(saved->phase))return {saved->phase==Phase::Completed,""};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetMap())return stop("party_training_actor_unavailable");
    for(const auto& write:state->pending)if(write.task.actor==actor)return stop("party_training_transition_pending");
    for(const auto& row:state->operations)if(row.second.request.transition.task.actor==actor) {
        if(row.second.request.transition.task.id!=id || row.second.request.kind!="party_training_learn")
            return stop("party_training_other_operation_pending");
        TrainingLessonQuote quote;
        if(!DecodePartyTrainingQuote(row.second.request.beforeState,quote))return stop("party_training_intent_invalid");
        NativePartyTraining adapter(quote);
        const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"party_training_learn");
        if(!grant.Permitted())return stop(grant.blocker);
        return stop(DispatchSavedOperation(row.first,grant,adapter).admission.blocker);
    }
    if(DefersNativeSave(actor))return stop("native_save_pending");
    const auto current=ReadNativeContext(*bot,state->policyRevision,state->boot);
    auto advance=[&](Phase phase,const std::string& step,const std::string& why="") {
        TaskRequest r;r.task=*saved;r.expectedRevision=saved->revision;r.receipt=NewId();
        ++r.task.revision;r.task.phase=phase;r.task.context=current;r.task.updatedAtMs=NowMs();
        r.task.checkpoint.step=step;r.task.checkpoint.blocker=why;
        r.task.retryAtMs=phase==Phase::Deferred?NowMs()+300000:0;return stop(SubmitTask(r).blocker);
    };
    if(saved->checkpoint.step=="party_training_learn" &&
        (saved->phase==Phase::Verifying || saved->phase==Phase::Executing || saved->phase==Phase::Reconciling)) {
        auto found=state->trainingReads.find(id);
        if(found==state->trainingReads.end()) {
            for(auto it=state->trainingReads.begin();it!=state->trainingReads.end();) {
                const auto task=state->cache.find(it->first);
                if(!it->second.pending && (task==state->cache.end() || Terminal(task->second.phase)))it=state->trainingReads.erase(it);
                else ++it;
            }
            if(state->trainingReads.size()>=64)return stop("party_training_receipt_queue_full");
            found=state->trainingReads.emplace(id,State::RepairRead{}).first;
        }
        auto& read=found->second;if(read.pending)return stop("party_training_receipt_pending");
        if(read.revision!=saved->revision)read={};
        if(!read.complete) {
            if(read.retryAt>NowMs() || state->ioPending)return stop("party_training_receipt_pending");
            read.pending=state->ioPending=true;read.revision=saved->revision;const auto revision=saved->revision;
            const auto sql="SELECT o.state,o.evidence_code,(SELECT COUNT(*) FROM living_activity_operation u"
                " JOIN living_activity_task t ON t.task_id=u.task_id WHERE t.actor_guid="+std::to_string(actor)+
                " AND u.state IN ('intent','reconciling')) FROM living_activity_operation o WHERE o.task_id="+SqlValue(id)+
                " AND o.kind='party_training_learn' AND o.task_revision<="+std::to_string(revision)+
                " ORDER BY o.task_revision DESC LIMIT 1";
            if(!CharacterDatabase.AsyncQuery([this,id,revision](QueryResult* rows) {
                state->ioPending=false;const auto found=state->trainingReads.find(id);if(found==state->trainingReads.end())return;
                auto& read=found->second;read.pending=false;if(read.revision!=revision)return;
                if(!rows || rows->GetFieldCount()!=3){read.retryAt=NowMs()+30000;return;}
                const auto* f=rows->Fetch();read.outcome=f[0].GetCppString();read.evidence=f[1].GetCppString();
                read.unresolved=f[2].GetUInt32();read.complete=true;
            },sql.c_str())) {state->ioPending=read.pending=false;read.retryAt=NowMs()+30000;}
            return stop("party_training_receipt_pending");
        }
        if(read.unresolved)return stop("party_training_native_operation_requires_reconciliation");
        if(read.outcome!="rejected" && (read.outcome!="verified" || !VerifiedPartyTrainingEvidence(read.evidence)))
            return stop("party_training_native_receipt_invalid");
        if(saved->retryAtMs>NowMs())return stop("party_training_native_retry_wait");
        if(!(current==saved->context))return advance(Phase::Reconciling,"party_training_learn");
        return advance(Phase::Preparing,"party_training_prepare");
    }
    if(!(current==saved->context))return advance(Phase::Reconciling,saved->checkpoint.step);
    const auto safety=ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
    std::string pause=safety?NativeSafetyReason(safety):bot->GetMap()->IsDungeon()?"native_dungeon_commitment":
        LivingServiceExecution::Busy(bot)?LivingServiceExecution::Blocker(bot):"";
    if(pause.empty())pause=SavedPartyBlocker(*bot,*saved,Mask(Effect::Spell)|Mask(Effect::Social));
    if(!pause.empty()) {
        if(saved->phase==Phase::Preparing || saved->phase==Phase::Traveling)return advance(Phase::Paused,saved->checkpoint.step,pause);
        return stop(pause);
    }
    if(saved->retryAtMs>NowMs())return stop(saved->checkpoint.blocker);
    if(saved->phase==Phase::Queued || saved->phase==Phase::Reconciling)return advance(Phase::Preparing,"party_training_prepare");
    if(saved->phase==Phase::Paused || saved->phase==Phase::Deferred || saved->phase==Phase::WaitingExternal)
        return advance(Phase::Reconciling,saved->checkpoint.step);
    if(saved->checkpoint.step==ServiceStep(ServiceDestination::ClassTrainer)) {
        if(saved->phase==Phase::Preparing)return advance(Phase::Traveling,ServiceStep(ServiceDestination::ClassTrainer));
        const auto route=sPlayerbotOrganicEconomy.ReachSavedService(actor,id,saved->revision,ServiceDestination::ClassTrainer);
        Task next;
        if(CheckpointServiceTravel(*saved,route,NowMs(),"party_training_prepare",next)) {
            TaskRequest r;r.task=std::move(next);r.expectedRevision=saved->revision;r.receipt=NewId();return stop(SubmitTask(r).blocker);
        }
        return stop(route.blocker);
    }
    if(saved->phase!=Phase::Preparing)return stop("party_training_preparation_required");
    TrainingLessonQuote quote;
    if(!QuoteNativePartyTraining(*bot,*saved,quote,blocker)) {
        if(blocker=="party_training_class_trainer_required" || blocker=="training_interacting_trainer_required")
            return advance(Phase::Traveling,ServiceStep(ServiceDestination::ClassTrainer));
        return advance(Phase::Deferred,"party_training_prepare",blocker);
    }
    NativePartyTraining adapter(quote);
    const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"party_training_prepare");
    if(!grant.Permitted())return stop(grant.blocker);
    OperationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
    ++r.transition.task.revision;r.transition.task.phase=Phase::Executing;r.transition.task.checkpoint.step="party_training_learn";
    r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();r.authorization=grant.action;
    r.kind=adapter.OperationKind();r.effects=adapter.OperationEffects();r.persistence=adapter.PersistencePolicy();
    r.beforeState=EncodePartyTrainingQuote(quote);return stop(SubmitOperationIntent(r,adapter).blocker);
}
