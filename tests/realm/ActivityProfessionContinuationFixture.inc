// Resume one explicitly named copied job through its ordinary finite runner.
// Unlike effect-specific assertions, this does not assume the next step is a
// cast: legitimate capacity, travel, waiting and settlement remain real steps.
// No grants, repositioning, new task, clock advance or native-effect bypass.
// Included inside the existing copied recovery entry point, never production.
    MANGOS_ASSERT(OnWorldThread());RequireIsolatedNativeFixtureEnvironment();
    if(state->fixtureFinished || NowMs()<state->fixtureNext) return;
    state->fixtureNext=NowMs()+1000;
    if(sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty() || !state->resources.Protection().ready) return;
    auto& fixture=state->professionRecoveryFixture;
    const char* named=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
    const std::string taskId=named?named:"";
    if(!fixture.deadline) fixture.deadline=NowMs()+600000;
    auto emit=[&](bool passed,const std::string& failure) {
        boost::property_tree::ptree report;
        report.put("suite","activity-profession-continue-v1");report.put("test_only",true);
        report.put("boot",state->boot);report.put("task",taskId);report.put("actor",fixture.beforeTask.actor);
        report.put("previous_boot",fixture.beforeTask.context.boot);report.put("previous_revision",fixture.beforeTask.revision);
        report.put("source_key",fixture.beforeTask.sourceKey);
        report.put("outcome",passed?"passed":"failed");report.put("failure",failure);report.put("blocker",fixture.blocker);
        report.put("ordinary_profession_runner",true);report.put("general_execution_enabled",false);
        report.put("new_tasks",0);report.put("item_grants",0);report.put("money_grants",0);report.put("skill_grants",0);
        report.put("spell_grants",0);report.put("fixture_relocations",0);report.put("process_crash_proof",false);
        report.put("money_before",fixture.before.money);report.put("money_after",fixture.after.money);
        report.put("skill_before",fixture.before.skill);report.put("skill_after",fixture.after.skill);
        auto stacks=[](const CraftFrame& frame) {
            boost::property_tree::ptree rows;
            for(const auto& stack:frame.stacks) {
                boost::property_tree::ptree row;row.put("guid",stack.guid);row.put("entry",stack.entry);
                row.put("quantity",stack.count);row.put("bag",stack.bagGuid);row.put("slot",uint32_t(stack.slot));rows.push_back({"",row});
            }
            return rows;
        };
        report.add_child("before_items",stacks(fixture.before));report.add_child("after_items",stacks(fixture.after));
        boost::property_tree::ptree claims;
        for(const auto& claim:fixture.claims) {
            boost::property_tree::ptree row;row.put("id",claim.id);row.put("revision",claim.revision);
            row.put("guid",claim.itemGuid);row.put("entry",claim.itemEntry);row.put("quantity",claim.quantity);
            row.put("copper",claim.copper);row.put("location",claim.location);row.put("state",claim.state);claims.push_back({"",row});
        }
        report.add_child("prior_claims",claims);report.add_child("checks",fixture.checks);
        report.add_child("transitions",fixture.continuationTrace);
        if(fixture.beforeTask.actor) report.put("actor_diagnostic",ActorJson(fixture.beforeTask.actor));
        std::ofstream output("/isolated/evidence/activity-profession-continue-"+state->boot+".json");
        output<<Json(report);output.close();
        if(!output) sLog.outError("Living isolated continuation evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        if(passed) state->gameplayFixtureActor.store(0,std::memory_order_release);
        state->fixtureFinished=true;
    };
    auto require=[&](bool ok,const char* name) {
        boost::property_tree::ptree row;row.put("case",name);row.put("passed",ok);fixture.checks.push_back({"",row});
        if(!ok) throw std::runtime_error(name);
    };
    try {
        if(!IsUuid(taskId)) {emit(false,"explicit_copied_task_uuid_required");return;}
        if(NowMs()>fixture.deadline) {emit(false,"continuation_deadline_with_exact_step");return;}
        const auto saved=ReadSavedTask(taskId);
        if(!saved || saved->mode!=Mode::Active || !saved->accepted || saved->source!="profession_job" ||
            saved->sourceKey.find("isolated_craft_v1:")!=0 || saved->root!=taskId || !saved->parent.empty()) {
            emit(false,"explicit_copied_profession_job_required");return;
        }
        if(!fixture.beforeTask.actor) {
            require(!Terminal(saved->phase) && saved->context.boot!=state->boot,"prior_nonterminal_job_retained");
            fixture.beforeTask=*saved;
            state->gameplayFixtureActor.store(saved->actor,std::memory_order_release);
        }
        auto* actor=sRandomPlayerbotMgr.GetPlayerBot(saved->actor);
        if(!actor) {
            if(!state->supportFixtureRequestedActor && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                state->supportFixtureRequestedActor=saved->actor;sRandomPlayerbotMgr.AddRandomBot(saved->actor);
            }
            fixture.blocker="continuation_native_login_pending";return;
        }
        if(!actor->IsInWorld() || !actor->GetPlayerbotAI() || NativeSafety(actor) || actor->GetGroup() ||
            actor->GetTradeData() || actor->GetMap()->IsDungeon() || DefersNativeSave(saved->actor)) {
            fixture.blocker="continuation_native_safety_wait";return;
        }
        ProfessionJob job;
        if(!DecodeProfessionJob(saved->checkpoint.data,job,fixture.blocker)) {emit(false,"saved_profession_intent_invalid");return;}
        if(!fixture.step) {
            UnsettledClaimBatch claims;
            require(state->resources.ReadUnsettled(taskId,claims,fixture.blocker) && claims.complete,
                "continuation_reads_exact_prior_claims");
            fixture.claims=claims.claims;
            require(ReadNativeCraftFrame(*actor,job,fixture.before,fixture.blocker),"continuation_reads_original_native_possessions");
            fixture.step=1;
        }
        if(saved->phase==Phase::Completed) {
            require(ReadNativeCraftFrame(*actor,job,fixture.after,fixture.blocker),"continuation_reads_final_native_possessions");
            UnsettledClaimBatch claims;
            require(state->resources.ReadUnsettled(taskId,claims,fixture.blocker) && claims.complete && claims.claims.empty(),
                "completed_job_has_no_unsettled_claims");
            require(saved->checkpoint.step=="profession_completed" && saved->checkpoint.blocker.empty(),
                "ordinary_runner_verified_completion");
            fixture.blocker.clear();emit(true,"");return;
        }
        if(Terminal(saved->phase)) {emit(false,"job_ended_without_completion");return;}
        const bool original=state->enforceEffects.exchange(true,std::memory_order_acq_rel);
        struct Restore {State& state;bool original;~Restore(){state.enforceEffects.store(original,std::memory_order_release);}} restore{*state,original};
        fixture.blocker=AdvanceProfessionJob(saved->actor,taskId).blocker;
        const auto stamp=std::to_string(saved->revision)+":"+fixture.blocker;
        if(stamp!=fixture.lastCheckpoint) {
            if(fixture.continuationTrace.size()>=256) {emit(false,"continuation_transition_quota");return;}
            boost::property_tree::ptree row;row.put("revision",saved->revision);row.put("phase",Name(saved->phase));
            row.put("step",saved->checkpoint.step);row.put("blocker",fixture.blocker);row.put("at_ms",NowMs());
            row.put("x",actor->GetPositionX());row.put("y",actor->GetPositionY());row.put("map",actor->GetMapId());
            sLog.outString("Living isolated continuation step: task=%s %s",taskId.c_str(),Json(row).c_str());
            fixture.continuationTrace.push_back({"",row});fixture.lastCheckpoint=stamp;
        }
    } catch(const std::exception& error) {emit(false,error.what());}
