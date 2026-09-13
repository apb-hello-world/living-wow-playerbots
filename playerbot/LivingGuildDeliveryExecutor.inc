// Compiled with LivingActivityCoordinator.cpp: uses its existing bounded due
// queue, persistence queue and private native-operation state, not a new loop.
bool LivingActivityCoordinator::OwnsGuildDelivery(uint32_t actor,const GuildDeliveryJob& job) const {
    if(!OnWorldThread() || !ValidGuildDeliveryJob(job) || !actor || (!job.incomingMail && actor!=job.donor))return false;
    const auto id=SourceId("guild_delivery",GuildDeliverySourceKey(job,actor));
    const auto saved=state->cache.find(id);
    if(saved!=state->cache.end() && IsManagedGuildDelivery(saved->second))return true;
    for(const auto& pending:state->pending)if((pending.task.id==id && IsManagedGuildDelivery(pending.task)) ||
        (pending.recipientTask.id==id && IsManagedGuildDelivery(pending.recipientTask)))return true;
    for(const auto& row:state->operations)if(row.second.dispatched && row.second.request.kind=="guild_mail_send") {
        GuildMailQuote q;
        if(DecodeGuildMailQuote(row.second.request.beforeState,q) && q.receiver==actor &&
            q.job.delivery==job.delivery && q.job.donor==job.donor)return true;
    }
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
    if(job.incomingMail) {
        ResourceClaim mail;
        if(!sGuildSupplies.ReadManagedMail(task,mail,why))return reject(AdmissionCode::ReconciliationRequired,why);
        task.checkpoint.step="guild_mail_prepare";
    } else {
        if(!sGuildSupplies.ReadManagedCarry(task,current,why))return reject(AdmissionCode::NotReady,why);
        if(current.deposited)return reject(AdmissionCode::ReconciliationRequired,"guild_delivery_prior_credit_requires_import");
    }
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
    if(const auto service=DispatchPendingItemService(actor,id))return stop(service->blocker);
    for(const auto& row:state->operations)if(row.second.request.transition.task.actor==actor) {
        if(row.second.request.transition.task.id!=id || (row.second.request.kind!="guild_bank_deposit" && row.second.request.kind!="guild_mail_send"))
            return stop("guild_delivery_other_operation_pending");
        if(!row.second.ready || row.second.dispatched)return stop("guild_delivery_native_receipt_pending");
        if(row.second.request.kind=="guild_mail_send") {
            GuildMailQuote quote;
            if(!DecodeGuildMailQuote(row.second.request.beforeState,quote))return stop("guild_mail_intent_requires_reconciliation");
            NativeGuildMail adapter(quote);
            const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"guild_delivery_send");
            if(!grant.Permitted())return stop(grant.blocker);
            return stop(DispatchSavedOperation(row.first,grant,adapter).admission.blocker);
        }
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
    const bool serviceResult=saved->checkpoint.step=="guild_bank_deposit" || saved->checkpoint.step=="guild_mail_send" || saved->checkpoint.step=="profession_mail_collect" ||
        saved->checkpoint.step=="profession_capacity_sale" || saved->checkpoint.step=="profession_capacity_bank";
    if((saved->phase==Phase::Verifying || saved->phase==Phase::Reconciling) && serviceResult) {
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
            const std::string query="SELECT IF(d.carrier_guid="+std::to_string(actor)+" AND d.mail_id="+
                std::to_string(job.incomingMail)+",d.phase,'handed_off'),d.deposited_quantity,(SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id="+
                SqlValue(id)+" AND o.state='rejected' AND o.evidence_code<>'native_guild_mail_not_committed'),(SELECT COALESCE(MAX(o.updated_at_ms),0) FROM living_activity_operation o WHERE o.task_id="+
                SqlValue(id)+" AND o.state='rejected' AND o.evidence_code<>'native_guild_mail_not_committed'),(SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
                " WHERE t.actor_guid="+std::to_string(actor)+" AND o.state IN ('intent','reconciling')) FROM guild_society_supply_delivery d WHERE d.delivery_id="+
                std::to_string(job.delivery)+" AND d.guild_id="+std::to_string(job.guild)+" AND d.goal_id="+SqlValue(job.goal)+
                " AND d.donor_guid="+std::to_string(job.donor)+
                " AND d.item_entry="+std::to_string(job.entry)+" AND d.quantity="+std::to_string(job.quantity);
            // One bounded operation snapshot only on verification/restart,
            // never a per-tick database scan or inferred resend permission.
            auto snapshot=query;
            const auto marker=snapshot.find(" FROM guild_society_supply_delivery d");
            snapshot.insert(marker,",COALESCE((SELECT "+GuildMailRollbackProjection()+
                " FROM living_activity_operation o WHERE o.task_id="+SqlValue(id)+" AND o.kind='guild_mail_send'"
                " AND (o.state='intent' OR (o.state='reconciling' AND o.evidence_code='native_save_capture_requires_reconciliation')) LIMIT 1),'')");
            if(!CharacterDatabase.AsyncQuery([this,id,revision](QueryResult* rows) {
                state->ioPending=false;const auto found=state->guildDeliveryReads.find(id);
                if(found==state->guildDeliveryReads.end())return;
                auto& read=found->second;read.pending=false;if(read.revision!=revision)return;
                if(!rows || rows->GetFieldCount()!=6){read.blocker="guild_delivery_native_record_missing";read.retryAt=NowMs()+30000;return;}
                auto* f=rows->Fetch();read.phase=f[0].GetCppString();read.deposited=f[1].GetUInt32();read.failures=f[2].GetUInt32();
                read.lastRejected=f[3].GetUInt64();read.unresolved=f[4].GetUInt32();read.sendRollback=f[5].GetCppString();read.complete=true;
            },snapshot.c_str())) {state->ioPending=false;read.pending=false;read.retryAt=NowMs()+30000;
                read.blocker="guild_delivery_database_unavailable";return stop(read.blocker);}
            return stop("guild_delivery_receipt_read_pending");
        }
        if(read.unresolved) {
            GuildMailRollback rollback;
            if(read.unresolved!=1 || !DecodeGuildMailRollback(read.sendRollback,rollback))
                return stop("guild_delivery_operation_requires_reconciliation");
            if(!saved->context.boot.empty())return stop("guild_mail_rollback_requires_native_reload");
            if(ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || bot->GetMap()->IsDungeon() ||
                bot->GetTradeData() || LivingServiceExecution::Busy(bot))return stop("guild_mail_rollback_safety_wait");
            GuildDepositQuote carry;if(!sGuildSupplies.ReadManagedCarry(*saved,carry,blocker))return stop(blocker);
            const auto& q=rollback.quote;auto* item=bot->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,q.item));
            if(!item || item->GetPos()!=q.position || bot->GetItemCount(q.job.entry,false)!=q.bagBefore ||
                bot->GetItemCount(q.job.entry,true)!=q.totalBefore)return stop("guild_mail_rollback_possessions_changed");
            UnsettledClaimBatch claims;if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker))return stop(blocker);
            std::vector<ClaimConsumption> uses;for(const auto& claim:claims.claims)uses.push_back({claim,claim.copper?claim.copper:claim.quantity});
            NativeItemStack stack{actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),
                item->GetContainer()?item->GetContainer()->GetGUIDLow():0,item->GetSlot()};
            GuildMailRollbackWrite recovered;const auto receipt=SourceId("guild_send_rollback",rollback.interrupted.id);
            if(!PrepareGuildMailRollback(*saved,current,rollback,uses,stack,bot->GetMoney(),NowMs(),receipt,recovered,blocker))return stop(blocker);
            if(state->pending.size()>=state->batch || state->transitionCount+state->pending.size()>=200000)return stop("guild_delivery_recovery_backpressure");
            State::Pending write;write.task=std::move(recovered.task);write.plan=std::move(recovered.journal);
            write.admissionReceipt=receipt;state->pending.push_back(std::move(write));state->nextWork=0;
            return stop("guild_mail_rollback_native_proof_pending");
        }
        if((read.phase=="completed" && read.deposited==job.quantity) ||
            (read.phase=="handed_off" && saved->checkpoint.step=="guild_mail_send")) {
            UnsettledClaimBatch claims;GuildDeliverySettlement settled;
            if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker))return stop(blocker);
            const auto balances=NativeClaimBalances(*bot,claims.claims,false);
            const auto receipt=SourceId("guild_delivery_settlement",id);
            const bool verified=read.phase=="handed_off"?
                PrepareGuildMailSenderSettlement(*saved,current,claims,NowMs(),receipt,settled,blocker,balances):
                PrepareGuildDeliverySettlement(*saved,current,claims,NowMs(),receipt,settled,blocker,balances);
            if(!verified)return stop(blocker);
            if(state->pending.size()>=state->batch || state->transitionCount+state->pending.size()>=200000)
                return stop("guild_delivery_settlement_backpressure");
            State::Pending write;write.task=std::move(settled.task);write.plan=std::move(settled.plan);
            write.claims=std::move(settled.claims);
            write.admissionReceipt=SourceId("guild_delivery_settlement",id);
            state->pending.push_back(std::move(write));state->nextWork=0;return stop("guild_delivery_settlement_pending");
        }
        if((read.phase!="carried" && !(read.phase=="mailed" && job.incomingMail && !read.deposited)) || read.deposited>=job.quantity)
            return stop("guild_delivery_native_state_requires_reconciliation");
        const auto retry=GuildDepositRetryAt(read.failures,read.lastRejected);
        if(retry>NowMs()) {
            TaskRequest request;request.task=*saved;request.expectedRevision=saved->revision;request.receipt=NewId();
            ++request.task.revision;request.task.phase=Phase::Deferred;request.task.context=current;request.task.updatedAtMs=NowMs();
            request.task.retryAtMs=retry;request.task.checkpoint.step="guild_delivery_prepare";
            request.task.checkpoint.blocker=read.failures<2?"guild_delivery_native_retry_wait":"guild_delivery_repeated_native_rejection";
            return stop(SubmitTask(request).blocker);
        }
        if(saved->retryAtMs>NowMs())return stop("guild_delivery_native_retry_wait");
        return advance(saved->context==current?Phase::Preparing:Phase::Reconciling,"guild_delivery_prepare");
    }
    if(ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || bot->GetMap()->IsDungeon() ||
        LivingServiceExecution::Busy(bot) || bot->GetTradeData())return stop("guild_delivery_safety_pause");
    if(saved->retryAtMs>NowMs())return stop("guild_delivery_retry_wait");
    if(!(saved->context==current))return advance(Phase::Reconciling);
    if(saved->phase==Phase::Queued || saved->phase==Phase::Reconciling || saved->phase==Phase::Paused ||
        saved->phase==Phase::Deferred || saved->phase==Phase::WaitingExternal)return advance(GuildDeliveryPreparationPhase(saved->phase));
    ServiceDestination preparation;
    if(ParseServiceStep(saved->checkpoint.step,preparation) && preparation!=ServiceDestination::GuildBank && preparation!=ServiceDestination::GuildMailbox)
        return stop(AdvanceItemPreparation(actor,id,ProfessionStep::Collect,{job.entry,job.quantity}).blocker);
    if(saved->phase==Phase::Traveling) {
        const auto destination=saved->checkpoint.step==ServiceStep(ServiceDestination::GuildMailbox)?ServiceDestination::GuildMailbox:ServiceDestination::GuildBank;
        const auto travel=sPlayerbotOrganicEconomy.ReachSavedService(actor,id,saved->revision,destination);
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
    if(job.incomingMail) {
        ResourceClaim incoming;
        if(sGuildSupplies.ReadManagedMail(*saved,incoming,blocker)) {
            UnsettledClaimBatch claims;
            if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker))return stop(blocker);
            for(const auto& claim:claims.claims)if(claim.location=="mail") {
                incoming.id=claim.id;incoming.revision=claim.revision;
                if(!SameResourceClaim(incoming,claim))return stop("guild_delivery_other_mail_claim_requires_reconciliation");
                return stop(AdvanceItemPreparation(actor,id,ProfessionStep::Collect,{job.entry,job.quantity}).blocker);
            }
            const auto grant=AcquireSavedTask(id,saved->revision,Mask(Effect::Inventory),60000,"guild_mail_reservation");
            if(!grant.Permitted())return stop(grant.blocker);
            incoming.id=NewId();ReservationRequest request;
            request.transition.task=*saved;request.transition.expectedRevision=saved->revision;
            ++request.transition.task.revision;request.transition.task.updatedAtMs=NowMs();request.transition.receipt=NewId();
            request.authorization=grant.action;request.changes.push_back({incoming,0});NativeGuildMailReservation adapter;
            return stop(SubmitResourceReservation(request,adapter).blocker);
        }
        if(blocker!="guild_delivery_mail_already_collected")return stop(blocker);
    }
    GuildDepositQuote quote;ResourceClaim held;
    GuildDepositQuote carry;
    const bool direct=sGuildSupplies.ReadManagedDeposit(*saved,carry,blocker);
    if(!direct && blocker=="guild_delivery_deposit_permission_revoked") {
        GuildMailQuote parcel;std::vector<ClaimConsumption> uses;
        if(!PlanNativeGuildMail(*bot,*saved,parcel,uses,blocker)) {
            if(blocker=="guild_mailbox_send_travel_required" || (blocker=="guild_mail_safety_or_arrival_wait" && !bot->IsStopped()))
                return advance(Phase::Traveling,ServiceStep(ServiceDestination::GuildMailbox));
            if(blocker=="guild_mail_no_deposit_recipient" || blocker=="guild_mail_insufficient_unreserved_postage" ||
                blocker=="guild_mail_exact_whole_stack_required") {
                TaskRequest wait;wait.task=*saved;wait.expectedRevision=saved->revision;wait.receipt=NewId();
                ++wait.task.revision;wait.task.phase=Phase::WaitingExternal;wait.task.updatedAtMs=NowMs();
                wait.task.retryAtMs=NowMs()+30000;wait.task.checkpoint.blocker=blocker;
                return stop(SubmitTask(wait).blocker);
            }
            return stop(blocker);
        }
        NativeGuildMail adapter(parcel);
        const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"guild_mail_prepare");
        if(!grant.Permitted())return stop(grant.blocker);
        if(uses.size()!=2) {
            ReservationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
            ++r.transition.task.revision;r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();r.authorization=grant.action;
            bool item=false,money=false;for(const auto& c:uses){item|=c.before.location=="bags";money|=c.before.location=="money";}
            if(!item){ResourceClaim c;c.id=NewId();c.task=id;c.actor=actor;c.itemGuid=parcel.item;c.itemEntry=job.entry;
                c.quantity=job.quantity;c.location="bags";c.state="held";r.changes.push_back({c,0});}
            if(!money){ResourceClaim c;c.id=NewId();c.task=id;c.actor=actor;c.copper=parcel.postage;c.location="money";c.state="held";r.changes.push_back({c,0});}
            NativeGuildSendReservation reservation;return stop(SubmitResourceReservation(r,reservation).blocker);
        }
        OperationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;++r.transition.task.revision;
        r.transition.task.phase=Phase::Executing;r.transition.task.updatedAtMs=NowMs();r.transition.task.checkpoint.step="guild_mail_send";
        r.transition.receipt=NewId();r.authorization=grant.action;r.kind=adapter.OperationKind();r.effects=adapter.OperationEffects();
        r.persistence=adapter.PersistencePolicy();r.beforeState=EncodeGuildMailQuote(parcel);r.consumption=uses;
        return stop(SubmitOperationIntent(r,adapter).blocker);
    }
    if(!direct)return stop(blocker);
    UnsettledClaimBatch claims;
    if(!ReadTaskClaims(actor,id,saved->revision,claims,blocker))return stop(blocker);
    for(auto c:claims.claims)if(c.location=="money" && c.state=="held" && c.copper==30) {
        const auto grant=AcquireSavedTask(id,saved->revision,Mask(Effect::Money),60000,"guild_mail_release_postage");
        if(!grant.Permitted())return stop(grant.blocker);
        ReservationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;++r.transition.task.revision;
        r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();r.authorization=grant.action;
        const auto expected=c.revision++;c.state="released";r.changes.push_back({c,expected});
        NativeGuildSendReservation reservation;return stop(SubmitResourceReservation(r,reservation).blocker);
    }
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
