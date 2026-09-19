#include "playerbot/PlayerbotTrainingLesson.h"
// Isolated input: a named copied actor joins a headless human at a real class
// trainer. Three existing known spells may be unlearned to create a repeatable
// lesson requirement. This is NOT organic need or travel coverage.
namespace {
template<class State> void RunIsolatedPartyTraining(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0;uint64_t deadline=0,recalledAt=0,diagnosticAt=0,trainer=0;
        bool logged=false,placed=false,prepared=false,resumed=false,recalled=false,reauthorized=false,leaderJoined=false;
        std::string task,blocker,pausedSpells,selectedTask;boost::property_tree::ptree report;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+250;
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool travel=suite=="activity-party-training-travel-resume-v1";
    const bool mountReplay=suite=="activity-party-training-mount-restart-v1";
    const bool mount=mountReplay || suite=="activity-party-training-mount-v1";
    const bool cast=mount || suite=="activity-party-training-cast-v1";
    const bool restart=travel || suite=="activity-party-training-resume-v1";
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    Player* bot=nullptr;
    auto spells=[&]() {
        std::set<uint32_t> known;
        for(const auto& row:bot->GetSpellMap())if(row.second.state!=PLAYERSPELL_REMOVED && !row.second.disabled)known.insert(row.first);
        std::string out;for(auto id:known){if(!out.empty())out+=',';out+=std::to_string(id);}return out;
    };
    auto emit=[&](bool passed,const std::string& why) {
        f.report.put("suite",suite);f.report.put("boot",state.boot);f.report.put("actor",f.actor);
        f.report.put("leader",f.leader);f.report.put("task",f.task);f.report.put("failure",why);f.report.put("blocker",f.blocker);
        f.report.put("outcome",passed?((restart || cast)?"passed":"checkpoint"):"failed");
        f.report.put("test_only",true);f.report.put("independent_audit_required",true);
        if(bot){f.report.put("money_after",bot->GetMoney());f.report.put("spells_after",spells());}
        if(passed)state.enforceEffects.store(false);
        if(!PartyMailFixture().done && (!passed || restart || cast))PartyMailFixture().Close();
        std::ofstream out("/isolated/evidence/activity-party-training-"+state.boot+".json");out<<Json(f.report);out.close();
        if(passed && !restart && !cast){std::ofstream copy("/isolated/evidence/party-training-checkpoint.json");copy<<Json(f.report);}
        if(passed && mount && !mountReplay){std::ofstream copy("/isolated/evidence/party-training-mount-completed.json");copy<<Json(f.report);}
        state.fixtureFinished=true;
        sLog.outString("Living isolated training fixture: %s",Json(f.report).c_str());
    };
    try {
        if(!f.actor) {
            const auto* actor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");
            const auto* leader=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(actor && leader,"explicit_training_pair_required");
            f.actor=uint32_t(std::stoul(actor));f.leader=uint32_t(std::stoul(leader));
            require(f.actor && f.leader && f.actor!=f.leader,"distinct_training_pair_required");
            state.gameplayFixtureActor.store(f.actor);state.professionCohortActors.back().store(f.leader);
            f.deadline=NowMs()+600000;
            if(restart) {
                std::ifstream input("/isolated/evidence/party-training-checkpoint.json");boost::property_tree::read_json(input,f.report);
                require(f.report.template get<std::string>("outcome")=="checkpoint" &&
                    f.report.template get<std::string>("suite")=="activity-party-training-checkpoint-v1" &&
                    f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                    "matching_training_checkpoint_required");
                f.task=f.report.template get<std::string>("task");require(IsUuid(f.task),"training_checkpoint_uuid_required");
                f.report.put("checkpoint_boot",f.report.template get<std::string>("boot"));f.prepared=true;
            }
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready)return;
        bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit(false,"training_deadline_preserved_obligation");return;}
        if(!f.logged){f.logged=true;for(auto id:{f.actor,f.leader})
            if(!sRandomPlayerbotMgr.GetPlayerBot(id))sRandomPlayerbotMgr.AddRandomBot(id);}
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(f.leader);
        if(!bot || !leader || !bot->IsInWorld() || !leader->IsInWorld() || NativeSafety(bot) || NativeSafety(leader))return;
        if(mountReplay) {
            std::ifstream input("/isolated/evidence/party-training-mount-completed.json");boost::property_tree::read_json(input,f.report);
            require(f.report.template get<std::string>("outcome")=="passed" &&
                f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                "matching_mount_completion_required");
            f.task=f.report.template get<std::string>("task");
            const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);if(!saved)return;
            require(saved->phase==Phase::Completed && !bot->GetGroup() && !leader->GetGroup(),"mount_completed_task_and_party_cleanup_required");
            require(spells()==f.report.template get<std::string>("spells_after") &&
                bot->GetMoney()==f.report.template get<uint32_t>("money_after"),"mount_restart_preserves_native_spells_and_money");
            require(bot->GetSkillValuePure(SKILL_RIDING)==75 && bot->GetSkillMaxPure(SKILL_RIDING)==75 &&
                bot->GetSkillStep(SKILL_RIDING)==1,"mount_restart_reconstructs_native_riding_step");
            f.report.put("mount_prior_boot",f.report.template get<std::string>("boot"));
            f.report.put("mount_restart_verified",true);emit(true,"");return;
        }
        if(!f.prepared) {
            require(!bot->GetGroup() && !leader->GetGroup(),"existing_training_groups_preserved");
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_training_actor_obligation_preserved");
            if(!f.placed) {
                require(cast?(bot->getClass()==CLASS_PALADIN && bot->GetLevel()>=4):
                    (bot->getClass()==CLASS_WARLOCK && bot->GetLevel()>=20),"explicit_class_training_fixture_required");
                auto target=WorldDatabase.PQuery("SELECT c.guid,c.id,c.position_x,c.position_y,c.position_z,c.orientation FROM creature c JOIN creature_template t ON t.Entry=c.id WHERE t.TrainerClass='%u' AND t.TrainerType=0 AND c.map='%u' ORDER BY POW(c.position_x-%.4f,2)+POW(c.position_y-%.4f,2) LIMIT 1",
                    uint32_t(bot->getClass()),bot->GetMapId(),bot->GetPositionX(),bot->GetPositionY());
                require(bool(target),"real_class_trainer_spawn_required");const auto* row=target->Fetch();
                f.trainer=ObjectGuid(HIGHGUID_UNIT,row[1].GetUInt32(),row[0].GetUInt32()).GetRawValue();
                f.report.put("test_service_placement",true);f.report.put("trainer_guid",f.trainer);
                f.report.put("test_spells_before_unlearning",spells());
                // Test setup only, never production admission. No spell grant.
                if(mount) {
                    require(bot->GetLevel()<40 && !bot->HasSkill(SKILL_RIDING),"mount_fixture_untrained_actor_required");
                    f.report.put("test_original_level",bot->GetLevel());f.report.put("test_level_input",40);
                    bot->GiveLevel(40); // Explicit copied-character level fixture, never organic advancement.
                }
                const std::vector<uint32_t> removed=mount?std::vector<uint32_t>{}:cast?std::vector<uint32_t>{20271}:std::vector<uint32_t>{5782,5697,698};
                if(cast && !mount)require(bot->HasSpell(21084),"training_cast_preknown_target_required");
                for(uint32_t lesson:removed) {
                    require(bot->HasSpell(lesson),"fixture_existing_lesson_required");bot->removeSpell(lesson,false,false);
                }
                f.report.put("test_unlearned_spells",mount?"":cast?"20271":"5782,5697,698");
                bot->StopMoving();bot->GetMotionMaster()->Clear();
                bot->NearTeleportTo(row[2].GetFloat()+1.0f,row[3].GetFloat(),row[4].GetFloat(),row[5].GetFloat());
                f.placed=true;return;
            }
            PartyTrainingJob planned;
            if(!PlanNativePartyTraining(*bot,ObjectGuid(f.trainer),planned,f.blocker))return;
            if(mount) {
                uint32_t selected=0;
                for(auto lesson:planned.lessons) {
                    TrainingLessonQuote q;
                    if(PlanNativeTrainingLesson(*bot->GetPlayerbotAI(),ObjectGuid(f.trainer),lesson,q,f.blocker) &&
                        CompleteNativeTrainingSkillQuote(*bot,q,f.blocker) && q.skill.id==SKILL_RIDING && q.skill.after.step==1) {
                        selected=lesson;f.report.put("test_mount_quote",EncodePartyTrainingQuote(q));break;
                    }
                }
                require(selected,"native_apprentice_mount_offer_required");planned.lessons={selected};
                f.report.put("test_selected_native_offer",true);
            }
            require(mount || (cast?std::find(planned.lessons.begin(),planned.lessons.end(),10321)!=planned.lessons.end():
                planned.lessons.size()>=3),"native_training_lesson_manifest_required");
            f.report.put("accepted_manifest",EncodePartyTrainingJob(planned));f.report.put("spells_before",spells());
            f.report.put("money_before",bot->GetMoney());f.report.put("money_grants",0);f.report.put("item_grants",0);
            bot->SaveToDB();f.prepared=true;state.enforceEffects.store(true);
            std::ofstream setup("/isolated/evidence/party-training-setup-"+state.boot+".json");setup<<Json(f.report);return;
        }
        if(f.task.empty()) {
            if(mount) {
                if(!PartyMailFixture().Prepare(state,bot,false)){f.blocker="mount_fixture_party_pending";return;}
                if(f.selectedTask.empty()) {
                    TaskRequest r;auto& task=r.task;task.source="party_training";
                    task.sourceKey="fixture_mount:"+std::to_string(f.actor)+':'+state.boot;
                    task.id=task.root=SourceId(task.source,task.sourceKey);task.actor=f.actor;
                    task.kind=Kind::PartyErrand;task.mode=Mode::Active;task.phase=Phase::Queued;task.priority=Priority::Delivery;task.accepted=true;
                    task.context=ReadNativeContext(*bot,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
                    task.checkpoint.step="party_training_prepare";task.checkpoint.data=f.report.template get<std::string>("accepted_manifest");
                    r.receipt=NewId();const auto admitted=sLivingActivityCoordinator.SubmitTask(r);
                    if(admitted.code!=AdmissionCode::Pending && admitted.code!=AdmissionCode::Saved){f.blocker=admitted.blocker;return;}
                    f.selectedTask=task.id;return;
                }
                if(!sLivingActivityCoordinator.ReadSavedTask(f.selectedTask))return;
            }
            if(!PartyMailFixture().Prepare(state,bot)){f.blocker="training_native_party_window_pending";return;}
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            if(!binding){f.blocker="training_native_admission_pending";return;}
            require(binding->service==PartyServiceBinding::Service::Training,"training_only_binding_required");f.task=binding->root;
            if(mount)require(f.task==f.selectedTask,"native_mount_selected_root_required");
            f.report.put("party_group",bot->GetGroup()->GetId());
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);if(!saved)return;
        PartyTrainingJob job;
        require(IsPartyTrainingTask(*saved) && saved->actor==f.actor && DecodePartyTrainingJob(saved->checkpoint.data,job),"original_training_root_required");
        const auto next=job.next;job.next=0;
        require(EncodePartyTrainingJob(job)==f.report.template get<std::string>("accepted_manifest"),"accepted_training_lessons_immutable");
        require(bot->GetMoney()==f.report.template get<uint32_t>("money_before"),"free_training_wallet_unchanged");
        f.blocker=state.executionBlockers[f.task];
        if(travel && saved->phase==Phase::Traveling && saved->checkpoint.step==ServiceStep(ServiceDestination::ClassTrainer))
            f.report.put("saved_trainer_travel_observed",true);
        if(NowMs()>=f.diagnosticAt) {
            f.diagnosticAt=NowMs()+10000;
            std::ofstream progress("/isolated/evidence/party-training-progress-"+state.boot+".json");progress<<sLivingActivityCoordinator.ActorJson(f.actor);
        }
        if(!restart && !cast && next==1 && saved->phase==Phase::Verifying && state.operations.empty()) {
            f.report.put("checkpoint_revision",saved->revision);f.report.put("checkpoint_at_ms",NowMs());
            f.report.put("checkpoint_spells",spells());f.report.put("party_group",bot->GetGroup()->GetId());
            bot->SaveToDB();leader->SaveToDB();emit(true,"");return;
        }
        if(restart && !f.resumed) {
            require(next==1 && spells()==f.report.template get<std::string>("checkpoint_spells"),"training_restart_spellbook_unchanged");
            auto& party=PartyMailFixture();
            if(travel && !f.placed) {
                // Deliberate isolated displacement, not a journey success. A
                // checked local path ensures the test starts outside trainer
                // range on reachable ground; only shared native movement may
                // bring the actor back after authorization.
                const float x=bot->GetPositionX(),y=bot->GetPositionY(),z=bot->GetPositionZ();
                for(unsigned i=0;i<8 && !f.placed;++i) {
                    const float angle=float(i)*float(M_PI)/4.0f;
                    PathFinder path(bot);
                    path.calculate(x+12*std::cos(angle),y+12*std::sin(angle),z,false);
                    const PathType invalid=PathType(PATHFIND_NOPATH|PATHFIND_SHORTCUT|PATHFIND_INCOMPLETE|PATHFIND_NOT_USING_PATH|PATHFIND_SHORT);
                    if(!(path.getPathType()&PATHFIND_NORMAL) || (path.getPathType()&invalid) || path.getPath().size()<2)continue;
                    const auto& end=path.getPath().back();
                    if(std::hypot(end.x-x,end.y-y)<10)continue;
                    f.report.put("test_walk_start_x",end.x);f.report.put("test_walk_start_y",end.y);f.report.put("test_walk_start_z",end.z);
                    f.report.put("test_displaced_outside_trainer_range",true);
                    bot->NearTeleportTo(end.x,end.y,end.z,bot->GetOrientation());f.placed=true;
                }
                require(f.placed,"training_walk_reachable_test_start_required");return;
            }
            if(!f.leaderJoined) {
                require(bot->GetMapId()==leader->GetMapId(),"training_replay_same_map_leader_input");f.leaderJoined=true;
                f.report.put("test_leader_joined_trainer",true);
                leader->NearTeleportTo(bot->GetPositionX(),bot->GetPositionY(),bot->GetPositionZ(),bot->GetOrientation());return;
            }
            if(!party.recovered){if(!party.Restore(bot,f.report.template get<uint32_t>("party_group"),f.leader))return;
                require(party.restoredReason=="realm_restart_errands_deferred","training_old_consent_deferred");}
            state.enforceEffects.store(true);
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"training_old_permission_not_restored");
            require(!sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,Mask(Effect::Spell)|Mask(Effect::Social),
                60000,"isolated_training_old_permission").Permitted(),"training_old_effects_denied");
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_training_restart_permission"))return;
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->root==f.task,"training_restart_same_root");
            f.report.put("restart_old_permission_denied",true);f.report.put("restart_spellbook_unchanged",true);
            f.report.put("restart_reauthorized_at_ms",NowMs());f.resumed=true;
        }
        if(restart && f.resumed && !f.recalled && next==2 && saved->phase==Phase::Verifying && state.operations.empty()) {
            f.pausedSpells=spells();f.recalledAt=NowMs();f.report.put("recalled_at_ms",f.recalledAt);
            require(sPlayerbotRendezvousManager.ResumePartyAssist(bot,leader,"isolated_training_recall"),"native_training_recall_required");
            f.recalled=true;return;
        }
        if(f.recalled && !f.reauthorized) {
            require(next==2 && !Terminal(saved->phase) && spells()==f.pausedSpells,"training_recall_preserves_spellbook");
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved) &&
                !sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,Mask(Effect::Spell)|Mask(Effect::Social),
                    60000,"isolated_training_recall_probe").Permitted(),"training_recall_effects_denied");
            if(NowMs()<f.recalledAt+10000)return;
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_training_reauthorization"))return;
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->root==f.task,"training_recall_reuses_original_root");
            f.report.put("reauthorized_at_ms",NowMs());f.report.put("recalled_effects_denied",true);
            f.report.put("paused_spellbook_unchanged",true);f.reauthorized=true;
        }
        if(saved->phase==Phase::Completed) {
            require((cast || (restart && f.recalled && f.reauthorized)) && next==job.lessons.size(),"training_all_interruptions_required");
            if(travel) {
                require(f.report.template get<bool>("saved_trainer_travel_observed",false),"saved_training_travel_phase_required");
                const ObjectGuid trainer(f.report.template get<uint64_t>("trainer_guid"));
                require(bot->GetNPCIfCanInteractWith(trainer,UNIT_NPC_FLAG_TRAINER)!=nullptr,"native_training_walk_arrival_required");
                f.report.put("native_walk_arrival_verified",true);
            }
            for(auto lesson:job.lessons) {
                if(mount) {
                    TrainingLessonQuote q;require(DecodePartyTrainingQuote(f.report.template get<std::string>("test_mount_quote"),q),"mount_native_quote_required");
                    for(auto spell:q.playerSpells)require(bot->HasSpell(spell),"mount_all_native_targets_required");
                    require(bot->GetSkillValuePure(SKILL_RIDING)==75 && bot->GetSkillMaxPure(SKILL_RIDING)==75 &&
                        bot->GetSkillStep(SKILL_RIDING)==1,"mount_native_skill_step_required");
                    f.report.put("mount_native_skill_value",75);f.report.put("mount_native_skill_maximum",75);f.report.put("mount_native_skill_step",1);
                }
                else if(cast && lesson==10321)require(bot->HasSpell(20271) && bot->HasSpell(21084),"training_all_cast_targets_required");
                else require(bot->HasSpell(lesson),"training_actual_spellbook_required");
            }
            f.report.put("final_revision",saved->revision);emit(true,"");
        }
    }catch(const std::exception& error){emit(false,error.what());}
}
}
