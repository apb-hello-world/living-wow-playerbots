// Copied realm only: one exact native blacksmithing job, ordinary due queue.
// Up to four recorded copper bars may be supplied as a test input. No tool,
// gold, skill, spell, equipment or position is granted or rewritten.
namespace {
template<class FixtureState>
void RunIsolatedToolFixture(FixtureState& state) {
    if (state.fixtureFinished || NowMs()<state.fixtureNext) return;
    state.fixtureNext=NowMs()+500;auto& f=state.toolFixture;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool passed,const std::string& why,Player* actor) {
        auto report=f.setup;report.put("suite","activity-tool-v1");report.put("boot",state.boot);
        report.put("task",f.task);report.put("actor",f.actor);report.put("test_only",true);
        report.put("outcome",passed?"passed":"failed");report.put("failure",why);report.put("blocker",f.blocker);
        report.put("ordinary_due_queue",true);report.put("independent_audit_required",true);
        if (actor && f.started) {
            report.put("money_after",actor->GetMoney());report.put("skill_after",actor->GetSkillValuePure(f.job.skill));
            report.put("tool_after",actor->GetItemCount(5956,true));report.put("reagent_after",actor->GetItemCount(2840,true));
            report.put("output_after",actor->GetItemCount(f.job.outputEntry,true));
            report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
        }
        std::ofstream out("/isolated/evidence/activity-tool-"+state.boot+".json");out<<Json(report);out.close();
        state.fixtureFinished=true;if(!out)sLog.outError("Living tool evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* actor=nullptr;
    try {
        if (!f.actor) {
            const auto* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const std::string id=raw?raw:"";
            require(!id.empty() && id.size()<=9 && id.find_first_not_of("0123456789")==std::string::npos,"explicit_tool_actor_required");
            f.actor=uint32_t(std::stoul(id));require(f.actor!=0,"explicit_tool_actor_required");
            f.task=SourceId("profession_job","isolated_tool_v1:"+state.boot);
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
        }
        if (sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready) return;
        if (!f.deadline) f.deadline=NowMs()+600000;
        actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if (NowMs()>f.deadline) {emit(false,"tool_deadline_with_preserved_task",actor);return;}
        if (!actor) {
            if (!f.requested && sRandomPlayerbotMgr.GetAllBots().size()<49) {f.requested=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
            f.blocker="tool_native_login_pending";return;
        }
        if (!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() || actor->GetTradeData() ||
            actor->GetMap()->IsDungeon()) {f.blocker="tool_native_safety_wait";return;}
        if (!f.started) {
            require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer() && actor->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"copied_native_blacksmith_required");
            for (const auto& row:state.cache) if (row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_tool_actor_obligation_preserved");
            require(BuildNativeSkillGainJob(*actor,2662,f.job,f.blocker),"native_tool_recipe:"+f.blocker);
            require(f.job.skill==SKILL_BLACKSMITHING && f.job.reagents==std::vector<ProfessionReagent>({{2840,4}}),"exact_tool_fixture_recipe");
            require(!actor->HasItemTotemCategory(13) && !actor->GetItemCount(5956,true),"native_tool_really_missing");
            const uint32_t owned=actor->GetItemCount(2840,true);
            require(owned==0 || owned>=4,"partial_test_stock_requires_separate_case");
            f.setup.put("actor",f.actor);f.setup.put("task",f.task);f.setup.put("job",EncodeProfessionJob(f.job));
            f.setup.put("money_before",actor->GetMoney());f.setup.put("skill_before",actor->GetSkillValuePure(f.job.skill));
            f.setup.put("tool_entry",5956);f.setup.put("tool_before",0);f.setup.put("tool_grants",0);
            f.setup.put("reagent_before",owned);f.setup.put("reagent_grants",owned?0:4);
            f.setup.put("output_before",actor->GetItemCount(f.job.outputEntry,true));
            f.setup.put("money_grants",0);f.setup.put("skill_grants",0);f.setup.put("spell_grants",0);
            f.setup.put("equipment_grants",0);f.setup.put("fixture_relocations",0);
            std::ofstream intent("/isolated/evidence/tool-setup-intent-"+f.task+".json");intent<<Json(f.setup);intent.close();
            require(bool(intent),"tool_fixture_intent_saved");
            if (!owned) {
                ItemPosCountVec positions;require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,2840,4)==EQUIP_ERR_OK,"tool_fixture_input_capacity");
                auto* item=actor->StoreNewItem(positions,2840,true);require(item!=nullptr,"tool_fixture_input_supplied");
                f.setup.put("fixture_reagent_guid",item->GetGUIDLow());actor->SaveToDB();
            }
            std::ofstream setup("/isolated/evidence/tool-setup-"+f.task+".json");setup<<Json(f.setup);setup.close();
            require(bool(setup),"tool_fixture_setup_saved");
            TaskRequest request;auto& task=request.task;task.id=task.root=f.task;task.actor=f.actor;
            task.source="profession_job";task.sourceKey="isolated_tool_v1:"+state.boot;task.kind=Kind::Profession;
            task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Delivery;
            task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeProfessionJob(f.job);request.receipt=NewId();
            state.enforceEffects.store(true,std::memory_order_release);
            const auto admitted=sLivingActivityCoordinator.SubmitTask(request);
            require(admitted.code==AdmissionCode::Pending,"tool_task_admission:"+admitted.blocker);
            f.started=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if (!saved) {f.blocker="tool_admission_pending";return;}
        const auto found=state.executionBlockers.find(f.task);
        f.blocker=found==state.executionBlockers.end()?saved->checkpoint.blocker:found->second;
        const auto key=std::to_string(saved->revision)+":"+f.blocker;
        if (key!=f.lastDiagnostic && f.diagnostics<128) {
            f.lastDiagnostic=key;++f.diagnostics;
            sLog.outString("Living tool progress: actor=%u task=%s revision=%llu phase=%s step=%s blocker=%s",f.actor,f.task.c_str(),
                static_cast<unsigned long long>(saved->revision),Name(saved->phase),saved->checkpoint.step.c_str(),f.blocker.c_str());
        }
        require(!Terminal(saved->phase) || saved->phase==Phase::Completed,"tool_job_ended_without_verified_success");
        if (saved->phase!=Phase::Completed) return;
        require(actor->GetItemCount(5956,true)==1 && actor->GetSkillValuePure(f.job.skill)>f.setup.template get<uint32_t>("skill_before"),
            "tool_owned_and_craft_skill_changed");
        require(!sLivingActivityCoordinator.DefersNativeSave(f.actor) && !state.executionTimes.count(f.task),"tool_native_save_and_queue_settled");
        f.blocker.clear();emit(true,"",actor);
    } catch (const std::exception& error) {emit(false,error.what(),actor);}
}
}
