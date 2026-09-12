// Compiled with LivingActivityCoordinator.cpp: uses its existing bounded due
// queue, persistence queue and private native-operation state, not a new loop.
bool LivingActivityCoordinator::OwnsGuildDelivery(uint32_t actor,const GuildDeliveryJob& job) const {
    if(!OnWorldThread() || !ValidGuildDeliveryJob(job) || !actor || (!job.incomingMail && actor!=job.donor))return false;
    const auto id=SourceId("guild_delivery",GuildDeliverySourceKey(job,actor));
    const auto saved=state->cache.find(id);
    if(saved!=state->cache.end() && IsManagedGuildDelivery(saved->second))return true;
    for(const auto& pending:state->pending)if(pending.task.id==id && IsManagedGuildDelivery(pending.task))return true;
    return false;
}
AdmissionResult LivingActivityCoordinator::AdmitGuildDelivery(uint32_t actor,const GuildDeliveryJob& job) {
    AdmissionResult result;
    auto reject=[&](AdmissionCode code,const std::string& why){result.code=code;result.blocker=why;return result;};
    if(!OnWorldThread() || !EffectEnforcementEnabled())return reject(AdmissionCode::Disabled,"execution_disabled");
    if(!ValidGuildDeliveryJob(job) || !actor || job.money || (!job.incomingMail && actor!=job.donor))
        return reject(AdmissionCode::InvalidRequest,"guild_delivery_item_leg_required");
    result.task=SourceId("guild_delivery",GuildDeliverySourceKey(job,actor));
    const auto saved=ReadSavedTask(result.task);
    if(saved) {
        result.revision=saved->revision;
        return reject(saved->checkpoint.data==EncodeGuildDeliveryJob(job)?AdmissionCode::Saved:AdmissionCode::ConflictingWrite,
            saved->checkpoint.data==EncodeGuildDeliveryJob(job)?"saved":"guild_delivery_intent_changed");
    }
    for(const auto& pending:state->pending)if(pending.task.id==result.task)
        return reject(pending.task.checkpoint.data==EncodeGuildDeliveryJob(job)?AdmissionCode::Pending:AdmissionCode::ConflictingWrite,
            "guild_delivery_admission_pending");
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld())return reject(AdmissionCode::StaleContext,"guild_delivery_actor_unavailable");
    TaskRequest request;auto& task=request.task;
    task.id=task.root=result.task;task.source="guild_delivery";task.sourceKey=GuildDeliverySourceKey(job,actor);
    task.actor=actor;task.kind=Kind::GuildDelivery;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.priority=Priority::Delivery;task.accepted=true;task.createdAtMs=task.updatedAtMs=NowMs();
    task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
    task.checkpoint.data=EncodeGuildDeliveryJob(job);task.checkpoint.step="guild_delivery_prepare";
    GuildDepositQuote current;std::string why;
    if(!sGuildSupplies.ReadManagedDeposit(task,current,why))return reject(AdmissionCode::NotReady,why);
    if(current.deposited)return reject(AdmissionCode::ReconciliationRequired,"guild_delivery_prior_credit_requires_import");
    request.receipt=SourceId("guild_delivery_admission",task.id);return SubmitTask(request);
}
LivingActivityCoordinator::ProfessionProgress LivingActivityCoordinator::AdvanceGuildDelivery(uint32_t actor,const std::string& id) {
    ProfessionProgress result;auto stop=[&](const std::string& why){result.blocker=why;return result;};
    if(!OnWorldThread() || !EffectEnforcementEnabled())return stop("execution_disabled");
    const auto saved=ReadSavedTask(id);GuildDeliveryJob job;std::string blocker;
    if(!saved || saved->actor!=actor || !IsManagedGuildDelivery(*saved) || !ValidateGuildDeliveryTask(*saved,blocker) ||
        !DecodeGuildDeliveryJob(saved->checkpoint.data,job,blocker))return stop("guild_delivery_saved_task_unavailable");
    if(saved->phase==Phase::Completed){result.completed=true;return result;}
    if(Terminal(saved->phase))return stop("guild_delivery_terminal");
    if(state->executingTask!=id) {
        const auto prior=state->executionBlockers.find(id);
        return stop(prior==state->executionBlockers.end()?"guild_delivery_queued":prior->second);
    }
    for(const auto& pending:state->pending)if(pending.task.actor==actor)return stop("guild_delivery_transition_pending");
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetMap())return stop("guild_delivery_actor_unavailable");
    for(const auto& row:state->operations)if(row.second.request.transition.task.actor==actor) {
        if(row.second.request.transition.task.id!=id || row.second.request.kind!="guild_bank_deposit")
            return stop("guild_delivery_other_operation_pending");
        if(!row.second.ready || row.second.dispatched)return stop("guild_delivery_native_receipt_pending");
        GuildDepositQuote quote;
        if(!DecodeGuildDepositQuote(row.second.request.beforeState,quote))return stop("guild_delivery_intent_requires_reconciliation");
        NativeGuildDeposit adapter(quote);
        const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"guild_delivery_deposit");
        if(!grant.Permitted())return stop(grant.blocker);
        return stop(DispatchSavedOperation(row.first,grant,adapter).admission.blocker);
    }
    if(saved->phase==Phase::Executing)return stop("guild_delivery_interrupted_operation_requires_reconciliation");
    if(DefersNativeSave(actor))return stop("native_save_pending");
    const auto current=ReadNativeContext(*bot,state->policyRevision,state->boot);
    auto advance=[&](Phase phase,const char* step=nullptr) {
        TaskRequest request;request.task=*saved;request.expectedRevision=saved->revision;request.receipt=NewId();
        ++request.task.revision;request.task.phase=phase;request.task.context=current;request.task.updatedAtMs=NowMs();
        request.task.checkpoint.blocker.clear();request.task.retryAtMs=0;if(step)request.task.checkpoint.step=step;
        return stop(SubmitTask(request).blocker);
    };
    if((saved->phase==Phase::Verifying || saved->phase==Phase::Reconciling) && saved->checkpoint.step=="guild_bank_deposit") {
        auto found=state->guildDeliveryReads.find(id);
        if(found==state->guildDeliveryReads.end()) {
            for(auto it=state->guildDeliveryReads.begin();it!=state->guildDeliveryReads.end();) {
                const auto task=state->cache.find(it->first);
                if(!it->second.pending && (task==state->cache.end() || Terminal(task->second.phase)))it=state->guildDeliveryReads.erase(it);
                else ++it;
            }
            if(state->guildDeliveryReads.size()>=64)return stop("guild_delivery_receipt_queue_full");
            found=state->guildDeliveryReads.emplace(id,State::GuildDeliveryRead{}).first;
        }
        auto& read=found->second;
        if(read.pending)return stop("guild_delivery_receipt_read_pending");
        if(read.revision!=saved->revision)read={};
        if(!read.complete) {
            if(read.retryAt>NowMs())return stop(read.blocker);
            if(state->ioPending)return stop("guild_delivery_database_queue_wait");
            read.revision=saved->revision;read.pending=true;state->ioPending=true;const auto revision=saved->revision;
            const std::string query="SELECT d.phase,d.deposited_quantity,(SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id="+
                SqlValue(id)+" AND o.state='rejected') FROM guild_society_supply_delivery d WHERE d.delivery_id="+
                std::to_string(job.delivery)+" AND d.guild_id="+std::to_string(job.guild)+" AND d.goal_id="+SqlValue(job.goal)+
                " AND d.carrier_guid="+std::to_string(actor)+" AND d.donor_guid="+std::to_string(job.donor)+
                " AND d.item_entry="+std::to_string(job.entry)+" AND d.quantity="+std::to_string(job.quantity)+
                " AND d.mail_id="+std::to_string(job.incomingMail);
            if(!CharacterDatabase.AsyncQuery([this,id,revision](QueryResult* rows) {
                state->ioPending=false;const auto found=state->guildDeliveryReads.find(id);
                if(found==state->guildDeliveryReads.end())return;
                auto& read=found->second;read.pending=false;if(read.revision!=revision)return;
                if(!rows || rows->GetFieldCount()!=3){read.blocker="guild_delivery_native_record_missing";read.retryAt=NowMs()+30000;return;}
                auto* f=rows->Fetch();read.phase=f[0].GetCppString();read.deposited=f[1].GetUInt32();read.failures=f[2].GetUInt32();read.complete=true;
            },query.c_str())) {state->ioPending=false;read.pending=false;read.retryAt=NowMs()+30000;
                read.blocker="guild_delivery_database_unavailable";return stop(read.blocker);}
            return stop("guild_delivery_receipt_read_pending");
        }
        if(read.phase=="completed" && read.deposited==job.quantity) {
            UnsettledClaimBatch claims;GuildDeliverySettlement settled;
            if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker) ||
                !PrepareGuildDeliverySettlement(*saved,current,claims,NowMs(),SourceId("guild_delivery_settlement",id),settled,blocker))return stop(blocker);
            if(state->pending.size()>=state->batch || state->transitionCount+state->pending.size()>=200000)
                return stop("guild_delivery_settlement_backpressure");
            State::Pending write;write.task=std::move(settled.task);write.plan=std::move(settled.plan);
            write.admissionReceipt=SourceId("guild_delivery_settlement",id);
            state->pending.push_back(std::move(write));state->nextWork=0;return stop("guild_delivery_settlement_pending");
        }
        if(read.phase!="carried" || read.deposited>=job.quantity)return stop("guild_delivery_native_state_requires_reconciliation");
        if(read.failures>=2)return stop("guild_delivery_repeated_native_rejection");
        if(saved->retryAtMs>NowMs())return stop("guild_delivery_native_retry_wait");
        return advance(saved->context==current?Phase::Preparing:Phase::Reconciling,"guild_delivery_prepare");
    }
    if(ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || bot->GetMap()->IsDungeon() ||
        LivingServiceExecution::Busy(bot) || bot->GetTradeData())return stop("guild_delivery_safety_pause");
    if(saved->retryAtMs>NowMs())return stop("guild_delivery_retry_wait");
    if(!(saved->context==current))return advance(Phase::Reconciling);
    if(saved->phase==Phase::Queued || saved->phase==Phase::Reconciling || saved->phase==Phase::Paused ||
        saved->phase==Phase::Deferred || saved->phase==Phase::WaitingExternal)return advance(Phase::Preparing);
    if(saved->phase==Phase::Traveling) {
        const auto travel=sPlayerbotOrganicEconomy.ReachSavedService(actor,id,saved->revision,ServiceDestination::GuildBank);
        const bool pause=travel.blocker=="recipe_service_safety_pause";
        if(travel.arrived || travel.retryAtMs || pause || (travel.activeElapsedMs>=saved->checkpoint.activeElapsedMs &&
            travel.activeElapsedMs-saved->checkpoint.activeElapsedMs>=30000)) {
            TaskRequest request;request.task=*saved;request.expectedRevision=saved->revision;request.receipt=NewId();
            ++request.task.revision;request.task.updatedAtMs=NowMs();
            request.task.phase=travel.arrived?Phase::Preparing:travel.retryAtMs?Phase::Deferred:pause?Phase::Paused:Phase::Traveling;
            request.task.retryAtMs=travel.retryAtMs;request.task.checkpoint.activeElapsedMs=std::max(saved->checkpoint.activeElapsedMs,travel.activeElapsedMs);
            request.task.checkpoint.blocker=(travel.retryAtMs || pause)?travel.blocker:"";
            if(travel.arrived){request.task.checkpoint.step="guild_delivery_prepare";request.task.checkpoint.lastProgressAtMs=NowMs();}
            return stop(SubmitTask(request).blocker);
        }
        return stop(travel.blocker);
    }
    if(saved->phase!=Phase::Preparing)return stop("guild_delivery_preparation_required");
    GuildDepositQuote quote;ResourceClaim held;
    if(!PlanNativeGuildDeposit(*bot,*saved,quote,held,blocker)) {
        if(blocker=="guild_bank_travel_required" || (blocker=="guild_delivery_safety_or_arrival_wait" && !bot->IsStopped()))
            return advance(Phase::Traveling,ServiceStep(ServiceDestination::GuildBank));
        return stop(blocker);
    }
    NativeGuildDeposit adapter(quote);
    const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"guild_delivery_preparation");
    if(!grant.Permitted())return stop(grant.blocker);
    if(held.id.empty()) {
        ResourceClaim claim;claim.id=NewId();claim.task=id;claim.actor=actor;claim.itemGuid=quote.item;claim.itemEntry=job.entry;
        claim.quantity=quote.amount;claim.state="held";claim.location="bags";
        ReservationRequest request;request.transition.task=*saved;request.transition.expectedRevision=saved->revision;
        ++request.transition.task.revision;request.transition.task.updatedAtMs=NowMs();request.transition.receipt=NewId();
        request.authorization=grant.action;request.changes.push_back({claim,0});NativeGuildDeliveryReservation reservation;
        return stop(SubmitResourceReservation(request,reservation).blocker);
    }
    OperationRequest request;request.transition.task=*saved;request.transition.expectedRevision=saved->revision;
    ++request.transition.task.revision;request.transition.task.phase=Phase::Executing;request.transition.task.updatedAtMs=NowMs();
    request.transition.task.checkpoint.step="guild_bank_deposit";request.transition.receipt=NewId();request.authorization=grant.action;
    request.kind=adapter.OperationKind();request.effects=adapter.OperationEffects();request.persistence=adapter.PersistencePolicy();
    request.beforeState=EncodeGuildDepositQuote(quote);request.consumption.push_back({held,quote.amount});
    return stop(SubmitOperationIntent(request,adapter).blocker);
}
