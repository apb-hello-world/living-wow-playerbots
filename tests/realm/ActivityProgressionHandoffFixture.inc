// Test-only native observer replay. The saved maintenance task is an ownership
// fixture, not proof of completed service work. Temporary group membership and
// watchdog age are explicit fixtures; no possessions or gameplay credit grants.
#include "playerbot/PlayerbotChatDirector.h"
namespace {
template<class FixtureState>
void RunIsolatedProgressionHandoff(FixtureState& state) {
    struct Fixture { uint32_t actor=0,human=0;uint64_t deadline=0;bool login=false;
        unsigned stage=0;TaskRequest request;boost::property_tree::ptree checks; };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[&](bool ok,const std::string& name){
        boost::property_tree::ptree check;check.put("case",name);check.put("passed",ok);
        f.checks.push_back({"",check});if(!ok)throw std::runtime_error(name);
    };
    auto finish=[&](bool passed,const std::string& reason){
        boost::property_tree::ptree report;report.put("suite","activity-progression-handoff-v1");
        report.put("boot",state.boot);report.put("actor",f.actor);report.put("offline_human",f.human);
        report.put("task",f.request.task.id);report.put("passed",passed);report.put("reason",reason);
        report.put("test_only",true);report.put("native_service_completion_claimed",false);
        report.put("item_money_skill_grants",0);report.add_child("checks",f.checks);
        std::ofstream file("/isolated/evidence/activity-progression-handoff-"+state.boot+".json");
        file<<Json(report);file.close();state.fixtureFinished=true;state.gameplayFixtureActor.store(0);
        sLog.outString("Living isolated progression handoff: %s",Json(report).c_str());
    };
    try {
        if(!f.deadline) {
            f.deadline=NowMs()+240000;
            const auto* actor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");
            const auto* human=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(actor && human,"explicit_fixture_characters");
            f.actor=std::stoul(actor);f.human=std::stoul(human);
            require(f.actor && f.human && f.actor!=f.human,"distinct_fixture_characters");
        }
        if(NowMs()>f.deadline){finish(false,"fixture_deadline");return;}
        if(!state.loaded || state.ioPending || !state.pending.empty() || !state.incoming.empty())return;
        if(sWorld.GetActiveAndQueuedSessionCount())return;
        if(!f.login){sRandomPlayerbotMgr.AddRandomBot(f.actor);f.login=true;return;}
        Player* bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(!bot || !bot->IsInWorld() || !bot->GetPlayerbotAI() || NativeSafety(bot))return;
        if(!state.bindings.count(f.actor))return;
        auto& coordinator=sLivingActivityCoordinator;
        auto admit=[&](const std::string& name){
            const auto result=coordinator.SubmitTask(f.request);
            require(result.code==AdmissionCode::Pending,name+":"+Name(result.code)+":"+result.blocker);
        };
        const bool oldEnforcement=state.enforceEffects.exchange(true);
        struct Restore { std::function<void()> fn;~Restore(){fn();} } restore{[&]{state.enforceEffects.store(oldEnforcement);}};
        if(f.stage==0) {
            require(!bot->GetGroup(),"actor_initially_ungrouped");
            require(!state.authority.Read(f.actor).lease.actor,"actor_initially_unowned");
            state.gameplayFixtureActor.store(f.actor);
            auto& task=f.request.task;task.actor=f.actor;
            task.source="isolated_progression_fixture";task.sourceKey=state.boot;
            task.id=task.root=SourceId(task.source,task.sourceKey);
            task.context=ReadNativeContext(*bot,state.policyRevision,state.boot);
            task.createdAtMs=task.updatedAtMs=NowMs();task.kind=Kind::Maintenance;
            task.priority=Priority::Delivery;task.mode=Mode::Active;task.phase=Phase::Queued;
            task.checkpoint.step="progression_handoff_probe";f.request.receipt=NewId();
            admit("durable_fixture_queued");
            f.stage=1;return;
        }
        const auto saved=coordinator.ReadSavedTask(f.request.task.id);
        if(!saved || saved->revision!=f.request.task.revision)return;
        if(f.stage==1) {
            f.request.task=*saved;f.request.expectedRevision=saved->revision;++f.request.task.revision;
            f.request.task.phase=Phase::Preparing;f.request.task.updatedAtMs=NowMs();f.request.receipt=NewId();
            admit("durable_preparation_queued");
            f.stage=2;return;
        }
        if(f.stage==3){require(saved->phase==Phase::Cancelled,"fixture_cancel_acknowledged");finish(true,"");return;}
        const auto grant=coordinator.AcquireSavedTask(saved->id,saved->revision,
            Mask(Effect::Movement)|Mask(Effect::TravelTarget),60000,"isolated_progression_fixture");
        require(grant.Permitted(),"saved_service_lease_acquired:"+grant.blocker);
        Restore release{[&]{coordinator.ReleaseTaskLease(grant.authority.lease);}};
        require(coordinator.MovementCommitmentBlocksRecovery(f.actor),"saved_owner_visible_to_watchdog");
        auto probe=[&](const std::string& name){
            boost::property_tree::ptree data;std::istringstream input(sPlayerbotChatDirector.IsolatedRecoveryProbe(bot));
            boost::property_tree::read_json(input,data);
            require(data.get<unsigned>("step")==0 && data.get<std::string>("result")=="yielded_to_activity_owner",
                name+"_yields_recovery");
            require(!data.get<unsigned>("failure_streak") && !data.get<unsigned>("objective_failures") &&
                !data.get<unsigned>("attempts"),name+"_no_false_failure");
            require(data.get<uint64_t>("route_before")==data.get<uint64_t>("route_after"),name+"_native_route_unchanged");
        };
        probe("saved_service");
        require(coordinator.ReleaseTaskLease(grant.authority.lease).code==AuthorityCode::Released,"service_released");
        require(!coordinator.MovementCommitmentBlocksRecovery(f.actor),"unowned_progression_available");
        ObjectGuid human(HIGHGUID_PLAYER,f.human);std::string name;
        require(!sRandomPlayerbotMgr.IsRandomBot(f.human) && !sObjectMgr.GetPlayer(human) &&
            sObjectMgr.GetPlayerNameByGUID(human,name),"existing_offline_human");
        auto* group=new Group();
        require(group->Create(bot->GetObjectGuid(),bot->GetName()),"temporary_native_group_created");
        sObjectMgr.AddGroup(group);
        Restore disband{[&]{group->Disband(true);sObjectMgr.RemoveGroup(group);delete group;}};
        require(group->AddMember(human,name.c_str()),"offline_human_native_roster_added");
        probe("offline_human_party");
        f.request.task=*saved;f.request.expectedRevision=saved->revision;++f.request.task.revision;
        f.request.task.phase=Phase::Cancelled;f.request.task.updatedAtMs=NowMs();f.request.receipt=NewId();
        f.request.task.checkpoint.blocker="isolated_probe_finished";
        // This isolated ownership probe acquired no goods and dispatched no
        // operation. Use the existing guarded cleanup write; normal admission
        // correctly refuses callers manufacturing terminal outcomes.
        state.Queue(f.request.task,f.request.expectedRevision,"isolated_fixture_cleanup");
        state.nextWork=0;
        f.stage=3;
    } catch(const std::exception& error){finish(false,error.what());}
}
}
