// Copied-realm observation only. Enable the existing producers and due queue
// together, with normal AI running. No actor, recipe, inventory or task grants.
// Ending an observation interval never constitutes proof of gameplay success.
template<class FixtureState>
void RunIsolatedServicesObservation(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto& f=state.guildDepositFixture;
    auto report=[&](const std::string& phase,const std::string& blocker) {
        auto result=f.setup;result.put("suite","activity-services-observe-v1");result.put("boot",state.boot);
        result.put("phase",phase);result.put("blocker",blocker);result.put("observed_at_ms",NowMs());
        result.put("test_only",true);result.put("isolated_active_policy_override",true);
        result.put("ordinary_ai_unpaused",true);result.put("fixture_grants",0);
        result.put("fixture_relocations",0);result.put("manual_admission_calls",0);
        result.put("native_effect_proof_required",true);result.put("production_proof",false);
        result.put("online_bots",sRandomPlayerbotMgr.GetAllBots().size());
        result.put("native_dispatches",state.nativeDispatches);result.put("native_result_receipts",state.nativeOutcomes);
        result.put("verified_native_results",state.nativeVerifiedResults);result.put("task_admissions",state.taskAdmissions);
        result.put("pending_writes",state.pending.size());result.put("pending_operations",state.operations.size());
        boost::property_tree::ptree tasks;unsigned count=0;
        for(const auto& row:state.cache) {
            const auto& task=row.second;
            if(task.mode!=Mode::Active || !sRandomPlayerbotMgr.GetPlayerBot(task.actor))continue;
            if(++count>128)continue;
            boost::property_tree::ptree item;item.put("task",task.id);item.put("actor",task.actor);
            item.put("source",task.source);item.put("phase",Name(task.phase));item.put("revision",task.revision);
            item.put("step",task.checkpoint.step);item.put("created_at_ms",task.createdAtMs);
            const auto blocked=state.executionBlockers.find(task.id);
            item.put("blocker",blocked==state.executionBlockers.end()?task.checkpoint.blocker:blocked->second);
            // Private, value-only diagnostic for this marked copied realm.
            // A generic travel blocker must not hide the exact failing native
            // destination/leg. Reading this never installs or advances a route.
            ServiceDestination service;
            auto* bot=sRandomPlayerbotMgr.GetPlayerBot(task.actor);
            if(bot && bot->GetPlayerbotAI() && ParseServiceStep(task.checkpoint.step,service)) {
                boost::property_tree::ptree route;
                route.put("actor_map",bot->GetMapId());route.put("actor_x",bot->GetPositionX());
                route.put("actor_y",bot->GetPositionY());route.put("actor_z",bot->GetPositionZ());
                route.put("alive",bot->IsAlive());route.put("combat",bot->IsInCombat());
                route.put("transport",bool(bot->GetTransport()));route.put("taxi",bot->IsTaxiFlying());
                auto* context=bot->GetPlayerbotAI()->GetAiObjectContext();
                auto* target=context->GetValue<ai::TravelTarget*>("travel target")->Get();
                if(target && target->GetDestination() && target->GetPosition()) {
                    const auto* position=target->GetPosition();
                    route.put("entry",target->GetEntry());route.put("purpose",uint32(target->GetDestination()->GetPurpose()));
                    route.put("revision",target->GetRouteRevision());route.put("status",uint32(target->GetStatus()));
                    route.put("map",position->getMapId());route.put("x",position->getX());
                    route.put("y",position->getY());route.put("z",position->getZ());
                    route.put("distance",target->Distance(bot));
                    route.put("owned",sPlayerbotOrganicEconomy.HasOwnedServiceRoute(task.actor,uint32(target->GetDestination()->GetPurpose())));
                }
                const auto& path=context->GetValue<ai::LastMovement&>("last movement")->Get().lastPath.getPath();
                route.put("path_nodes",path.size());
                if(!path.empty()) {
                    route.put("next_leg_type",uint32(path.front().type));route.put("next_leg_entry",path.front().entry);
                    route.put("next_leg_map",path.front().point.getMapId());route.put("next_leg_x",path.front().point.getX());
                    route.put("next_leg_y",path.front().point.getY());route.put("next_leg_z",path.front().point.getZ());
                }
                item.add_child("native_route",route);
            }
            tasks.push_back({"",item});
        }
        result.put("task_snapshot_truncated",count>128);result.add_child("tasks",tasks);
        std::ofstream out("/isolated/evidence/activity-services-"+state.boot+".json");out<<Json(result);out.close();
        if(!out) {
            state.effective=Mode::Observe;state.fixtureFinished=true;
            sLog.outError("Isolated service observation stopped: evidence unavailable");return;
        }
        sLog.outString("Living isolated service observation: %s",Json(result).c_str());
    };
    if(sWorld.GetActiveAndQueuedSessionCount() || sRandomPlayerbotMgr.GetAllBots().size()>50) {
        // Stop new admission, but keep native journal reconciliation intact.
        state.effective=Mode::Observe;report("stopped","isolated_population_or_human_safety_limit");
        state.fixtureFinished=true;return;
    }
    if(!state.loaded || !state.incoming.empty() || !state.resources.Protection().ready)return;
    if(!f.started) {
        f.started=true;f.deadline=NowMs()+1200000;
        f.setup.put("started_at_ms",NowMs());f.setup.put("duration_seconds",1200);
        f.setup.put("initial_native_dispatches",state.nativeDispatches);
        f.setup.put("initial_verified_results",state.nativeVerifiedResults);
        f.setup.put("initial_task_admissions",state.taskAdmissions);
    }
    if(NowMs()>=f.deadline) {
        state.effective=Mode::Observe;report("observation_finished","");state.fixtureFinished=true;return;
    }
    state.effective=Mode::Active;state.enforceEffects.store(true,std::memory_order_release);
    if(!f.diagnostics || NowMs()>=f.setup.template get<uint64_t>("next_sample_at_ms",0)) {
        ++f.diagnostics;f.setup.put("next_sample_at_ms",NowMs()+30000);report("observing","");
    }
}
