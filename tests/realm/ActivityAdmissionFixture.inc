// Marked, copied-realm fixture. Uses the actual coordinator, CharacterDatabase
// transaction/receipt callbacks and native TravelTarget, not a fake DB class.
// No item/money/quest/talent effects, and no fabricated completed task.
void LivingActivityCoordinator::RunIsolatedAdmissionFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    RequireIsolatedNativeFixtureEnvironment();
    auto finish = [&](bool passed, const std::string& failure = "") {
        state->fixtureFinished = true;
        boost::property_tree::ptree report;
        report.put("suite", "activity-admission-v1"); report.put("test_only", true);
        report.put("boot", state->boot); report.put("task", state->admissionFixtureRequest.task.id);
        report.put("actor", state->admissionFixtureRequest.task.actor);
        report.put("outcome", passed ? "passed" : "failed"); report.put("failure", failure);
        report.put("fixture_step", state->admissionFixtureStep);
        report.put("native_inventory_operations", 0); report.put("persistent_task_completions", 0);
        report.put("fixture_task_cancelled", passed);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        report.add_child("checks", state->admissionFixtureChecks);
        std::ofstream evidence("/isolated/evidence/activity-admission-" + state->boot + ".json");
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
                state->admissionFixtureDeadline = NowMs() + 120000;
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
