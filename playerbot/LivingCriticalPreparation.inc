// Shared prerequisite of the existing root, driven only by its due-queue turn.
// It does not acquire a second job, change domain data, or own another timer.
std::optional<LivingActivityCoordinator::ProfessionProgress> LivingActivityCoordinator::AdvanceCriticalPreparation(
    uint32_t actor,const std::string& id) {
    auto stop=[](const std::string& why){return ProfessionProgress{false,why};};
    const auto saved=ReadSavedTask(id);
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !saved || saved->actor!=actor ||
        state->executingTask!=id || saved->mode!=Mode::Active || !saved->accepted || saved->root!=id ||
        Terminal(saved->phase))return {};
    const bool repairing=RepairPrerequisiteStep(saved->checkpoint.step);
    if(!repairing && saved->phase!=Phase::Preparing && saved->phase!=Phase::Traveling)return {};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetMap())return {};
    for(const auto& write:state->pending)if(write.task.actor==actor)return stop("critical_preparation_transition_pending");
    for(const auto& row:state->operations)if(row.second.request.transition.task.actor==actor) {
        // Already committed atomic work wins; never repair between its effect
        // and acknowledgement. The domain dispatcher handles non-repair work.
        if(row.second.request.transition.task.id!=id || row.second.request.kind!="critical_equipment_repair")return {};
        return DispatchPendingItemService(actor,id);
    }
    if(DefersNativeSave(actor))return stop("native_save_pending");
    const auto current=ReadNativeContext(*bot,state->policyRevision,state->boot);
    auto advance=[&](Phase phase,const std::string& step) {
        TaskRequest r;r.task=*saved;r.expectedRevision=saved->revision;r.receipt=NewId();
        ++r.task.revision;r.task.phase=phase;r.task.context=current;r.task.updatedAtMs=NowMs();
        r.task.checkpoint.step=step;r.task.checkpoint.blocker.clear();r.task.retryAtMs=0;
        return stop(SubmitTask(r).blocker);
    };
    const auto resume=IsManagedGuildDelivery(*saved)?"guild_delivery_prepare":
        IsGuildProcurementTask(*saved)?"guild_procurement_prepare":IsRecipeLearningTask(*saved)?"recipe_prepare":"profession_prepare";
    if(repairing && saved->checkpoint.step=="maintenance_repair" &&
        (saved->phase==Phase::Verifying || saved->phase==Phase::Executing || saved->phase==Phase::Reconciling)) {
        auto found=state->repairReads.find(id);
        if(found==state->repairReads.end()) {
            for(auto it=state->repairReads.begin();it!=state->repairReads.end();) {
                const auto task=state->cache.find(it->first);
                if(!it->second.pending && (task==state->cache.end() || Terminal(task->second.phase) ||
                    !RepairPrerequisiteStep(task->second.checkpoint.step)))it=state->repairReads.erase(it);
                else ++it;
            }
            if(state->repairReads.size()>=64)return stop("critical_repair_receipt_queue_full");
            found=state->repairReads.emplace(id,State::RepairRead{}).first;
        }
        auto& read=found->second;
        if(read.pending)return stop("critical_repair_receipt_pending");
        if(read.revision!=saved->revision)read={};
        if(!read.complete) {
            if(read.retryAt>NowMs() || state->ioPending)return stop("critical_repair_receipt_pending");
            read.pending=state->ioPending=true;read.revision=saved->revision;const auto revision=saved->revision;
            const auto sql="SELECT o.state,o.evidence_code,(SELECT COUNT(*) FROM living_activity_operation u"
                " JOIN living_activity_task t ON t.task_id=u.task_id WHERE t.actor_guid="+std::to_string(actor)+
                " AND u.state IN ('intent','reconciling')) FROM living_activity_operation o WHERE o.task_id="+SqlValue(id)+
                " AND o.kind='critical_equipment_repair' AND o.task_revision<="+std::to_string(revision)+
                " ORDER BY o.task_revision DESC LIMIT 1";
            if(!CharacterDatabase.AsyncQuery([this,id,revision](QueryResult* rows) {
                state->ioPending=false;const auto found=state->repairReads.find(id);if(found==state->repairReads.end())return;
                auto& read=found->second;read.pending=false;if(read.revision!=revision)return;
                if(!rows || rows->GetFieldCount()!=3){read.retryAt=NowMs()+30000;return;}
                const auto* f=rows->Fetch();read.outcome=f[0].GetCppString();read.evidence=f[1].GetCppString();
                read.unresolved=f[2].GetUInt32();read.complete=true;
            },sql.c_str())) {state->ioPending=read.pending=false;read.retryAt=NowMs()+30000;}
            return stop("critical_repair_receipt_pending");
        }
        // An interrupted or uncertain charge cannot authorize another charge.
        if(read.unresolved)return stop("critical_repair_native_operation_requires_reconciliation");
        if(read.outcome!="rejected" && (read.outcome!="verified" || read.evidence!="native_repair_durability_and_payment_observed"))
            return stop("critical_repair_native_receipt_invalid");
        if(saved->retryAtMs>NowMs())return stop("critical_repair_native_retry_wait");
        if(!(current==saved->context))return advance(Phase::Reconciling,"maintenance_repair");
        // Rejected work retains escrow; release/requote it in Preparing.
        return advance(Phase::Preparing,"maintenance_repair_prepare");
    }
    if(!repairing && !(current==saved->context))return {}; // Domain restart validators run first.
    const auto safety=ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
    std::string pause=safety?NativeSafetyReason(safety):bot->GetMap()->IsDungeon()?"native_dungeon_commitment":
        bot->GetTradeData()?"native_trade_in_progress":LivingServiceExecution::Busy(bot)?LivingServiceExecution::Blocker(bot):"";
    if(pause.empty())pause=PartyAdmissionBlocker(NativePartyProtection(*bot),PartyAdmission::SavedExecutor,false);
    if(!pause.empty()) {
        if(!repairing)return {}; // The current owner performs its ordinary safety pause.
        if(saved->phase==Phase::Preparing || saved->phase==Phase::Traveling) {
            TaskRequest r;r.task=*saved;r.expectedRevision=saved->revision;r.receipt=NewId();
            ++r.task.revision;r.task.phase=Phase::Paused;r.task.updatedAtMs=NowMs();r.task.checkpoint.blocker=pause;
            return stop(SubmitTask(r).blocker);
        }
        return stop(pause);
    }
    if(repairing && !(current==saved->context))return advance(Phase::Reconciling,saved->checkpoint.step);
    if(saved->retryAtMs>NowMs())return stop(saved->checkpoint.blocker.empty()?"critical_repair_retry_wait":saved->checkpoint.blocker);
    if(repairing)if(const auto phase=RepairResumePhase(saved->phase))
        return advance(*phase,saved->checkpoint.step);
    UnsettledClaimBatch claims;std::string blocker;ResourceClaim money;
    if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker))return stop(blocker);
    for(const auto& c:claims.claims)if(c.location=="money") {
        if(!repairing)return {}; // Never spend money already committed to a purchase or postage.
        if(!money.id.empty() || c.state!="held")return stop("critical_repair_money_requires_reconciliation");
        money=c;
    }
    if(!repairing) {
        if(!HasNativeCriticalRepair(*bot))return {};
        return advance(Phase::Preparing,"maintenance_repair_prepare");
    }
    if(saved->checkpoint.step==ServiceStep(ServiceDestination::Repair)) {
        if(saved->phase==Phase::Preparing)return advance(Phase::Traveling,ServiceStep(ServiceDestination::Repair));
        if(saved->phase==Phase::Traveling) {
            const auto route=sPlayerbotOrganicEconomy.ReachSavedService(actor,id,saved->revision,ServiceDestination::Repair);
            Task next;
            if(CheckpointServiceTravel(*saved,route,NowMs(),"maintenance_repair_prepare",next)) {
                TaskRequest r;r.task=std::move(next);r.expectedRevision=saved->revision;r.receipt=NewId();
                return stop(SubmitTask(r).blocker);
            }
            return stop(route.blocker);
        }
    }
    if(saved->phase!=Phase::Preparing || saved->checkpoint.step!="maintenance_repair_prepare")
        return stop("critical_repair_preparation_requires_reconciliation");
    NativeRepairQuote quote;ResourceClaim held;
    const bool planned=PlanNativeCriticalRepair(*bot,*saved,quote,held,blocker);
    // Only a repair-owned step can contain this escrow: entry into the step
    // above required no pre-existing money claim. Always release stale quotes.
    const bool release=!money.id.empty() && (!planned || money.copper!=quote.copper);
    if(release || planned) {
        NativeCriticalRepair adapter(quote);
        const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"critical_repair_prepare");
        if(!grant.Permitted())return stop(grant.blocker);
        if(release || held.id.empty()) {
            ResourceClaim c=money;const auto expected=release?c.revision:0;
            if(release){++c.revision;c.state="released";}
            else {c={};c.id=NewId();c.task=id;c.actor=actor;c.location="money";c.state="held";c.copper=quote.copper;}
            ReservationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
            ++r.transition.task.revision;r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();
            r.authorization=grant.action;r.changes.push_back({c,expected});NativeRepairReservation reservation;
            return stop(SubmitResourceReservation(r,reservation).blocker);
        }
        OperationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
        ++r.transition.task.revision;r.transition.task.phase=Phase::Executing;r.transition.task.checkpoint.step="maintenance_repair";
        r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();r.authorization=grant.action;
        r.kind=adapter.OperationKind();r.effects=adapter.OperationEffects();r.persistence=adapter.PersistencePolicy();
        r.beforeState=EncodeNativeRepairQuote(quote);r.consumption.push_back({held,quote.copper});
        return stop(SubmitOperationIntent(r,adapter).blocker);
    }
    if(blocker=="critical_repair_travel_required")return advance(Phase::Traveling,ServiceStep(ServiceDestination::Repair));
    if(blocker=="critical_repair_not_required")return advance(Phase::Preparing,resume);
    return stop(blocker);
}
