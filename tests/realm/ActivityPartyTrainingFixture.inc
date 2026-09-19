#include "playerbot/PlayerbotTrainingLesson.h"
// Isolated input: a named copied actor joins a headless human at a real class
// trainer. Three existing known spells may be unlearned to create a repeatable
// lesson requirement. This is NOT organic need or travel coverage.
namespace {
template<class State> void RunIsolatedPartyTraining(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0;uint64_t deadline=0,recalledAt=0,diagnosticAt=0,trainer=0;
        bool logged=false,placed=false,prepared=false,resumed=false,recalled=false,reauthorized=false,leaderJoined=false;
        bool mountTrainerSpawned=false;
        std::string task,blocker,pausedSpells,selectedTask;boost::property_tree::ptree report;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+250;
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool travel=suite=="activity-party-training-travel-resume-v1";
    const bool mountReplay=suite=="activity-party-training-mount-restart-v1";
    const bool tierReplay=suite=="activity-party-training-tier-restart-v1";
    const bool tier=tierReplay || suite=="activity-party-training-tier-v1";
    const bool petReplay=suite=="activity-party-training-pet-restart-v1";
    const bool petOwner=petReplay || suite=="activity-party-training-pet-v1"; // Historical local name; this is actual pet learning.
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
    auto petSpells=[&]() {
        std::string out;if(auto* pet=bot->GetPet())for(const auto& row:pet->m_spells)if(row.second.state!=PETSPELL_REMOVED) {
            if(!out.empty())out+=',';out+=std::to_string(row.first);
        }
        return out;
    };
    auto emit=[&](bool passed,const std::string& why) {
        f.report.put("suite",suite);f.report.put("boot",state.boot);f.report.put("actor",f.actor);
        f.report.put("leader",f.leader);f.report.put("task",f.task);f.report.put("failure",why);f.report.put("blocker",f.blocker);
        f.report.put("outcome",passed?((restart || cast || tier || petOwner)?"passed":"checkpoint"):"failed");
        f.report.put("test_only",true);f.report.put("independent_audit_required",true);
        if(bot){f.report.put("money_after",bot->GetMoney());f.report.put("spells_after",spells());
            if(petOwner && bot->GetPet()){f.report.put("pet_spells_after",petSpells());f.report.put("pet_points_after",bot->GetPet()->m_TrainingPoints);}}
        if(passed)state.enforceEffects.store(false);
        if(!PartyMailFixture().done && (!passed || restart || cast || tier || petOwner))PartyMailFixture().Close();
        std::ofstream out("/isolated/evidence/activity-party-training-"+state.boot+".json");out<<Json(f.report);out.close();
        if(passed && !restart && !cast && !tier && !petOwner){std::ofstream copy("/isolated/evidence/party-training-checkpoint.json");copy<<Json(f.report);}
        if(passed && mount && !mountReplay){std::ofstream copy("/isolated/evidence/party-training-mount-completed.json");copy<<Json(f.report);}
        if(passed && tier && !tierReplay){std::ofstream copy("/isolated/evidence/party-training-tier-completed.json");copy<<Json(f.report);}
        if(passed && petOwner && !petReplay){std::ofstream copy("/isolated/evidence/party-training-pet-owner-completed.json");copy<<Json(f.report);}
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
        bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        // A failed retained save deliberately keeps pending work alive. It must
        // not suppress diagnostics or make this finite fixture wait forever.
        if(NowMs()>=f.diagnosticAt) {
            f.diagnosticAt=NowMs()+10000;
            std::ofstream progress("/isolated/evidence/party-training-progress-"+state.boot+".json");progress<<sLivingActivityCoordinator.ActorJson(f.actor);
            boost::property_tree::ptree diagnostic;
            diagnostic.put("boot",state.boot);diagnostic.put("actor",f.actor);diagnostic.put("blocker",f.blocker);
            diagnostic.put("placed",f.placed);diagnostic.put("prepared",f.prepared);diagnostic.put("task",f.task);
            diagnostic.put("loaded",state.loaded);diagnostic.put("io_pending",state.ioPending);
            diagnostic.put("pending",state.pending.size());diagnostic.put("incoming",state.incoming.size());
            if(bot){diagnostic.put("safety",NativeSafety(bot));diagnostic.put("stopped",bot->IsStopped());diagnostic.put("spells",spells());}
            if(auto* human=sRandomPlayerbotMgr.GetPlayerBot(f.leader))diagnostic.put("leader_safety",NativeSafety(human));
            std::ofstream detail("/isolated/evidence/party-training-fixture-progress-"+state.boot+".json");detail<<Json(diagnostic);
        }
        if(NowMs()>f.deadline){emit(false,"training_deadline_preserved_obligation");return;}
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.logged){f.logged=true;for(auto id:{f.actor,f.leader})
            if(!sRandomPlayerbotMgr.GetPlayerBot(id))sRandomPlayerbotMgr.AddRandomBot(id);}
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(f.leader);
        if(!bot || !leader || !bot->IsInWorld() || !leader->IsInWorld() || NativeSafety(bot) || NativeSafety(leader))return;
        if(petReplay) {
            std::ifstream input("/isolated/evidence/party-training-pet-owner-completed.json");boost::property_tree::read_json(input,f.report);
            require(f.report.template get<std::string>("outcome")=="passed" &&
                f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                "matching_pet_owner_completion_required");
            f.task=f.report.template get<std::string>("task");require(IsUuid(f.task),"pet_owner_completion_uuid_required");
            const auto row=CharacterDatabase.Query(("SELECT "+PersistedTaskProjection()+" FROM living_activity_task WHERE task_id="+SqlValue(f.task)).c_str());
            Task saved;std::string decode;
            require(row && DecodeTaskProjection(row->Fetch()[0].GetCppString(),saved,decode) && saved.id==f.task &&
                saved.actor==f.actor && saved.source=="party_training" && saved.phase==Phase::Completed &&
                !bot->GetGroup() && !leader->GetGroup(),"pet_owner_completed_task_and_party_cleanup_required");
            require(spells()==f.report.template get<std::string>("spells_after") && bot->GetPet() &&
                bot->GetMoney()==f.report.template get<uint32_t>("money_after") && petSpells()==f.report.template get<std::string>("pet_spells_after") &&
                bot->GetPet()->m_TrainingPoints==f.report.template get<int32_t>("pet_points_after") &&
                bot->GetPet()->GetCharmInfo()->GetPetNumber()==f.report.template get<uint32_t>("pet_number"),"pet_restart_preserves_exact_subject_and_spells");
            f.report.put("pet_owner_prior_boot",f.report.template get<std::string>("boot"));
            f.report.put("pet_owner_restart_verified",true);emit(true,"");return;
        }
        if(tierReplay) {
            std::ifstream input("/isolated/evidence/party-training-tier-completed.json");boost::property_tree::read_json(input,f.report);
            require(f.report.template get<std::string>("outcome")=="passed" &&
                f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                "matching_tier_completion_required");
            f.task=f.report.template get<std::string>("task");require(IsUuid(f.task),"tier_completion_uuid_required");
            const auto row=CharacterDatabase.Query(("SELECT "+PersistedTaskProjection()+" FROM living_activity_task WHERE task_id="+SqlValue(f.task)).c_str());
            Task saved;std::string decode;
            require(row && DecodeTaskProjection(row->Fetch()[0].GetCppString(),saved,decode) && saved.id==f.task &&
                saved.actor==f.actor && saved.source=="party_training" && saved.phase==Phase::Completed &&
                !bot->GetGroup() && !leader->GetGroup(),"tier_completed_task_and_party_cleanup_required");
            require(spells()==f.report.template get<std::string>("spells_after") &&
                bot->GetMoney()==f.report.template get<uint32_t>("money_after") &&
                bot->GetSkillValuePure(SKILL_ALCHEMY)==50 && bot->GetSkillMaxPure(SKILL_ALCHEMY)==150 &&
                bot->GetSkillStep(SKILL_ALCHEMY)==2,"tier_restart_preserves_spellbook_money_and_skill");
            f.report.put("tier_prior_boot",f.report.template get<std::string>("boot"));
            f.report.put("tier_restart_verified",true);emit(true,"");return;
        }
        if(mountReplay) {
            std::ifstream input("/isolated/evidence/party-training-mount-completed.json");boost::property_tree::read_json(input,f.report);
            require(f.report.template get<std::string>("outcome")=="passed" &&
                f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                "matching_mount_completion_required");
            f.task=f.report.template get<std::string>("task");
            // Terminal history deliberately is not restored into the active
            // cache. Read this exact fixture record once, without admitting or
            // replaying any work. The independent auditor checks its receipt.
            require(IsUuid(f.task),"mount_completion_uuid_required");
            const auto sql="SELECT "+PersistedTaskProjection()+" FROM living_activity_task WHERE task_id="+SqlValue(f.task);
            const auto row=CharacterDatabase.Query(sql.c_str());Task saved;std::string decode;
            require(row && DecodeTaskProjection(row->Fetch()[0].GetCppString(),saved,decode) &&
                saved.id==f.task && saved.actor==f.actor && saved.source=="party_training" &&
                saved.phase==Phase::Completed && saved.accepted && saved.mode==Mode::Active &&
                !bot->GetGroup() && !leader->GetGroup(),"mount_completed_task_and_party_cleanup_required");
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
                require(petOwner?(bot->getClass()==CLASS_HUNTER && bot->GetLevel()>=10):tier?(bot->HasSpell(2259) && !bot->HasSpell(3101) && bot->GetLevel()>=10 &&
                    bot->GetSkillValuePure(SKILL_ALCHEMY)>0 && bot->GetSkillMaxPure(SKILL_ALCHEMY)==75):
                    cast?(bot->getClass()==CLASS_PALADIN && bot->GetLevel()>=4):
                    (bot->getClass()==CLASS_WARLOCK && bot->GetLevel()>=20),"explicit_class_training_fixture_required");
                if(petOwner) {
                    auto* pet=bot->GetPet();require(pet && pet->IsAlive() && pet->GetCharmInfo() && pet->HasSpell(4187),"existing_known_pet_fixture_required");
                    f.report.put("test_pet_spells_before_unlearning",petSpells());f.report.put("pet_number",pet->GetCharmInfo()->GetPetNumber());
                    f.report.put("pet_points_before",pet->m_TrainingPoints);f.report.put("test_unlearned_pet_spell",4187);
                    require(pet->unlearnSpell(4187,false,false),"test_pet_spell_unlearn_required");
                    pet->SavePetToDB(PET_SAVE_AS_CURRENT,bot); // Explicit copied fixture input, not gameplay.
                    f.report.put("pet_spells_before",petSpells());
                }
                auto target=(tier || petOwner)?WorldDatabase.PQuery("SELECT c.guid,c.id,c.position_x,c.position_y,c.position_z,c.orientation FROM creature c JOIN creature_template t ON t.Entry=c.id WHERE t.TrainerType=%u AND t.TrainerTemplateId=%u AND c.map='%u' ORDER BY POW(c.position_x-%.4f,2)+POW(c.position_y-%.4f,2) LIMIT 1",
                    petOwner?3u:2u,petOwner?100u:1050u,
                    bot->GetMapId(),bot->GetPositionX(),bot->GetPositionY()):
                    WorldDatabase.PQuery("SELECT c.guid,c.id,c.position_x,c.position_y,c.position_z,c.orientation FROM creature c JOIN creature_template t ON t.Entry=c.id WHERE t.TrainerClass='%u' AND t.TrainerType=0 AND c.map='%u' ORDER BY POW(c.position_x-%.4f,2)+POW(c.position_y-%.4f,2) LIMIT 1",
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
                if(tier) {
                    f.report.put("test_original_skill",bot->GetSkillValuePure(SKILL_ALCHEMY));f.report.put("test_skill_input",50);
                    bot->SetSkill(SKILL_ALCHEMY,50,75,1); // Copied prerequisite fixture, not earned skill credit.
                }
                const std::vector<uint32_t> removed=(mount || tier || petOwner)?std::vector<uint32_t>{}:cast?std::vector<uint32_t>{20271}:std::vector<uint32_t>{5782,5697,698};
                if(cast && !mount)require(bot->HasSpell(21084),"training_cast_preknown_target_required");
                for(uint32_t lesson:removed) {
                    require(bot->HasSpell(lesson),"fixture_existing_lesson_required");bot->removeSpell(lesson,false,false);
                }
                f.report.put("test_unlearned_spells",(mount || tier || petOwner)?"":cast?"20271":"5782,5697,698");
                bot->StopMoving();bot->GetMotionMaster()->Clear();
                bot->NearTeleportTo(row[2].GetFloat()+1.0f,row[3].GetFloat(),row[4].GetFloat(),row[5].GetFloat());
                f.placed=true;return;
            }
            if(mount && !f.mountTrainerSpawned) {
                // This teaching-cast template is not normally spawned in the
                // pinned world. Supply a temporary native NPC only in this
                // isolated case; do not claim organic trainer availability.
                RequireIsolatedNativeFixtureEnvironment();
                const auto* info=sObjectMgr.GetCreatureTemplate(4988);
                require(info && info->TrainerClass==CLASS_PALADIN && info->TrainerTemplateId==2010,
                    "mount_native_test_trainer_template_required");
                auto* trainer=bot->SummonCreature(4988,bot->GetPositionX()+1.0f,bot->GetPositionY(),bot->GetPositionZ(),
                    bot->GetOrientation(),TEMPSPAWN_TIMED_DESPAWN,600000);
                require(trainer!=nullptr,"mount_native_test_trainer_spawn_failed");
                f.trainer=trainer->GetObjectGuid().GetRawValue();f.mountTrainerSpawned=true;
                f.report.put("trainer_guid",f.trainer);f.report.put("test_temporary_trainer_entry",4988);
                f.report.put("organic_trainer_availability",false);return;
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
            if(tier) {
                require(planned.lessons==std::vector<uint32_t>{3101},"exact_existing_alchemy_tier_only");
                TrainingLessonQuote q;
                require(PlanNativeTrainingLesson(*bot->GetPlayerbotAI(),ObjectGuid(f.trainer),3101,q,f.blocker) &&
                    CompleteNativeTrainingSkillQuote(*bot,q,f.blocker) && DirectSkillTrainingQuote(q),"native_direct_tier_quote_required");
                f.report.put("test_tier_quote",EncodePartyTrainingQuote(q));
            }
            if(petOwner) {
                require(std::find(planned.lessons.begin(),planned.lessons.end(),4195)!=planned.lessons.end(),"native_pet_owner_offer_required");
                for(auto lesson:planned.lessons) {
                    TrainingLessonQuote q;
                    require(PlanNativeTrainingLesson(*bot->GetPlayerbotAI(),ObjectGuid(f.trainer),lesson,q,f.blocker) &&
                        CompleteNativeTrainingSkillQuote(*bot,q,f.blocker) && FreePetTrainingQuote(q),"pet_trainer_requires_exact_pet_contract");
                }
                require(planned.lessons==std::vector<uint32_t>{4195},"single_copied_pet_lesson_required");
                f.report.put("pet_owner_native_offer",true);
            }
            require(petOwner || tier || mount || (cast?std::find(planned.lessons.begin(),planned.lessons.end(),10321)!=planned.lessons.end():
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
        if(!restart && !cast && !tier && !petOwner && next==1 && saved->phase==Phase::Verifying && state.operations.empty()) {
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
            require((petOwner || tier || cast || (restart && f.recalled && f.reauthorized)) && next==job.lessons.size(),"training_all_interruptions_required");
            if(tier) {
                require(bot->GetSkillValuePure(SKILL_ALCHEMY)==50 && bot->GetSkillMaxPure(SKILL_ALCHEMY)==150 &&
                    bot->GetSkillStep(SKILL_ALCHEMY)==2,"native_tier_changes_capacity_not_earned_skill");
                f.report.put("tier_native_skill_value",50);f.report.put("tier_native_skill_maximum",150);f.report.put("tier_native_skill_step",2);
            }
            if(travel) {
                require(f.report.template get<bool>("saved_trainer_travel_observed",false),"saved_training_travel_phase_required");
                const ObjectGuid trainer(f.report.template get<uint64_t>("trainer_guid"));
                require(bot->GetNPCIfCanInteractWith(trainer,UNIT_NPC_FLAG_TRAINER)!=nullptr,"native_training_walk_arrival_required");
                f.report.put("native_walk_arrival_verified",true);
            }
            for(auto lesson:job.lessons) {
                if(petOwner) {
                    require(lesson==4195 && bot->GetPet() && bot->GetPet()->HasSpell(4187),"training_actual_pet_spellbook_required");
                    require(!bot->HasSpell(lesson) && spells()==f.report.template get<std::string>("spells_before"),"pet_training_owner_unchanged");
                }
                else if(mount) {
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
