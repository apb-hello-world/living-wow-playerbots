// No grants, forced travel, spells or purchases. Reconcile a prior boot's exact
// verified skill-job receipt and backed goods through the real finite executor.
void LivingActivityCoordinator::RunIsolatedProfessionRecoveryFixture() {
    MANGOS_ASSERT(OnWorldThread());RequireIsolatedNativeFixtureEnvironment();
    if (state->fixtureFinished || NowMs()<state->fixtureNext) return;
    state->fixtureNext=NowMs()+1000;
    if (sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty() || !state->resources.Protection().ready) return;
    auto& fixture=state->professionRecoveryFixture;
    if (!fixture.deadline) fixture.deadline=NowMs()+180000;
    auto emit=[&](bool passed,const std::string& failure) {
        boost::property_tree::ptree report;
        report.put("suite","activity-profession-recover-v1");report.put("boot",state->boot);
        report.put("test_only",true);report.put("outcome",passed ? "passed" : "failed");
        report.put("failure",failure);report.put("blocker",fixture.blocker);report.put("step",fixture.step);
        report.put("actor",fixture.beforeTask.actor);report.put("task",fixture.beforeTask.id);
        report.put("previous_boot",fixture.beforeTask.context.boot);report.put("previous_revision",fixture.beforeTask.revision);
        report.put("operation",fixture.operation);report.put("settlement_receipt",fixture.receipt);
        report.put("money_before",fixture.before.money);report.put("money_after",fixture.after.money);
        report.put("skill_before",fixture.before.skill);report.put("skill_after",fixture.after.skill);
        auto stacks=[](const CraftFrame& frame) {
            boost::property_tree::ptree rows;
            for (const auto& item : frame.stacks) {
                boost::property_tree::ptree row;
                row.put("guid",item.guid);row.put("entry",item.entry);row.put("count",item.count);
                row.put("bag",item.bagGuid);row.put("slot",uint32_t(item.slot));rows.push_back({"",row});
            }
            return rows;
        };
        report.add_child("stacks_before",stacks(fixture.before));report.add_child("stacks_after",stacks(fixture.after));
        report.put("native_launches",0);report.put("purchases",0);report.put("item_grants",0);report.put("gold_grants",0);
        report.put("general_execution_enabled",false);report.put("restart_completion_proof",passed);
        report.put("process_crash_proof",false);report.put("travel_proof",false);report.add_child("checks",fixture.checks);
        if (fixture.beforeTask.actor) report.put("actor_diagnostic",ActorJson(fixture.beforeTask.actor));
        std::ofstream evidence("/isolated/evidence/activity-profession-recover-"+state->boot+".json");
        evidence<<Json(report);evidence.close();
        if (!evidence) sLog.outError("Living isolated profession recovery evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        state->gameplayFixtureActor.store(0,std::memory_order_release);state->fixtureFinished=true;
    };
    auto require=[&](bool value,const char* name) {
        boost::property_tree::ptree check;check.put("case",name);check.put("passed",value);fixture.checks.push_back({"",check});
        if (!value) throw std::runtime_error(name);
    };
    if (NowMs()>fixture.deadline) {emit(false,"fixture_deadline");return;}
    try {
        if (!fixture.beforeTask.actor) {
            const Task* chosen=nullptr;
            for (const auto& row : state->cache) {
                const auto& task=row.second;ProfessionJob job;std::string blocker;
                if (task.mode!=Mode::Active || task.phase!=Phase::Verifying || task.source!="profession_job" ||
                    task.root!=task.id || !task.parent.empty() || task.context.boot==state->boot ||
                    !DecodeProfessionJob(task.checkpoint.data,job,blocker) || job.purpose!=ProfessionPurpose::SkillGain ||
                    job.recipe!=2329) continue;
                auto* actor=sRandomPlayerbotMgr.GetPlayerBot(task.actor);
                // A current party/route is not displaced by bookkeeping-only
                // completion. The real executor does not acquire that lease.
                if (!actor || !actor->GetPlayerbotAI() || !actor->IsInWorld() ||
                    actor->GetTradeData() || NativeSafety(actor)) continue;
                if (!chosen || task.id<chosen->id) chosen=&task;
            }
            if (!chosen) {fixture.blocker="no_safe_prior_verified_skill_job";return;}
            fixture.beforeTask=*chosen;fixture.receipt=NewId();
            state->gameplayFixtureActor.store(chosen->actor,std::memory_order_release);
        }
        const auto& before=fixture.beforeTask;
        auto* actor=sRandomPlayerbotMgr.GetPlayerBot(before.actor);
        if (!actor || !actor->IsInWorld() || actor->IsBeingTeleported()) {fixture.blocker="recovery_actor_unavailable";return;}
        const bool original=state->enforceEffects.exchange(true,std::memory_order_acq_rel);
        struct Restore {State& state;bool original;~Restore(){state.enforceEffects.store(original,std::memory_order_release);}} restore{*state,original};
        if (!fixture.step) {
            ProfessionHistory history;
            if (!ReadProfessionHistory(before.actor,before.id,before.revision,history,fixture.blocker)) return;
            require(history.complete && !history.unresolvedOperation && history.attempts.size()==1 &&
                history.attempts.front().receipt.state==OperationState::Verified && history.attempts.front().nativeEffectVerified,
                "restart_reads_original_verified_native_operation");
            fixture.operation=history.attempts.front().receipt.id;
            UnsettledClaimBatch batch;ProfessionJob job;
            require(state->resources.ReadUnsettled(before.id,batch,fixture.blocker) && batch.complete && !batch.claims.empty(),
                "restart_retains_exact_outstanding_claims");
            fixture.claims=batch.claims;
            require(DecodeProfessionJob(before.checkpoint.data,job,fixture.blocker) &&
                ReadNativeCraftFrame(*actor,job,fixture.before,fixture.blocker),"restart_reads_actual_native_goods");
            require(!AcquireSavedTask(before.id,before.revision,Mask(Effect::Spell),60000,"isolated_recovery_fixture").Permitted(),
                "restart_old_context_does_not_authorize_another_cast");
            require(SettleProfessionJob(before.actor,before.id,before.revision,fixture.receipt).code==AdmissionCode::StaleContext,
                "ordinary_completion_rejects_stale_boot");
            const auto result=ReconcileProfessionCompletion(before.actor,before.id,before.revision,fixture.receipt);
            fixture.blocker=result.blocker;
            if (result.code==AdmissionCode::NotReady) return;
            require(result.code==AdmissionCode::Pending,"restart_native_reconciliation_queued");
            require(ReconcileProfessionCompletion(before.actor,before.id,before.revision,fixture.receipt).code==AdmissionCode::Pending,
                "restart_duplicate_shares_pending_receipt");
            for (const auto& claim : fixture.claims) {
                const auto* actual=state->resources.Inspect(claim.id);
                require(actual && SameResourceClaim(*actual,claim),"restart_protection_retained_until_ack");
            }
            fixture.step=1;return;
        }
        const auto saved=ReadSavedTask(before.id);
        require(saved && saved->revision==before.revision+1 && saved->phase==Phase::Completed &&
            saved->context.boot==state->boot && saved->checkpoint.data==before.checkpoint.data,
            "restart_completes_same_accepted_job_with_fresh_native_context");
        require(ReconcileProfessionCompletion(before.actor,before.id,before.revision,fixture.receipt).code==AdmissionCode::Saved,
            "restart_saved_receipt_prevents_duplicate_completion");
        for (const auto& claim : fixture.claims) {
            auto released=claim;released.state="released";++released.revision;
            const auto* actual=state->resources.Inspect(claim.id);
            require(actual && SameResourceClaim(*actual,released),"restart_releases_only_exact_protected_surplus_and_output");
        }
        UnsettledClaimBatch remaining;ProfessionJob job;
        require(state->resources.ReadUnsettled(before.id,remaining,fixture.blocker) && remaining.complete && remaining.claims.empty(),
            "restart_has_no_hidden_unsettled_claims");
        require(DecodeProfessionJob(before.checkpoint.data,job,fixture.blocker) &&
            ReadNativeCraftFrame(*actor,job,fixture.after,fixture.blocker) && SameCraftFrame(fixture.before,fixture.after),
            "restart_does_not_change_items_gold_or_skill");
        require(!AcquireSavedTask(before.id,saved->revision,Mask(Effect::Spell),60000,"isolated_recovery_fixture").Permitted(),
            "restart_completed_job_cannot_run_again");
        fixture.blocker.clear();emit(true,"");
    } catch (const std::exception& error) {emit(false,error.what());}
}
