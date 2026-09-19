// Finite accepted inventory, executed by the existing shared due queue.
bool LivingActivityCoordinator::HasPendingPartyAuctionService(uint32_t actor) const {
    if(!OnWorldThread() || !EffectEnforcementEnabled())return false;
    const auto indexed=state->cachedByActor.find(actor);
    if(indexed!=state->cachedByActor.end())for(const auto& id:indexed->second) {
        const auto& task=state->cache.at(id);
        if(IsPartyAuctionTask(task) && task.accepted && task.mode==Mode::Active && !Terminal(task.phase))return true;
    }
    for(const auto& write:state->pending)if(write.task.actor==actor && IsPartyAuctionTask(write.task) &&
        write.task.accepted && write.task.mode==Mode::Active && !Terminal(write.task.phase))return true;
    return false;
}
std::optional<LivingActivity::PartyServiceBinding> LivingActivityCoordinator::PreparePartyAuctionService(
    uint32_t actor,const std::string& sourceKey,std::string& blocker) {
    blocker="party_auction_admission_pending";
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !ProfessionStoreReady() ||
        !state->resources.Protection().ready || !IsSourceKey(sourceKey) ||
        !sPlayerbotRendezvousManager.IsPartyFreeTime(actor))return {};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetGroup())return {};
    auto binding=[](const Task& task) {
        PartyServiceBinding value;value.root=task.id;value.actor=task.actor;
        value.acceptedRevision=task.revision;value.service=PartyServiceBinding::Service::Auction;return value;
    };
    const Task* chosen=nullptr;
    const auto indexed=state->cachedByActor.find(actor);
    if(indexed!=state->cachedByActor.end())for(const auto& id:indexed->second) {
        const auto& task=state->cache.at(id);
        if(IsPartyAuctionTask(task) && task.accepted && task.mode==Mode::Active && !Terminal(task.phase) &&
            (!chosen || Before(task,*chosen)))chosen=&task;
    }
    if(chosen)return binding(*chosen);
    for(const auto& write:state->pending)if(write.task.actor==actor && IsPartyAuctionTask(write.task) &&
        !Terminal(write.task.phase))return binding(write.task);
    PartyAuctionJob job;if(!PlanNativePartyAuctionBatch(*bot,job,blocker))return {};
    TaskRequest request;auto& task=request.task;
    task.id=task.root=SourceId("party_auction",sourceKey);task.source="party_auction";task.sourceKey=sourceKey;
    task.actor=actor;task.kind=Kind::PartyErrand;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.priority=Priority::Delivery;task.accepted=true;
    task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
    task.createdAtMs=task.updatedAtMs=NowMs();task.checkpoint.step="party_auction_prepare";
    task.checkpoint.data=EncodePartyAuctionJob(job);request.receipt=NewId();
    const auto admitted=SubmitTask(request);blocker=admitted.blocker;
    if(admitted.code!=AdmissionCode::Pending && admitted.code!=AdmissionCode::Saved)return {};
    return binding(task);
}
LivingActivityCoordinator::ProfessionProgress LivingActivityCoordinator::AdvancePartyAuction(uint32_t actor,const std::string& id) {
    auto stop=[](const std::string& why){return ProfessionProgress{false,why};};
    const auto saved=ReadSavedTask(id);std::string blocker;PartyAuctionJob job;
    if(!OnWorldThread() || !EffectEnforcementEnabled() || !saved || saved->actor!=actor ||
        state->executingTask!=id || !IsPartyAuctionTask(*saved) || !ValidatePartyAuctionTask(*saved,blocker) ||
        !DecodePartyAuctionJob(saved->checkpoint.data,job))return stop("party_auction_saved_task_required");
    if(Terminal(saved->phase))return {saved->phase==Phase::Completed,""};
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !bot->GetMap())return stop("party_auction_actor_unavailable");
    for(const auto& write:state->pending)if(write.task.actor==actor)return stop("party_auction_transition_pending");
    for(const auto& row:state->operations)if(row.second.request.transition.task.actor==actor) {
        if(row.second.request.transition.task.id!=id || row.second.request.kind!="party_auction_post")
            return stop("party_auction_other_operation_pending");
        const auto result=DispatchPendingItemService(actor,id);
        return result?*result:stop("party_auction_operation_requires_reconciliation");
    }
    if(DefersNativeSave(actor))return stop("native_save_pending");
    const auto current=ReadNativeContext(*bot,state->policyRevision,state->boot);
    auto advance=[&](Phase phase,const std::string& step,const std::string& why="") {
        TaskRequest r;r.task=*saved;r.expectedRevision=saved->revision;r.receipt=NewId();
        ++r.task.revision;r.task.phase=phase;r.task.context=current;r.task.updatedAtMs=NowMs();
        r.task.checkpoint.step=step;r.task.checkpoint.blocker=why;
        r.task.retryAtMs=phase==Phase::Deferred?NowMs()+300000:0;
        return stop(SubmitTask(r).blocker);
    };
    if(saved->checkpoint.step=="party_auction_post" &&
        (saved->phase==Phase::Verifying || saved->phase==Phase::Executing || saved->phase==Phase::Reconciling)) {
        auto found=state->auctionPostReads.find(id);
        if(found==state->auctionPostReads.end()) {
            for(auto it=state->auctionPostReads.begin();it!=state->auctionPostReads.end();) {
                const auto task=state->cache.find(it->first);
                if(!it->second.pending && (task==state->cache.end() || Terminal(task->second.phase)))it=state->auctionPostReads.erase(it);
                else ++it;
            }
            if(state->auctionPostReads.size()>=64)return stop("party_auction_receipt_queue_full");
            found=state->auctionPostReads.emplace(id,State::RepairRead{}).first;
        }
        auto& read=found->second;if(read.pending)return stop("party_auction_receipt_pending");
        if(read.revision!=saved->revision)read={};
        if(!read.complete) {
            if(read.retryAt>NowMs() || state->ioPending)return stop("party_auction_receipt_pending");
            read.pending=state->ioPending=true;read.revision=saved->revision;const auto revision=saved->revision;
            const auto sql="SELECT o.state,o.evidence_code,(SELECT COUNT(*) FROM living_activity_operation u"
                " JOIN living_activity_task t ON t.task_id=u.task_id WHERE t.actor_guid="+std::to_string(actor)+
                " AND u.state IN ('intent','reconciling')) FROM living_activity_operation o WHERE o.task_id="+SqlValue(id)+
                " AND o.kind='party_auction_post' AND o.task_revision<="+std::to_string(revision)+
                " ORDER BY o.task_revision DESC LIMIT 1";
            if(!CharacterDatabase.AsyncQuery([this,id,revision](QueryResult* rows) {
                state->ioPending=false;const auto found=state->auctionPostReads.find(id);if(found==state->auctionPostReads.end())return;
                auto& read=found->second;read.pending=false;if(read.revision!=revision)return;
                if(!rows || rows->GetFieldCount()!=3){read.retryAt=NowMs()+30000;return;}
                const auto* f=rows->Fetch();read.outcome=f[0].GetCppString();read.evidence=f[1].GetCppString();
                read.unresolved=f[2].GetUInt32();read.complete=true;
            },sql.c_str())) {state->ioPending=read.pending=false;read.retryAt=NowMs()+30000;}
            return stop("party_auction_receipt_pending");
        }
        if(read.unresolved)return stop("party_auction_native_operation_requires_reconciliation");
        if(read.outcome!="rejected" && (read.outcome!="verified" || read.evidence!="native_auction_post_escrow_and_deposit_observed"))
            return stop("party_auction_native_receipt_invalid");
        if(saved->retryAtMs>NowMs())return stop("party_auction_native_retry_wait");
        if(!(current==saved->context))return advance(Phase::Reconciling,"party_auction_post");
        return advance(Phase::Preparing,"party_auction_prepare");
    }
    if(!(current==saved->context))return advance(Phase::Reconciling,saved->checkpoint.step);
    const auto safety=ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
    std::string pause=safety?NativeSafetyReason(safety):bot->GetMap()->IsDungeon()?"native_dungeon_commitment":
        bot->GetTradeData()?"native_trade_in_progress":LivingServiceExecution::Busy(bot)?LivingServiceExecution::Blocker(bot):"";
    if(pause.empty())pause=SavedPartyBlocker(*bot,*saved,Mask(Effect::Inventory));
    if(!pause.empty()) {
        if(saved->phase==Phase::Preparing || saved->phase==Phase::Traveling)return advance(Phase::Paused,saved->checkpoint.step,pause);
        return stop(pause);
    }
    if(saved->retryAtMs>NowMs())return stop(saved->checkpoint.blocker);
    if(const auto phase=PartyAuctionResumePhase(saved->phase))return advance(*phase,saved->checkpoint.step);
    if(saved->checkpoint.step==ServiceStep(ServiceDestination::AuctionHouse)) {
        if(saved->phase==Phase::Preparing)return advance(Phase::Traveling,ServiceStep(ServiceDestination::AuctionHouse));
        const auto route=sPlayerbotOrganicEconomy.ReachSavedService(actor,id,saved->revision,ServiceDestination::AuctionHouse);
        Task next;
        if(CheckpointServiceTravel(*saved,route,NowMs(),"party_auction_prepare",next)) {
            TaskRequest r;r.task=std::move(next);r.expectedRevision=saved->revision;r.receipt=NewId();return stop(SubmitTask(r).blocker);
        }
        return stop(route.blocker);
    }
    if(saved->phase!=Phase::Preparing)return stop("party_auction_preparation_required");
    AuctionPostQuote quote;std::vector<ClaimConsumption> held;
    if(!PlanNativePartyAuctionPost(*bot,*saved,quote,held,blocker)) {
        if(blocker=="party_auction_travel_required")return advance(Phase::Traveling,ServiceStep(ServiceDestination::AuctionHouse));
        if(blocker=="party_auction_accepted_stack_changed_or_protected" || blocker=="auction_posting_listing_limit" ||
            blocker=="party_auction_discretionary_deposit_shortfall" || blocker=="auction_posting_disposition_changed")
            return advance(Phase::Deferred,"party_auction_prepare",blocker);
        return stop(blocker);
    }
    NativeAuctionPost adapter(quote);
    const auto grant=AcquireSavedTask(id,saved->revision,adapter.OperationEffects(),60000,"party_auction_prepare");
    if(!grant.Permitted())return stop(grant.blocker);
    if(held.empty()) {
        ResourceClaim item;item.id=NewId();item.task=id;item.actor=actor;item.itemGuid=quote.item.guid;item.itemEntry=quote.item.entry;
        item.quantity=quote.item.quantity;item.location="bags";item.state="held";
        ReservationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
        ++r.transition.task.revision;r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();
        r.authorization=grant.action;r.changes.push_back({item,0});
        if(quote.deposit) {
            ResourceClaim money;money.id=NewId();money.task=id;money.actor=actor;money.location="money";money.state="held";money.copper=quote.deposit;
            r.changes.push_back({money,0});
        }
        NativeAuctionPostReservation reservation;
        return stop(SubmitResourceReservation(r,reservation).blocker);
    }
    OperationRequest r;r.transition.task=*saved;r.transition.expectedRevision=saved->revision;
    ++r.transition.task.revision;r.transition.task.phase=Phase::Executing;r.transition.task.checkpoint.step="party_auction_post";
    r.transition.task.updatedAtMs=NowMs();r.transition.receipt=NewId();r.authorization=grant.action;
    r.kind=adapter.OperationKind();r.effects=adapter.OperationEffects();r.persistence=adapter.PersistencePolicy();
    r.beforeState=EncodeAuctionPostQuote(quote);r.consumption=held;
    return stop(SubmitOperationIntent(r,adapter).blocker);
}
