// Marked, copied-realm fixture. Uses the actual coordinator, CharacterDatabase
// transaction/receipt callbacks and native TravelTarget, not a fake DB class.
// No item/quest/talent effects or fabricated completed task. The explicitly
// selected consumption suite spends one native copper and records its test-only
// restoration; this is not a vendor purchase or real-economy workflow proof.
namespace {
    class NativeWalletReservation final : public NativeReservationAdapter {
    public:
        bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) override {
            bool valid=request.transition.task.source == "isolated_admission_fixture" && request.changes.size() == 1;
            if (valid) {
                const auto& claim=request.changes.front().after;
                valid=claim.actor == actor.GetGUIDLow() && claim.copper == 1 && claim.location == "money" &&
                    (claim.state == "held" || claim.state == "released");
            }
            if (!valid) blocker="isolated_wallet_fixture_only";
            return valid;
        }
    };
    class NativeTargetProbe final : public NativeOperationAdapter {
    public:
        explicit NativeTargetProbe(unsigned& executions) : executions(executions) {}
        const char* OperationKind() const override { return "isolated_target_probe"; }
        uint32_t OperationEffects() const override { return Mask(Effect::TravelTarget); }
        bool ValidateNative(Player& actor, const OperationRequest& request, std::string& blocker) override {
            const std::string identity = "{\"actor\":" + std::to_string(actor.GetGUIDLow()) +
                ",\"level\":" + std::to_string(actor.GetLevel()) + '}';
            const bool valid = actor.GetPlayerbotAI() && actor.IsInWorld() && !NativeSafety(&actor) && request.beforeState == identity;
            if (!valid) blocker = "isolated_actor_changed";
            return valid;
        }
        NativeObservation ExecuteNative(Player& actor, const OperationRequest&) override {
            ++executions;
            auto* target = actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
            const auto before = target->GetRelevance();
            struct Restore { ai::TravelTarget* target; uint32 value; ~Restore() { target->SetRelevance(value); } } restore{target, before};
            target->SetRelevance(before ^ 0x100);
            const bool changed = target->GetRelevance() == (before ^ 0x100);
            target->SetRelevance(before);
            NativeObservation result;
            if (changed && target->GetRelevance() == before) {
                result.state = OperationState::Verified;
                result.nativeReference = "isolated_actor:" + std::to_string(actor.GetGUIDLow()) + ":travel_relevance";
                result.evidence = "native_target_probe_restored";
                result.afterState = "{\"before\":" + std::to_string(before) + ",\"restored\":true}";
            }
            return result;
        }
    private:
        unsigned& executions;
    };
    class NativeWalletConsumption final : public NativeOperationAdapter {
    public:
        explicit NativeWalletConsumption(unsigned& executions) : executions(executions) {}
        const char* OperationKind() const override { return "isolated_wallet_consumption"; }
        uint32_t OperationEffects() const override { return Mask(Effect::Money); }
        bool SupportsClaimedConsumption() const override { return true; }
        bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override {
            const bool valid=actor.IsInWorld() && actor.GetPlayerbotAI() && !NativeSafety(&actor) &&
                !actor.GetTradeData() && actor.GetMoney() >= 1 &&
                request.transition.task.source == "isolated_admission_fixture" && request.consumption.size() == 1 &&
                request.consumption.front().before.copper == 1 && request.consumption.front().used == 1 &&
                request.beforeState == "{\"money\":"+std::to_string(actor.GetMoney())+'}';
            if (!valid) blocker="isolated_wallet_changed";
            return valid;
        }
        NativeObservation ExecuteNative(Player& actor,const OperationRequest&) override {
            ++executions; const auto before=actor.GetMoney(); actor.ModifyMoney(-1);
            NativeObservation result;
            result.nativeReference="isolated_actor:"+std::to_string(actor.GetGUIDLow())+":wallet";
            result.afterState="{\"money\":"+std::to_string(actor.GetMoney())+",\"fixture_only\":true}";
            if (before && actor.GetMoney() == before-1) {
                actor.SaveToDB();
                result.state=OperationState::Verified; result.evidence="isolated_native_copper_consumed";
            }
            return result;
        }
    private:
        unsigned& executions;
    };
}
void LivingActivityCoordinator::RunIsolatedAdmissionFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished) {
        // The hold fixture leaves one decision hold for a bounded restart
        // window. It must not strand this bot if an operator does not restart.
        if (NowMs() > state->admissionFixtureDeadline) state->gameplayFixtureActor.store(0,std::memory_order_release);
        return;
    }
    if (NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    RequireIsolatedNativeFixtureEnvironment();
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool consumptionSuite=suite == "activity-consumption-v1";
    const bool operationSuite = suite == "activity-operations-v1" || consumptionSuite;
    const bool holdSuite = suite == "activity-resources-hold-v1", recoverSuite = suite == "activity-resources-recover-v1";
    const bool resourceSuite = suite == "activity-resources-v1" || holdSuite || recoverSuite || consumptionSuite;
    auto finish = [&](bool passed, const std::string& failure = "") {
        state->fixtureFinished = true;
        boost::property_tree::ptree report;
        report.put("suite", suite); report.put("test_only", true);
        report.put("boot", state->boot); report.put("task", state->admissionFixtureRequest.task.id);
        report.put("child_task", state->admissionFixtureChild.task.id);
        report.put("actor", state->admissionFixtureRequest.task.actor);
        report.put("native_probe_executions", state->operationFixtureCalls);
        report.put("operation", state->operationFixtureRequest.transition.receipt);
        report.put("outcome", passed ? "passed" : "failed"); report.put("failure", failure);
        report.put("fixture_step", state->admissionFixtureStep);
        report.put("native_inventory_operations", 0); report.put("persistent_task_completions", 0);
        report.put("fixture_task_cancelled", passed && !holdSuite);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        if (consumptionSuite) {
            Player* actor=sRandomPlayerbotMgr.GetPlayerBot(state->admissionFixtureRequest.task.actor);
            const bool restore=actor && state->operationFixtureCalls == 1 && actor->GetMoney()+1 == state->gameplayOriginalMoney;
            report.put("native_copper_consumed",state->operationFixtureCalls == 1 ? 1 : 0);
            report.put("test_restoration_copper",restore ? 1 : 0);
            report.put("native_money_before_restoration",actor ? actor->GetMoney() : 0);
            if (restore) {actor->ModifyMoney(1);actor->SaveToDB();}
            report.put("paid_service_workflow_proof",false);
        }
        if (resourceSuite) {
            if (!holdSuite || !passed) state->gameplayFixtureActor.store(0,std::memory_order_release);
            report.put("nonterminal_claim_retained_for_restart",holdSuite && passed);
            report.put("restart_recovery",recoverSuite);
            report.put("native_money_before",state->gameplayOriginalMoney);
            Player* actor=sRandomPlayerbotMgr.GetPlayerBot(state->admissionFixtureRequest.task.actor);
            report.put("native_money_after",actor ? actor->GetMoney() : 0);
            report.put("claim",state->reservationFixtureRequest.changes.empty() ? "" : state->reservationFixtureRequest.changes.front().after.id);
        }
        report.add_child("checks", state->admissionFixtureChecks);
        std::ofstream evidence("/isolated/evidence/" + suite + "-" + state->boot + ".json");
        evidence << Json(report); evidence.close();
        if (!evidence) sLog.outError("Living isolated admission evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
    };
    if (state->admissionFixtureStep && NowMs() > state->admissionFixtureDeadline) {
        finish(false, "fixture_receipt_deadline"); return;
    }
    if (sWorld.GetActiveAndQueuedSessionCount()) return;
    if (!state->loaded || state->ioPending || !state->pending.empty() || !state->incoming.empty()) return;
    if (resourceSuite && !state->resources.Protection().ready) return;
    if (!state->admissionFixtureDeadline) state->admissionFixtureDeadline=NowMs()+180000;
    if (NowMs() > state->admissionFixtureDeadline) { finish(false,"no_safely_available_fixture_actor"); return; }
    auto require = [&](bool result, const char* name) {
        boost::property_tree::ptree check; check.put("case", name); check.put("passed", result);
        state->admissionFixtureChecks.push_back({"", check});
        if (!result) throw std::runtime_error(name);
    };
    try {
        if (recoverSuite && !state->admissionFixtureStep) {
            const auto id=SourceId("isolated_admission_fixture","isolated_resource_restart_v1");
            const auto saved=state->cache.find(id);
            require(saved != state->cache.end(),"restart_task_identity_restored");
            const auto& old=saved->second;
            require(old.source == "isolated_admission_fixture" && old.sourceKey == "isolated_resource_restart_v1" &&
                old.mode == Mode::Active && old.phase == Phase::Preparing && old.context.boot != state->boot,
                "restart_preserves_accepted_preparation");
            boost::property_tree::ptree data; std::istringstream input(old.checkpoint.data);
            boost::property_tree::read_json(input,data);
            const auto* claim=state->resources.Inspect(data.get<std::string>("claim"));
            require(claim && claim->task == id && claim->actor == old.actor && claim->state == "held" &&
                claim->copper == 1 && claim->location == "money","restart_restores_exact_native_reservation");
            Player* actor=sRandomPlayerbotMgr.GetPlayerBot(old.actor);
            if (!actor || !actor->GetPlayerbotAI() || !actor->IsInWorld() || actor->GetGroup() ||
                actor->GetGuildId() || actor->GetTradeData() || NativeSafety(actor)) return;
            const auto current=ReadNativeContext(*actor,state->policyRevision,state->boot);
            if (!current.mapGeneration) return;
            state->admissionFixtureRequest.task=old;
            state->gameplayOriginalMoney=actor->GetMoney();
            require(actor->GetMoney() == data.get<uint64_t>("money"),"restart_preserves_native_money");
            state->gameplayFixtureActor.store(old.actor,std::memory_order_release);
            require(ResourceReservations().Inspect()->UnreservedMoney(old.actor,actor->GetMoney()) == actor->GetMoney()-1,
                "restored_claim_protects_native_money");
            const bool original=state->enforceEffects.exchange(true,std::memory_order_acq_rel);
            struct Restore { State& state; bool value; ~Restore(){state.enforceEffects.store(value,std::memory_order_release);} }
                restore{*state,original};
            require(!AcquireSavedTask(id,old.revision,Mask(Effect::Money),60000,"isolated_fixture").Permitted(),
                "restart_old_context_cannot_acquire_lease");
            auto& request=state->admissionFixtureRequest;
            request.expectedRevision=old.revision; ++request.task.revision; request.receipt=NewId();
            request.task.context=current; request.task.phase=Phase::Reconciling; request.task.updatedAtMs=NowMs();
            require(SubmitTask(request).code == AdmissionCode::Pending,"restart_native_context_reconciliation_queued");
            state->reservationFixtureRequest.changes={{*claim,claim->revision}};
            state->admissionFixtureStep=60; return;
        }
        if (!state->admissionFixtureStep) {
            for (const auto& binding : state->bindings) {
                Player* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
                if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->GetGroup() || NativeSafety(bot)) continue;
                if (resourceSuite && (bot->GetGuildId() || bot->GetMoney() < 10 || bot->GetTradeData() ||
                    sPlayerbotActionBroker.ReservedCopper(bot->GetGUIDLow()))) continue;
                const auto context = ReadNativeContext(*bot, state->policyRevision, state->boot);
                if (!context.mapGeneration || state->authority.Read(binding.first).lease.actor) continue;
                auto& request = state->admissionFixtureRequest;
                request.receipt = NewId(); request.task.source = "isolated_admission_fixture";
                request.task.sourceKey = holdSuite ? "isolated_resource_restart_v1" : state->boot;
                request.task.id = request.task.root = SourceId(request.task.source, request.task.sourceKey);
                request.task.actor = binding.first; request.task.context = context;
                request.task.kind = Kind::Maintenance; request.task.priority = Priority::Optional;
                request.task.mode = Mode::Active; request.task.phase = Phase::Queued;
                request.task.checkpoint.step = "native_admission_fixture";
                request.task.createdAtMs = request.task.updatedAtMs = NowMs();
                state->admissionFixtureDeadline = NowMs() + 180000;
                if (resourceSuite) {
                    state->gameplayOriginalMoney=bot->GetMoney();
                    state->gameplayFixtureActor.store(bot->GetGUIDLow(),std::memory_order_release);
                }
                break;
            }
            if (!state->admissionFixtureRequest.task.actor) return;
        }
        auto& request = state->admissionFixtureRequest;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(request.task.actor);
        require(bot && bot->GetPlayerbotAI() && bot->IsInWorld() && !NativeSafety(bot), "fixture_actor_safely_available");
        auto* ai = bot->GetPlayerbotAI();
        const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
        require(current == request.task.context, "fixture_native_context_still_current");
        // Only this synchronous fixture call may enforce. The destructor runs
        // before map updates, including assertion failures and early returns.
        const bool originalEnforcement = state->enforceEffects.load(std::memory_order_acquire);
        struct Enforcement { State& state; bool original; ~Enforcement() { state.enforceEffects.store(original, std::memory_order_release); } }
            enforcement{*state, originalEnforcement};
        state->enforceEffects.store(true, std::memory_order_release);
        if (state->admissionFixtureStep == 0) {
            require(SubmitTask(request).code == AdmissionCode::Pending, "accepted_request_queues_native_persistence");
            require(SubmitTask(request).code == AdmissionCode::Pending && state->pending.size() == 1,
                "duplicate_submission_has_one_write");
            auto conflict = request; conflict.task.checkpoint.data = "{\"changed\":true}";
            require(SubmitTask(conflict).code == AdmissionCode::ConflictingWrite, "changed_pending_request_rejected");
            require(!AcquireSavedTask(request.task.id, 1, Mask(Effect::TravelTarget), 60000, "isolated_fixture").authority.Granted(),
                "enqueued_write_is_not_execution_authority");
            state->admissionFixtureStep = 1; return;
        }
        const auto saved = state->cache.find(request.task.id);
        require(saved != state->cache.end() && saved->second.revision == request.task.revision,
            "exact_native_receipt_acknowledged");
        if (recoverSuite && state->admissionFixtureStep == 60) {
            require(saved->second.phase == Phase::Reconciling,"restart_context_reconciliation_acknowledged");
            request.expectedRevision=request.task.revision++; request.receipt=NewId();
            request.task.phase=Phase::Preparing; request.task.updatedAtMs=NowMs();
            require(SubmitTask(request).code == AdmissionCode::Pending,"restart_resume_preparation_queued");
            state->admissionFixtureStep=61; return;
        }
        if (recoverSuite && state->admissionFixtureStep == 61) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,Mask(Effect::Money),60000,"isolated_fixture");
            require(grant.Permitted(),"restart_fresh_saved_predecessor_granted");
            auto& reservation=state->reservationFixtureRequest;
            reservation.transition=request; reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision; reservation.transition.receipt=NewId();
            reservation.transition.task.updatedAtMs=NowMs(); reservation.authorization=grant.action;
            auto& change=reservation.changes.front(); ++change.after.revision; change.after.state="released";
            NativeWalletReservation adapter;
            require(SubmitResourceReservation(reservation,adapter).code == AdmissionCode::Pending,"restart_release_queued");
            require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney()-1,
                "restart_release_waits_for_exact_receipt");
            request=reservation.transition; state->admissionFixtureStep=41; return;
        }
        if (resourceSuite && state->admissionFixtureStep == 2) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,Mask(Effect::Money),60000,"isolated_fixture");
            require(grant.Permitted(),"reservation_predecessor_grant");
            auto& reservation=state->reservationFixtureRequest;
            reservation.transition=request; reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision; reservation.transition.receipt=NewId();
            reservation.transition.task.updatedAtMs=NowMs(); reservation.authorization=grant.action;
            ResourceClaim claim; claim.id=NewId(); claim.task=request.task.id; claim.actor=request.task.actor;
            claim.copper=1; claim.location="money"; claim.state="held";
            if (holdSuite) reservation.transition.task.checkpoint.data="{\"claim\":\""+claim.id+
                "\",\"money\":"+std::to_string(bot->GetMoney())+"}";
            reservation.changes={{claim,0}};
            NativeWalletReservation adapter;
            require(SubmitResourceReservation(reservation,adapter).code == AdmissionCode::Pending,"native_money_reservation_queued");
            require(SubmitResourceReservation(reservation,adapter).code == AdmissionCode::Pending && state->pending.size() == 1,
                "duplicate_pending_reservation_has_one_write");
            auto changed=reservation; changed.changes.front().after.copper=2;
            require(SubmitResourceReservation(changed,adapter).code == AdmissionCode::ConflictingWrite,"changed_reservation_rejected");
            const auto view=ResourceReservations().Inspect();
            require(view && view->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney()-1,
                "pending_reservation_protected_before_receipt");
            require(!state->resources.Inspect(claim.id),"pending_claim_not_falsely_saved");
            request=reservation.transition; state->admissionFixtureStep=40; return;
        }
        if (resourceSuite && state->admissionFixtureStep == 40) {
            auto& reservation=state->reservationFixtureRequest; NativeWalletReservation adapter;
            const auto* claim=state->resources.Inspect(reservation.changes.front().after.id);
            require(claim && claim->state == "held" && !state->resources.PendingCount(),"native_reservation_receipt_installed");
            require(SubmitResourceReservation(reservation,adapter).code == AdmissionCode::Saved,"saved_reservation_duplicate_read_only");
            require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney()-1,
                "saved_reservation_not_double_counted");
            if (holdSuite) {
                require(bot->GetMoney() == state->gameplayOriginalMoney,"native_money_unchanged");
                state->enforceEffects.store(originalEnforcement,std::memory_order_release); finish(true); return;
            }
            if (consumptionSuite) {
                const auto grant=AcquireSavedTask(request.task.id,request.task.revision,Mask(Effect::Money),60000,"isolated_fixture");
                require(grant.Permitted(),"consumption_predecessor_grant");
                auto& operation=state->operationFixtureRequest;
                operation.transition=request; operation.transition.expectedRevision=request.task.revision;
                ++operation.transition.task.revision; operation.transition.task.phase=Phase::Executing;
                operation.transition.task.updatedAtMs=NowMs(); operation.transition.receipt=NewId();
                operation.authorization=grant.action; operation.kind="isolated_wallet_consumption";
                operation.effects=Mask(Effect::Money); operation.consumption={{*claim,1}};
                operation.beforeState="{\"money\":"+std::to_string(bot->GetMoney())+'}';
                NativeWalletConsumption consume(state->operationFixtureCalls);
                auto wrong=operation; ++wrong.consumption.front().before.revision;
                require(SubmitOperationIntent(wrong,consume).blocker == "acknowledged_claim_changed",
                    "consumption_rejects_unacknowledged_claim_revision");
                require(SubmitOperationIntent(operation,consume).code == AdmissionCode::Pending,"consumption_intent_queued");
                require(SubmitOperationIntent(operation,consume).code == AdmissionCode::Pending && state->pending.size() == 1,
                    "duplicate_consumption_intent_has_one_write");
                require(!DispatchSavedOperation(operation.transition.receipt,grant,consume).executed && !state->operationFixtureCalls,
                    "consumption_waits_for_saved_intent");
                request=operation.transition; state->admissionFixtureStep=30; return;
            }
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,Mask(Effect::Money),60000,"isolated_fixture");
            require(grant.Permitted(),"release_predecessor_grant");
            reservation.transition.expectedRevision=reservation.transition.task.revision++;
            reservation.transition.receipt=NewId(); reservation.transition.task.updatedAtMs=NowMs();
            reservation.authorization=grant.action;
            auto& change=reservation.changes.front(); change.expectedRevision=change.after.revision++;
            change.after.state="released";
            require(SubmitResourceReservation(reservation,adapter).code == AdmissionCode::Pending,"native_release_queued");
            require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney()-1,
                "queued_release_keeps_protection");
            request=reservation.transition; state->admissionFixtureStep=41; return;
        }
        if (resourceSuite && state->admissionFixtureStep == 41) {
            const auto* claim=state->resources.Inspect(state->reservationFixtureRequest.changes.front().after.id);
            require(claim && claim->state == "released" && !state->resources.PendingCount(),"release_receipt_installed");
            require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney(),
                "receipt_releases_exact_reserved_amount");
            require(bot->GetMoney() == state->gameplayOriginalMoney,"native_money_unchanged");
            request.expectedRevision=request.task.revision++; request.task.phase=Phase::Cancelled;
            request.task.checkpoint.blocker="isolated_fixture_finished"; request.task.updatedAtMs=NowMs();
            state->Queue(request.task,request.expectedRevision,"isolated_fixture_cleanup");
            state->nextWork=0; state->admissionFixtureStep=42; return;
        }
        if (resourceSuite && state->admissionFixtureStep == 42) {
            require(saved->second.phase == Phase::Cancelled,"reservation_fixture_cancelled_with_receipt");
            state->enforceEffects.store(originalEnforcement,std::memory_order_release); finish(true); return;
        }
        if (operationSuite && !consumptionSuite && state->admissionFixtureStep == 2) {
            const auto grant = AcquireSavedTask(request.task.id, request.task.revision,
                Mask(Effect::TravelTarget), 60000, "isolated_fixture");
            require(grant.Permitted(), "native_intent_predecessor_grant");
            auto& operation = state->operationFixtureRequest;
            operation.transition = request; operation.transition.expectedRevision = request.task.revision;
            ++operation.transition.task.revision; operation.transition.task.phase = Phase::Executing;
            operation.transition.receipt = NewId(); operation.transition.task.updatedAtMs = NowMs();
            operation.authorization = grant.action; operation.kind = "isolated_target_probe";
            operation.effects = Mask(Effect::TravelTarget);
            operation.beforeState = "{\"actor\":" + std::to_string(bot->GetGUIDLow()) + ",\"level\":" + std::to_string(bot->GetLevel()) + '}';
            NativeTargetProbe adapter(state->operationFixtureCalls);
            require(SubmitOperationIntent(operation, adapter).code == AdmissionCode::Pending, "native_intent_queued_before_effect");
            require(SubmitOperationIntent(operation, adapter).code == AdmissionCode::Pending && state->pending.size() == 1,
                "duplicate_intent_has_one_native_write");
            require(!DispatchSavedOperation(operation.transition.receipt, grant, adapter).executed && !state->operationFixtureCalls,
                "unacknowledged_intent_cannot_execute");
            request = operation.transition; state->admissionFixtureStep = 30; return;
        }
        if (operationSuite && state->admissionFixtureStep == 30) {
            const auto grant = AcquireSavedTask(request.task.id, request.task.revision,
                consumptionSuite ? Mask(Effect::Money) : Mask(Effect::TravelTarget), 60000, "isolated_fixture");
            require(grant.Permitted(), "exact_journalled_task_receives_grant");
            NativeTargetProbe target(state->operationFixtureCalls); NativeWalletConsumption wallet(state->operationFixtureCalls);
            NativeOperationAdapter& adapter=consumptionSuite ? static_cast<NativeOperationAdapter&>(wallet) : target;
            const auto id = state->operationFixtureRequest.transition.receipt;
            const auto result = DispatchSavedOperation(id, grant, adapter);
            require(result.executed && result.outcomeQueued && state->operationFixtureCalls == 1,
                "persisted_intent_executes_native_callback_once");
            require(!DispatchSavedOperation(id, grant, adapter).executed && state->operationFixtureCalls == 1,
                "repeated_dispatch_before_ack_is_rejected");
            require(state->pending.size() == 1 && state->pending.front().task.phase == Phase::Verifying,
                "native_probe_result_queued_for_verification");
            if (consumptionSuite) {
                const auto* claim=state->resources.Inspect(state->reservationFixtureRequest.changes.front().after.id);
                require(claim && claim->state == "held" && claim->revision == 1,"consumed_claim_waits_for_receipt");
                require(bot->GetMoney()+1 == state->gameplayOriginalMoney,"native_consumption_changed_exactly_one_copper");
                require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney()-1,
                    "consumed_money_protected_until_receipt");
            }
            request.task = state->pending.front().task; state->admissionFixtureStep = 31; return;
        }
        if (operationSuite && state->admissionFixtureStep == 31) {
            require(saved->second.phase == Phase::Verifying && state->nativeOutcomes == 1 && state->operations.empty(),
                "exact_native_operation_outcome_receipt_acknowledged");
            require(!state->authority.Read(request.task.actor).lease.actor, "journalled_result_releases_transient_owner");
            NativeTargetProbe target(state->operationFixtureCalls); NativeWalletConsumption wallet(state->operationFixtureCalls);
            NativeOperationAdapter& adapter=consumptionSuite ? static_cast<NativeOperationAdapter&>(wallet) : target;
            require(!DispatchSavedOperation(state->operationFixtureRequest.transition.receipt, {}, adapter).executed &&
                state->operationFixtureCalls == 1, "acknowledged_operation_cannot_be_replayed");
            if (consumptionSuite) {
                const auto* claim=state->resources.Inspect(state->reservationFixtureRequest.changes.front().after.id);
                require(claim && claim->state == "consumed" && claim->revision == 2,"consumption_receipt_installs_exact_claim");
                require(ResourceReservations().Inspect()->UnreservedMoney(bot->GetGUIDLow(),bot->GetMoney()) == bot->GetMoney(),
                    "consumption_receipt_releases_protection");
                require(bot->GetMoney()+1 == state->gameplayOriginalMoney,"native_consumption_not_repeated_after_receipt");
            }
            request.expectedRevision = request.task.revision++; request.task.phase = Phase::Cancelled;
            request.task.checkpoint.blocker = "isolated_fixture_finished"; request.task.updatedAtMs = NowMs();
            state->Queue(request.task, request.expectedRevision, "isolated_fixture_cleanup");
            state->nextWork = 0; state->admissionFixtureStep = 32; return;
        }
        if (operationSuite && state->admissionFixtureStep == 32) {
            require(saved->second.phase == Phase::Cancelled, "probe_task_cancelled_with_native_receipt");
            state->enforceEffects.store(originalEnforcement, std::memory_order_release); finish(true); return;
        }
        if (state->admissionFixtureStep == 1) {
            require(SubmitTask(request).code == AdmissionCode::Saved, "acknowledged_duplicate_is_read_only");
            require(!AcquireSavedTask(request.task.id, 1, Mask(Effect::TravelTarget), 60000, "isolated_fixture").authority.Granted(),
                "queued_task_cannot_execute");
            request.expectedRevision = request.task.revision++; request.receipt = NewId();
            request.task.phase = Phase::Preparing; request.task.updatedAtMs = NowMs();
            require(SubmitTask(request).code == AdmissionCode::Pending, "preparation_transition_queued");
            require(!AcquireSavedTask(request.task.id, 1, Mask(Effect::TravelTarget), 60000, "isolated_fixture").authority.Granted(),
                "old_saved_revision_cannot_bypass_pending_transition");
            state->admissionFixtureStep = 2; return;
        }
        if (state->admissionFixtureStep == 2) {
            auto& child = state->admissionFixtureChild;
            child.task = request.task; child.task.sourceKey += ":prepare";
            child.task.id = SourceId(child.task.source, child.task.sourceKey);
            child.task.parent = child.task.root = request.task.id;
            child.task.revision = 1; child.task.phase = Phase::Queued;
            child.task.createdAtMs = child.task.updatedAtMs = NowMs();
            child.task.checkpoint.step = "native_child_fixture";
            child.rootRevision = request.task.revision;
            child.task.checkpoint.data = "{\"_root_revision\":" + std::to_string(child.rootRevision) + "}";
            child.receipt = NewId();
            require(SubmitTask(child).code == AdmissionCode::Pending, "child_admitted_under_saved_parent_revision");
            require(!AcquireSavedTask(request.task.id, request.task.revision, Mask(Effect::TravelTarget), 60000,
                "isolated_fixture").Permitted(), "parent_waits_for_pending_preparation_receipt");
            state->admissionFixtureStep = 20; return;
        }
        if (state->admissionFixtureStep >= 20 && state->admissionFixtureStep <= 23) {
            auto& child = state->admissionFixtureChild;
            const auto childSaved = state->cache.find(child.task.id);
            require(childSaved != state->cache.end() && childSaved->second.revision == child.task.revision,
                "exact_child_native_receipt_acknowledged");
            if (state->admissionFixtureStep == 20) {
                child.expectedRevision = child.task.revision++; child.receipt = NewId();
                child.task.phase = Phase::Preparing; child.task.updatedAtMs = NowMs();
                require(SubmitTask(child).code == AdmissionCode::Pending, "child_preparation_transition_queued");
                state->admissionFixtureStep = 21; return;
            }
            if (state->admissionFixtureStep == 21) {
                const auto rootGrant = AcquireSavedTask(request.task.id, request.task.revision,
                    Mask(Effect::TravelTarget), 60000, "isolated_fixture");
                require(rootGrant.Permitted(), "saved_parent_retains_preparation_authority");
                auto* target = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
                const auto relevance = target->GetRelevance();
                struct RestoreChild { std::function<void()> fn; ~RestoreChild() { fn(); } }
                    restore{[&] {
                        state->enforceEffects.store(false, std::memory_order_release);
                        target->SetRelevance(relevance); ReleaseTaskLease(rootGrant.authority.lease);
                    }};
                const auto childGrant = SelectSavedStep(rootGrant.authority.lease, child.task.id,
                    child.task.revision, Mask(Effect::TravelTarget), "isolated_fixture");
                require(childGrant.Permitted() && SameLease(rootGrant.authority.lease, childGrant.authority.lease),
                    "saved_child_inherits_exact_parent_lease");
                {
                    ExecutionScope parentScope(rootGrant.task, rootGrant.action);
                    target->SetRelevance(relevance ^ 0x100);
                    require(target->GetRelevance() == relevance, "saved_parent_cannot_replace_child_target");
                }
                {
                    ExecutionScope childScope(childGrant.task, childGrant.action);
                    target->SetRelevance(relevance ^ 0x100);
                    require(target->GetRelevance() == (relevance ^ 0x100), "saved_child_scope_performs_native_mutation");
                }
                const auto resumed = SelectSavedStep(rootGrant.authority.lease, "", request.task.revision,
                    Mask(Effect::TravelTarget), "isolated_fixture");
                require(resumed.Permitted() && SameLease(resumed.authority.lease, childGrant.authority.lease),
                    "saved_parent_resumes_without_second_owner");
                {
                    ExecutionScope childScope(childGrant.task, childGrant.action);
                    target->SetRelevance(relevance);
                    require(target->GetRelevance() == (relevance ^ 0x100), "returned_child_scope_is_stale");
                }
                {
                    ExecutionScope parentScope(resumed.task, resumed.action);
                    target->SetRelevance(relevance);
                    require(target->GetRelevance() == relevance, "resumed_saved_parent_has_native_authority");
                }
                require(ReleaseTaskLease(resumed.authority.lease).code == AuthorityCode::Released,
                    "saved_child_bundle_releases_once");
                child.expectedRevision = child.task.revision++; child.receipt = NewId();
                child.task.phase = Phase::Paused; child.task.checkpoint.blocker = "isolated_fixture_finished";
                child.task.updatedAtMs = NowMs();
                require(SubmitTask(child).code == AdmissionCode::Pending, "child_pause_retains_root_identity");
                state->admissionFixtureStep = 22; return;
            }
            if (state->admissionFixtureStep == 22) {
                require(childSaved->second.phase == Phase::Paused, "paused_child_retained_after_receipt");
                child.expectedRevision = child.task.revision++; child.task.phase = Phase::Cancelled;
                child.task.updatedAtMs = NowMs();
                // Fixture-only cleanup; native cancellation still guards pending claims/intents.
                state->Queue(child.task, child.expectedRevision, "isolated_fixture_cleanup");
                state->nextWork = 0; state->admissionFixtureStep = 23; return;
            }
            require(childSaved->second.phase == Phase::Cancelled, "test_only_child_cancelled_with_native_receipt");
            state->admissionFixtureStep = 24;
        }
        if (state->admissionFixtureStep == 24) {
            auto grant = AcquireSavedTask(request.task.id, request.task.revision, Mask(Effect::TravelTarget), 60000, "isolated_fixture");
            require(grant.authority.Granted(), "acknowledged_preparation_receives_exact_grant");
            auto* target = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
            const auto relevance = target->GetRelevance();
            struct Restore {
                std::function<void()> fn; ~Restore() { fn(); }
            } restore{[&] {
                state->enforceEffects.store(false, std::memory_order_release);
                target->SetRelevance(relevance);
                state->authority.Release(grant.authority.lease);
                state->bindings.at(request.task.actor).publisher.Publish(state->authority.Read(request.task.actor));
            }};
            ExecutionScope scope(grant.task, grant.action);
            target->SetRelevance(relevance ^ 0x100);
            require(target->GetRelevance() == (relevance ^ 0x100), "saved_task_scope_performs_native_target_mutation");
            request.expectedRevision = request.task.revision++; request.receipt = NewId();
            request.task.phase = Phase::Paused; request.task.checkpoint.blocker = "isolated_fixture_pause";
            request.task.updatedAtMs = NowMs();
            require(SubmitTask(request).code == AdmissionCode::Pending, "pause_transition_queues_without_losing_identity");
            target->SetRelevance(relevance);
            require(target->GetRelevance() == (relevance ^ 0x100), "pending_revision_immediately_revokes_old_scope");
            restore.fn(); restore.fn = [] {};
            require(target->GetRelevance() == relevance && !state->authority.Read(request.task.actor).lease.actor,
                "native_transient_state_restored_before_map_updates");
            state->admissionFixtureStep = 3; return;
        }
        if (state->admissionFixtureStep == 3) {
            require(saved->second.phase == Phase::Paused, "paused_task_retained_after_receipt");
            require(!AcquireSavedTask(request.task.id, request.task.revision, Mask(Effect::TravelTarget), 60000, "isolated_fixture").authority.Granted(),
                "paused_task_has_no_execution_authority");
            request.expectedRevision = request.task.revision++;
            request.task.phase = Phase::Cancelled; request.task.checkpoint.blocker = "isolated_fixture_finished";
            request.task.updatedAtMs = NowMs();
            // The isolated fixture never acquired native resources or started
            // an operation. Actual cancellation SQL still requires no pending
            // claims/intents. Do not generate a fake completed outcome.
            state->Queue(request.task, request.expectedRevision, "isolated_fixture_cleanup");
            state->nextWork = 0; state->admissionFixtureStep = 4; return;
        }
        require(saved->second.phase == Phase::Cancelled, "test_only_task_cancelled_with_native_receipt");
        state->enforceEffects.store(originalEnforcement, std::memory_order_release);
        finish(true);
    } catch (const std::exception& error) { finish(false, error.what()); }
}
