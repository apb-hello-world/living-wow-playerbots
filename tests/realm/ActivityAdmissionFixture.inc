// Marked, copied-realm fixture. Uses the actual coordinator, CharacterDatabase
// transaction/receipt callbacks and native TravelTarget, not a fake DB class.
// No item/money/quest/talent effects, and no fabricated completed task.
namespace {
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
}
void LivingActivityCoordinator::RunIsolatedAdmissionFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    RequireIsolatedNativeFixtureEnvironment();
    const bool operationSuite = std::string(std::getenv("LIVING_WOW_NATIVE_FIXTURE")) == "activity-operations-v1";
    const std::string suite = operationSuite ? "activity-operations-v1" : "activity-admission-v1";
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
        report.put("fixture_task_cancelled", passed);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        report.add_child("checks", state->admissionFixtureChecks);
        std::ofstream evidence("/isolated/evidence/" + std::string(operationSuite ? "activity-operations-" : "activity-admission-") + state->boot + ".json");
        evidence << Json(report); evidence.close();
        if (!evidence) sLog.outError("Living isolated admission evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
    };
    if (state->admissionFixtureStep && NowMs() > state->admissionFixtureDeadline) {
        finish(false, "fixture_receipt_deadline"); return;
    }
    if (sWorld.GetActiveAndQueuedSessionCount()) return;
    if (!state->loaded || state->ioPending || !state->pending.empty() || !state->incoming.empty()) return;
    auto require = [&](bool result, const char* name) {
        boost::property_tree::ptree check; check.put("case", name); check.put("passed", result);
        state->admissionFixtureChecks.push_back({"", check});
        if (!result) throw std::runtime_error(name);
    };
    try {
        if (!state->admissionFixtureStep) {
            for (const auto& binding : state->bindings) {
                Player* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
                if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->GetGroup() || NativeSafety(bot)) continue;
                const auto context = ReadNativeContext(*bot, state->policyRevision, state->boot);
                if (!context.mapGeneration || state->authority.Read(binding.first).lease.actor) continue;
                auto& request = state->admissionFixtureRequest;
                request.receipt = NewId(); request.task.source = "isolated_admission_fixture";
                request.task.sourceKey = state->boot; request.task.id = request.task.root = SourceId(request.task.source, request.task.sourceKey);
                request.task.actor = binding.first; request.task.context = context;
                request.task.kind = Kind::Maintenance; request.task.priority = Priority::Optional;
                request.task.mode = Mode::Active; request.task.phase = Phase::Queued;
                request.task.checkpoint.step = "native_admission_fixture";
                request.task.createdAtMs = request.task.updatedAtMs = NowMs();
                state->admissionFixtureDeadline = NowMs() + 180000;
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
        if (operationSuite && state->admissionFixtureStep == 2) {
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
                Mask(Effect::TravelTarget), 60000, "isolated_fixture");
            require(grant.Permitted(), "exact_journalled_task_receives_grant");
            NativeTargetProbe adapter(state->operationFixtureCalls);
            const auto id = state->operationFixtureRequest.transition.receipt;
            const auto result = DispatchSavedOperation(id, grant, adapter);
            require(result.executed && result.outcomeQueued && state->operationFixtureCalls == 1,
                "persisted_intent_executes_native_callback_once");
            require(!DispatchSavedOperation(id, grant, adapter).executed && state->operationFixtureCalls == 1,
                "repeated_dispatch_before_ack_is_rejected");
            require(state->pending.size() == 1 && state->pending.front().task.phase == Phase::Verifying,
                "native_probe_result_queued_for_verification");
            request.task = state->pending.front().task; state->admissionFixtureStep = 31; return;
        }
        if (operationSuite && state->admissionFixtureStep == 31) {
            require(saved->second.phase == Phase::Verifying && state->nativeOutcomes == 1 && state->operations.empty(),
                "exact_native_operation_outcome_receipt_acknowledged");
            require(!state->authority.Read(request.task.actor).lease.actor, "journalled_result_releases_transient_owner");
            NativeTargetProbe adapter(state->operationFixtureCalls);
            require(!DispatchSavedOperation(state->operationFixtureRequest.transition.receipt, {}, adapter).executed &&
                state->operationFixtureCalls == 1, "acknowledged_operation_cannot_be_replayed");
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
