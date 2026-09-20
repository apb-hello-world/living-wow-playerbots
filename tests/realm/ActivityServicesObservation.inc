// Copied-realm observation only. Enable the existing producers and due queue
// together, with normal AI running. No actor, recipe, inventory or task grants.
// Ending an observation interval never constitutes proof of gameplay success.
#include "GuildObservationCohort.h"
template<class FixtureState>
void RunIsolatedServicesObservation(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto& f=state.guildDepositFixture;
    auto report=[&](const std::string& phase,const std::string& blocker) {
        auto result=f.setup;result.put("suite","activity-services-observe-v1");result.put("boot",state.boot);
        result.put("phase",phase);result.put("blocker",blocker);result.put("observed_at_ms",NowMs());
        result.put("test_only",true);result.put("isolated_active_policy_override",true);
        result.put("ordinary_ai_unpaused",true);
        result.put("fixture_grants",f.setup.template get<uint32_t>("source_fixture_corpse_grants",0));
        result.put("fixture_relocations",0);
        result.put("manual_admission_calls",f.setup.template get<uint32_t>("source_fixture_admission_calls",0));
        result.put("native_effect_proof_required",true);result.put("production_proof",false);
        result.put("online_bots",sRandomPlayerbotMgr.GetAllBots().size());
        result.put("native_dispatches",state.nativeDispatches);result.put("native_result_receipts",state.nativeOutcomes);
        result.put("verified_native_results",state.nativeVerifiedResults);result.put("task_admissions",state.taskAdmissions);
        result.put("pending_writes",state.pending.size());result.put("pending_operations",state.operations.size());
        boost::property_tree::ptree actors;unsigned actorCount=0;
        for(const auto& binding:state.bindings) {
            auto* bot=sRandomPlayerbotMgr.GetPlayerBot(binding.first);
            if(!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || ++actorCount>50)continue;
            boost::property_tree::ptree actor;actor.put("actor",binding.first);
            actor.put("alive",bot->IsAlive());actor.put("combat",bot->IsInCombat());
            actor.put("ghost",bot->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST));
            actor.put("corpse_present",bot->GetCorpse()!=nullptr);
            if(auto* corpse=bot->GetCorpse()) {
                actor.put("corpse_map",corpse->GetMapId());actor.put("corpse_age_seconds",time(nullptr)-corpse->GetGhostTime());
                if(corpse->GetMapId()==bot->GetMapId())actor.put("corpse_distance",bot->GetDistance(corpse));
            }
            actor.put("health",bot->GetHealth());actor.put("max_health",bot->GetMaxHealth());
            actor.put("mana",bot->GetPower(POWER_MANA));actor.put("max_mana",bot->GetMaxPower(POWER_MANA));
            actor.put("map",bot->GetMapId());actor.put("x",bot->GetPositionX());
            actor.put("y",bot->GetPositionY());actor.put("z",bot->GetPositionZ());
            actor.put("moving",sServerFacade.isMoving(bot));
            actor.put("ai_state",bot->GetPlayerbotAI()->HandleRemoteCommand("state"));
            actor.put("last_action",bot->GetPlayerbotAI()->HandleRemoteCommand("action"));
            actor.put("strategies",bot->GetPlayerbotAI()->HandleRemoteCommand("strategy"));
            const auto quest=f.setup.template get<uint32_t>("guild_fixture_quest",0);
            const auto dungeon=f.setup.template get<uint32_t>("guild_fixture_dungeon",0);
            if(dungeon) {
                actor.put("dungeon_ready",sGuildEventExecutor.DungeonParticipantReady(bot,dungeon));
                actor.put("instance",bot->GetInstanceId());
                actor.put("active_role",PlayerbotAI::IsTank(bot,false)?"tank":PlayerbotAI::IsHeal(bot,false)?"healer":"damage");
                actor.put("group_id",bot->GetGroup()?bot->GetGroup()->GetId():0);
                actor.put("group_leader",bot->GetGroup()?bot->GetGroup()->GetLeaderGuid().GetCounter():0);
            }
            if(quest) {
                actor.put("quest_status",uint32_t(bot->GetQuestStatus(quest)));
                actor.put("quest_rewarded",bot->GetQuestRewardStatus(quest));
                actor.put("group_id",bot->GetGroup()?bot->GetGroup()->GetId():0);
                actor.put("group_leader",bot->GetGroup()?bot->GetGroup()->GetLeaderGuid().GetCounter():0);
            }
            actor.put("attackers",bot->getAttackers().size());
            auto unitSnapshot=[&](Unit* unit) {
                boost::property_tree::ptree native;
                native.put("present",unit!=nullptr);
                if(!unit)return native;
                native.put("guid",unit->GetObjectGuid().GetRawValue());native.put("entry",unit->GetEntry());
                native.put("alive",unit->IsAlive());native.put("in_world",unit->IsInWorld());
                native.put("health",unit->GetHealth());native.put("max_health",unit->GetMaxHealth());
                native.put("map",unit->GetMapId());native.put("x",unit->GetPositionX());
                native.put("y",unit->GetPositionY());native.put("z",unit->GetPositionZ());
                native.put("combat",unit->IsInCombat());
                native.put("victim",unit->GetVictim()?unit->GetVictim()->GetObjectGuid().GetRawValue():0);
                if(unit->IsInWorld() && unit->GetMap()==bot->GetMap()) {
                    native.put("distance",bot->GetDistance(unit));
                    native.put("line_of_sight",bot->IsWithinLOSInMap(unit));
                }
                return native;
            };
            actor.add_child("victim",unitSnapshot(bot->GetVictim()));
            actor.add_child("ai_target",unitSnapshot(bot->GetPlayerbotAI()->GetAiObjectContext()->template GetValue<Unit*>("current target")->Get()));
            actor.add_child("pet",unitSnapshot(bot->GetPet()));
            if(bot->getClass()==CLASS_HUNTER) {
                // Two original, non-scripted native checks at the existing30s
                // copied observation cadence. No spell, item, target or movement
                // is changed. Capture the real rejection instead of interpreting
                // an IMPOSSIBLE action as an ownership failure.
                boost::property_tree::ptree hunter;
                const uint32 ammo=bot->GetUInt32Value(PLAYER_AMMO_ID);
                hunter.put("ammo_entry",ammo);hunter.put("ammo_count",ammo?bot->GetItemCount(ammo,false):0);
                auto readiness=[&](const char* name,Unit* target) {
                    boost::property_tree::ptree cast;
                    auto* ai=bot->GetPlayerbotAI();
                    const uint32 spell=ai->GetAiObjectContext()->template GetValue<uint32>("spell id",name)->Get();
                    cast.put("spell",spell);cast.put("known",spell && bot->HasSpell(spell));
                    cast.put("target_present",target!=nullptr);
                    SpellCastResult reason=SPELL_FAILED_BAD_TARGETS;
                    const bool ready=target && ai->CanCastSpell(spell,target,0,true,nullptr,false,false,false,&reason);
                    cast.put("native_ready",ready);cast.put("native_result",uint32(reason));
                    return cast;
                };
                hunter.add_child("auto_shot",readiness("auto shot",bot->GetPlayerbotAI()->GetAiObjectContext()->template GetValue<Unit*>("current target")->Get()));
                hunter.add_child("mend_pet",readiness("mend pet",bot->GetPet()));
                actor.add_child("hunter_readiness",hunter);
            }
            actor.put("transport",bool(bot->GetTransport()));actor.put("taxi",bot->IsTaxiFlying());
            actor.put("execution_owner",PlayerbotRendezvousManager::PartyActivityOwnerName(
                sPlayerbotRendezvousManager.GetPartyActivityOwner(binding.first)));
            boost::property_tree::ptree admissions;
            for(const auto& candidate:binding.second.procurementCandidates)admissions.push_back({"",candidate.second});
            actor.add_child("guild_procurement_admissions",admissions);actors.push_back({"",actor});
        }
        result.put("actor_snapshot_truncated",actorCount>50);result.add_child("actors",actors);
        const uint64_t leaseNow=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
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
            item.put("saved_blocker",task.checkpoint.blocker);item.put("retry_at_ms",task.retryAtMs);
            // Private, value-only diagnostic for this marked copied realm.
            // A generic travel blocker must not hide the exact failing native
            // destination/leg. Reading this never installs or advances a route.
            ServiceDestination service{};
            auto* bot=sRandomPlayerbotMgr.GetPlayerBot(task.actor);
            if(bot && bot->GetPlayerbotAI() && (ParseServiceStep(task.checkpoint.step,service) || IsGuildEventCommitment(task))) {
                boost::property_tree::ptree route;
                route.put("actor_map",bot->GetMapId());route.put("actor_x",bot->GetPositionX());
                route.put("actor_y",bot->GetPositionY());route.put("actor_z",bot->GetPositionZ());
                route.put("alive",bot->IsAlive());route.put("combat",bot->IsInCombat());
                route.put("transport",bool(bot->GetTransport()));route.put("taxi",bot->IsTaxiFlying());
                // An authorization getter requires a caller's ExecutionScope.
                // Observing outside that scope is not evidence of no owner.
                // Read the same lease book used by the Admin projection; never
                // acquire a scope or relax permissions to produce diagnostics.
                const auto owner=state.authority.Read(task.actor);
                route.put("owner_state",Name(state.authority.Inspect(task.actor,leaseNow).code));
                route.put("owner_root",owner.lease.rootTask);route.put("owner_generation",owner.lease.generation);
                route.put("owner_matches_task",owner.lease.rootTask==task.id);
                route.put("actor_epoch",bot->GetPlayerbotAI()->GetActivityActorEpoch());
                route.put("map_epoch",bot->GetPlayerbotAI()->GetActivityMapEpoch());
                route.put("task_actor_epoch",task.context.actorGeneration);route.put("task_map_epoch",task.context.mapGeneration);
                auto* context=bot->GetPlayerbotAI()->GetAiObjectContext();
                auto* target=context->template GetValue<ai::TravelTarget*>("travel target")->Get();
                if(target && target->GetDestination() && target->GetPosition()) {
                    const auto* position=target->GetPosition();
                    route.put("entry",target->GetEntry());route.put("purpose",uint32(target->GetDestination()->GetPurpose()));
                    route.put("revision",target->GetRouteRevision());route.put("status",uint32(target->GetStatus()));
                    route.put("map",position->getMapId());route.put("x",position->getX());
                    route.put("y",position->getY());route.put("z",position->getZ());
                    route.put("distance",target->Distance(bot));
                    if(service==ServiceDestination::Gathering) {
                        // Inspect the selected native spawn as well as the
                        // cached nearby list. A point in the travel catalogue
                        // is not proof of a live, eligible resource node.
                        boost::property_tree::ptree gather;
                        GuildProcurementJob job;std::string why;
                        const auto* spawn=dynamic_cast<const ai::GuidPosition*>(position);
                        // Spawn groups use dynamic runtime GUIDs. Resolve the
                        // DB identity through the native map's spawn index,
                        // just as GameObject::LoadFromDB does.
                        auto* node=spawn && spawn->IsGameObject()?bot->GetMap()->GetGameObject(spawn->GetCounter()):nullptr;
                        gather.put("spawn_identity",spawn?spawn->GetRawValue():0);
                        gather.put("live_node",node!=nullptr);
                        const auto nearby=context->template GetValue<std::list<ObjectGuid>>("nearest game objects no los")->Get();
                        gather.put("nearby_count",nearby.size());
                        gather.put("spawn_in_nearby",node && std::find(nearby.begin(),nearby.end(),node->GetObjectGuid())!=nearby.end());
                        if(node) {
                            gather.put("runtime_identity",node->GetObjectGuid().GetRawValue());gather.put("runtime_entry",node->GetEntry());
                            gather.put("spawned",sServerFacade.isSpawned(node));gather.put("in_use",node->IsInUse());
                            gather.put("has_loot",node->m_loot!=nullptr);gather.put("go_state",uint32(node->GetGoState()));
                            gather.put("distance",bot->GetDistance(node));gather.put("stopped",bot->IsStopped());
                            ai::LootObject loot(bot,node->GetObjectGuid());
                            gather.put("legacy_loot_empty",loot.IsEmpty());gather.put("legacy_loot_possible",loot.IsLootPossible(bot));
                            gather.put("skill",loot.skillId);gather.put("required_skill",loot.reqSkillValue);
                            gather.put("approach_blocked",context->template GetValue<ai::LootObjectStack*>("available loot")->Get()->IsApproachBlocked(node->GetObjectGuid()));
                            gather.put("legacy_should_loot",context->template GetValue<bool>("should loot object",std::to_string(node->GetObjectGuid().GetRawValue()))->LazyGet());
                            gather.put("entry_excluded",context->template GetValue<std::set<uint32>&>("skip go loot list")->Get().count(node->GetEntry())!=0);
                            gather.put("mining_pick_carried",bot->HasItemCount(2901,1));
                            if(DecodeGuildProcurementJob(task.checkpoint.data,job,why)) {
                                NativeGatherQuote quote;
                                gather.put("quote_eligible",InspectNativeGatherQuote(*bot,node->GetObjectGuid().GetRawValue(),job.entry,quote,why));
                                gather.put("quote_blocker",why);
                            }
                        }
                        route.add_child("gather_node",gather);
                    }
                }
                const auto& path=context->template GetValue<ai::LastMovement&>("last movement")->Get().lastPath.getPath();
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
    const auto* selection=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
    if(selection && (std::string(selection)=="guild-event-observe-v1" || std::string(selection)=="guild-dungeon-observe-v1")) {
        // Existing characters and their existing quest state only. The private
        // case prepares an explicit calendar appointment; ordinary guild code
        // admits/executes it. No fixture task, group, relocation or quest reward.
        RequireIsolatedNativeFixtureEnvironment();
        const auto number=[](const char* name)->uint32_t {
            const auto* raw=std::getenv(name);const std::string value=raw?raw:"";
            if(value.empty() || value.size()>9 || value.find_first_not_of("0123456789")!=std::string::npos)
                throw std::runtime_error("explicit_guild_observation_ids_required");
            return uint32_t(std::stoul(value));
        };
        try {
            std::vector<uint32_t> cohort;
            if(std::string(selection)=="guild-dungeon-observe-v1") {
                const auto map=number("LIVING_WOW_NATIVE_FIXTURE_ITEM");
                if(!sGuildEventExecutor.DungeonSupported(map))throw std::runtime_error("supported_native_dungeon_required");
                const auto* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_GUILD_MEMBERS");
                cohort=LivingIsolated::GuildDungeonCohort(raw?raw:"");
                f.setup.put("guild_fixture_dungeon",map);
            } else {
                const auto quest=number("LIVING_WOW_NATIVE_FIXTURE_ITEM");
                if(!sObjectMgr.GetQuestTemplate(quest))throw std::runtime_error("native_quest_required");
                f.setup.put("guild_fixture_quest",quest);
                cohort={number("LIVING_WOW_NATIVE_FIXTURE_ACTOR"),number("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT")};
            }
            // An invalid later member must not partially log in a cohort.
            for(const auto actor:cohort)
                if(!actor || !sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER,actor))))
                    throw std::runtime_error("existing_random_bot_required");
            bool online=true;
            for(const auto actor:cohort) {
                auto* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
                if(bot && bot->IsInWorld())continue;
                online=false;const auto key="guild_fixture_login_"+std::to_string(actor);
                if(!f.setup.template get<bool>(key,false)) {
                    f.setup.put(key,true);sRandomPlayerbotMgr.AddRandomBot(actor);
                }
            }
            if(!online)return;
        } catch(const std::exception& error) {
            report("stopped",error.what());state.fixtureFinished=true;return;
        }
    }
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
    try {PrepareIsolatedSkinningSource(state);TraceIsolatedSkinningSource(state);}
    catch(const std::exception& error) {
        state.effective=Mode::Observe;report("stopped",error.what());state.fixtureFinished=true;return;
    }
    if(!f.diagnostics || NowMs()>=f.setup.template get<uint64_t>("next_sample_at_ms",0)) {
        ++f.diagnostics;f.setup.put("next_sample_at_ms",NowMs()+30000);report("observing","");
    }
}
