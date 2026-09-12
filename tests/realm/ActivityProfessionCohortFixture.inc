// A bounded copied-only cohort, not another production scheduler. Four jobs
// are admitted at a time; the ordinary coordinator due queue executes them.
// Test setup is an independently recorded OFFLINE fixture. This runner grants
// no items, money, skills, spells, completion flags, or service arrival.
namespace {
template<class FixtureState>
void RunIsolatedProfessionCohort(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool passed,const std::string& why) {
        boost::property_tree::ptree report,rows;
        report.put("suite","activity-profession-cohort-v1");report.put("boot",state.boot);
        report.put("cohort",state.professionCohortId);report.put("outcome",passed?"passed":"failed");
        report.put("failure",why);report.put("test_only",true);report.put("independent_audit_required",true);
        report.put("ordinary_due_queue",true);report.put("maximum_active_jobs",4);
        report.put("general_execution_enabled",state.effective==Mode::Active);
        report.add_child("fixture_setup",state.professionCohortSetup);
        for(auto& f:state.professionCohort) {
            boost::property_tree::ptree row;row.put("actor",f.actor);row.put("recipe",f.recipe);
            row.put("skill",f.skill);row.put("task",f.task);row.put("blocker",f.blocker);
            row.put("money_before",f.moneyBefore);row.put("skill_before",f.skillBefore);
            row.put("output_before",f.outputBefore);row.put("output",f.output);row.put("finished",f.finished);
            if(const auto task=sLivingActivityCoordinator.ReadSavedTask(f.task)) {
                row.put("phase",Name(task->phase));row.put("revision",task->revision);row.put("step",task->checkpoint.step);
            }
            if(auto* actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor)) {
                row.put("money_after",actor->GetMoney());row.put("skill_after",actor->GetSkillValuePure(f.skill));
                row.put("output_after",actor->GetItemCount(f.output,true));
            }
            rows.push_back({"",row});
        }
        report.add_child("cases",rows);
        std::ofstream out("/isolated/evidence/activity-profession-cohort-"+state.boot+".json");
        out<<Json(report);out.close();
        state.fixtureFinished=true;
        if(!out)sLog.outError("Living cohort evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        // Retain all fixture actor holds for the independent native SQL audit.
        // An incomplete job survives a test deadline and can resume on restart.
    };
    try {
        if(state.professionCohortId.empty()) {
            std::ifstream in("/isolated/evidence/profession-cohort-setup-v1.json",std::ios::ate);
            require(in.good() && in.tellg()>0 && in.tellg()<=1024*1024,"bounded_offline_cohort_setup_required");
            in.seekg(0);boost::property_tree::ptree setup;boost::property_tree::read_json(in,setup);
            const auto id=setup.get<std::string>("cohort");
            require(IsUuid(id) && setup.get<std::string>("environment")=="isolated-migration" &&
                setup.get<std::string>("outcome")=="prepared" && setup.get<uint32_t>("moneyGrants")==0 &&
                setup.get<uint32_t>("skillGrants")==0,"verified_copied_cohort_setup_required");
            const auto& cases=setup.get_child("cases");
            require(!cases.empty() && cases.size()<=20,"cohort_population_bound");
            std::set<uint32_t> actors;
            for(const auto& value:cases) {
                const auto& row=value.second;typename FixtureState::ProfessionCohortCase f;
                f.actor=row.get<uint32_t>("actor");f.recipe=row.get<uint32_t>("recipe");f.skill=row.get<uint32_t>("skill");
                f.moneyBefore=row.get<uint32_t>("moneyBefore");f.skillBefore=row.get<uint32_t>("skillBefore");
                f.output=row.get<uint32_t>("output");f.outputBefore=row.get<uint32_t>("outputBefore");
                require(f.actor && f.recipe && f.skill && f.output && actors.insert(f.actor).second,"unique_native_cohort_actors");
                f.task=SourceId("profession_job","isolated_profession_cohort_v1:"+id+":"+std::to_string(f.actor));
                state.professionCohort.push_back(f);
            }
            // Publish only after the whole bounded setup has passed validation.
            for(size_t i=0;i<state.professionCohort.size();++i)
                state.professionCohortActors[i].store(state.professionCohort[i].actor,std::memory_order_release);
            state.professionCohortId=id;state.professionCohortSetup=setup;
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!state.professionCohortDeadline)state.professionCohortDeadline=NowMs()+1800000;
        if(NowMs()>state.professionCohortDeadline){emit(false,"cohort_deadline_with_preserved_tasks");return;}
        state.enforceEffects.store(true,std::memory_order_release);
        unsigned active=0,finished=0;
        for(auto& f:state.professionCohort) {
            auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
            if(saved)f.started=true;
            if(f.finished){++finished;continue;}
            if(!f.started && active>=4)continue;
            ++active;
            auto* actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
            if(!actor) {
                if(!f.requested && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                    f.requested=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);
                }
                f.blocker="cohort_native_actor_login_pending";continue;
            }
            require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer(),"copied_native_bot_required");
            if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() ||
                actor->GetTradeData() || actor->GetMap()->IsDungeon()) {f.blocker="cohort_native_safety_wait";continue;}
            if(!f.started) {
                require(actor->GetMoney()==f.moneyBefore && actor->GetSkillValuePure(f.skill)==f.skillBefore &&
                    actor->GetItemCount(f.output,true)==f.outputBefore,"cohort_starting_native_state_changed");
                ProfessionJob job;
                require(BuildNativeSkillGainJob(*actor,f.recipe,job,f.blocker),"cohort_native_recipe:"+f.blocker);
                require(job.skill==f.skill && job.outputEntry==f.output,"cohort_native_recipe_identity");
                TaskRequest request;auto& task=request.task;task.id=task.root=f.task;task.actor=f.actor;
                task.source="profession_job";task.sourceKey="isolated_profession_cohort_v1:"+state.professionCohortId+":"+std::to_string(f.actor);
                task.kind=Kind::Profession;task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Delivery;
                task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);
                task.createdAtMs=task.updatedAtMs=NowMs();task.checkpoint.step="profession_prepare";
                task.checkpoint.data=EncodeProfessionJob(job);request.receipt=NewId();
                const auto admission=sLivingActivityCoordinator.SubmitTask(request);f.blocker=admission.blocker;
                require(admission.code==AdmissionCode::Pending,"cohort_task_admission:"+f.blocker);
                f.started=true;continue;
            }
            if(!saved){f.blocker="cohort_task_admission_pending";continue;}
            const auto found=state.executionBlockers.find(f.task);
            f.blocker=found==state.executionBlockers.end()?saved->checkpoint.blocker:found->second;
            const auto key=std::to_string(saved->revision)+":"+f.blocker;
            if(key!=f.lastDiagnostic && f.diagnostics<160) {
                ++f.diagnostics;f.lastDiagnostic=key;
                sLog.outString("Living cohort progress: actor=%u task=%s revision=%llu phase=%s step=%s blocker=%s",f.actor,
                    f.task.c_str(),static_cast<unsigned long long>(saved->revision),Name(saved->phase),saved->checkpoint.step.c_str(),f.blocker.c_str());
            }
            require(!Terminal(saved->phase) || saved->phase==Phase::Completed,"cohort_task_ended_without_success");
            if(saved->phase==Phase::Completed) {
                require(actor->GetSkillValuePure(f.skill)>f.skillBefore && actor->GetItemCount(f.output,true)>f.outputBefore &&
                    actor->GetMoney()<f.moneyBefore,"cohort_native_paid_craft_required");
                require(!sLivingActivityCoordinator.DefersNativeSave(f.actor),"cohort_native_save_pending");
                f.finished=true;f.blocker.clear();++finished;--active;
            }
        }
        if(finished==state.professionCohort.size())emit(true,"");
    } catch(const std::exception& error){emit(false,error.what());}
}
}
