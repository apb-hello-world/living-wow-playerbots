// Explicit copied-realm inputs. Existing personal executor performs every cast,
// claim and journal transition. Test-only recipe/material grants are disclosed.
namespace {
template<class State> void RunIsolatedPartyProfession(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0;uint64_t deadline=0,waitAt=0,diagnosticAt=0;
        bool logged=false,prepared=false,authorized=false,recalled=false,reauthorized=false,restored=false,left=false;
        std::string task,blocker;boost::property_tree::ptree report;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+250;
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool restart=suite=="activity-party-profession-resume-v1";
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    Player* bot=nullptr;
    auto inventory=[&]() {
        std::string rows="[";bool first=true;
        auto items=bot->GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask(uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BAGS)|uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BANK)));
        items.sort([](Item* a,Item* b){return a->GetGUIDLow()<b->GetGUIDLow();});
        for(auto* item:items){boost::property_tree::ptree row;row.put("guid",item->GetGUIDLow());row.put("entry",item->GetEntry());
            row.put("quantity",item->GetCount());row.put("position",item->GetPos());
            if(!first)rows+=',';first=false;rows+=Json(row);}
        return rows+"]";
    };
    auto operations=[&]() {
        auto rows=CharacterDatabase.PQuery("SELECT COUNT(*) FROM living_activity_operation WHERE task_id='%s'",f.task.c_str());
        require(bool(rows),"craft_operation_read_required");return rows->Fetch()[0].GetUInt32();
    };
    auto emit=[&](bool passed,const std::string& why) {
        f.report.put("suite",suite);f.report.put("boot",state.boot);f.report.put("actor",f.actor);f.report.put("leader",f.leader);
        f.report.put("task",f.task);f.report.put("outcome",passed?(restart?"passed":"checkpoint"):"failed");
        f.report.put("failure",why);f.report.put("blocker",f.blocker);f.report.put("test_only",true);
        f.report.put("independent_audit_required",true);
        if(bot){f.report.put("money_after",bot->GetMoney());f.report.put("inventory_after",inventory());
            f.report.put("skill_after",bot->GetSkillValuePure(SKILL_ALCHEMY));f.report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));}
        if(passed)state.enforceEffects.store(false);
        if(!PartyMailFixture().done && !passed)PartyMailFixture().Close();
        std::ofstream out("/isolated/evidence/activity-party-profession-"+state.boot+".json");out<<Json(f.report);out.close();
        if(passed && !restart){std::ofstream checkpoint("/isolated/evidence/party-profession-checkpoint.json");checkpoint<<Json(f.report);checkpoint.close();
            if(!checkpoint)sLog.outError("Party profession checkpoint write failed");}
        state.fixtureFinished=true;sLog.outString("Living isolated party profession: %s",Json(f.report).c_str());
    };
    try {
        if(!f.actor) {
            const auto* actor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const auto* leader=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(actor && leader,"explicit_profession_pair_required");f.actor=uint32_t(std::stoul(actor));f.leader=uint32_t(std::stoul(leader));
            require(f.actor && f.leader && f.actor!=f.leader,"distinct_profession_pair_required");
            state.gameplayFixtureActor.store(f.actor);state.professionCohortActors.back().store(f.leader);f.deadline=NowMs()+600000;
            if(restart) {
                std::ifstream input("/isolated/evidence/party-profession-checkpoint.json");boost::property_tree::read_json(input,f.report);
                require(f.report.template get<std::string>("outcome")=="checkpoint" &&
                    f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                    "matching_profession_checkpoint_required");
                f.task=f.report.template get<std::string>("task");require(IsUuid(f.task),"profession_checkpoint_uuid");
                f.report.put("checkpoint_boot",f.report.template get<std::string>("boot"));f.prepared=true;
            }
        }
        bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>=f.diagnosticAt){f.diagnosticAt=NowMs()+10000;
            std::ofstream progress("/isolated/evidence/party-profession-progress-"+state.boot+".json");progress<<sLivingActivityCoordinator.ActorJson(f.actor);}
        if(NowMs()>f.deadline){emit(false,"profession_deadline_preserved_obligation");return;}
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.logged){f.logged=true;for(auto id:{f.actor,f.leader})if(!sRandomPlayerbotMgr.GetPlayerBot(id))sRandomPlayerbotMgr.AddRandomBot(id);}
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(f.leader);
        if(!bot || !leader || !bot->IsInWorld() || !leader->IsInWorld() || NativeSafety(bot) || NativeSafety(leader))return;
        if(!f.prepared) {
            require(!bot->GetGroup() && !leader->GetGroup(),"existing_profession_groups_preserved");
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active)
                require(Terminal(row.second.phase),"existing_profession_obligation_preserved");
            require(bot->HasSpell(2259) && bot->GetSkillValuePure(SKILL_ALCHEMY)>0 &&
                bot->GetSkillValuePure(SKILL_ALCHEMY)<10,"existing_native_apprentice_required");
            f.report.put("money_before",bot->GetMoney());f.report.put("skill_before",bot->GetSkillValuePure(SKILL_ALCHEMY));
            f.report.put("inventory_pre_fixture",inventory());f.report.put("money_grants",0);f.report.put("skill_grants",0);
            f.report.put("recipe_granted",bot->HasSpell(2330)?0:2330);
            boost::property_tree::ptree grants;
            for(uint32_t entry:{765,2447,3371}) {
                const auto bag=bot->GetItemCount(entry,false),all=bot->GetItemCount(entry,true);
                require(bag || !all,"fixture_never_copies_banked_materials");
                if(!bag){boost::property_tree::ptree grant;grant.put("entry",entry);grant.put("quantity",1);grants.push_back({"",grant});}
            }
            f.report.add_child("material_grants",grants);
            const auto setupPath="/isolated/evidence/party-profession-setup-"+state.boot+".json";
            require(!std::ifstream(setupPath).good(),"fixture_setup_never_replayed");
            std::ofstream setup(setupPath);setup<<Json(f.report);setup.close();require(bool(setup),"fixture_intent_saved");
            if(!bot->HasSpell(2330))bot->learnSpell(2330,false);
            for(const auto& row:grants){const auto entry=row.second.template get<uint32_t>("entry");ItemPosCountVec positions;
                require(bot->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)==EQUIP_ERR_OK && bot->StoreNewItem(positions,entry,true),
                    "explicit_fixture_material_capacity");}
            f.report.put("inventory_before",inventory());f.report.put("output_before",bot->GetItemCount(118,true));
            bot->SaveToDB();f.prepared=true;state.enforceEffects.store(true);return;
        }
        if(f.task.empty()) {
            if(!PartyMailFixture().Prepare(state,bot,false))return;
            ProfessionJob job;require(BuildNativeSkillGainJob(*bot,2330,job,f.blocker),"native_ready_recipe:"+f.blocker);
            require(job.reagents==std::vector<ProfessionReagent>({{765,1},{2447,1},{3371,1}}),"native_exact_recipe_reagents");
            TaskRequest request;auto& task=request.task;task.source="profession_job";task.sourceKey="isolated_party_profession_v1:"+state.boot;
            task.id=task.root=f.task=SourceId(task.source,task.sourceKey);task.actor=f.actor;task.kind=Kind::Profession;
            task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Progression;
            task.context=ReadNativeContext(*bot,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeProfessionJob(job);request.receipt=NewId();
            f.report.put("accepted_job",task.checkpoint.data);f.report.put("party_group",bot->GetGroup()->GetId());
            require(sLivingActivityCoordinator.SubmitTask(request).code==AdmissionCode::Pending,"ordinary_profession_admission");
            f.waitAt=NowMs();return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);if(!saved)return;
        require(saved->actor==f.actor && saved->source=="profession_job" && saved->root==f.task &&
            saved->checkpoint.data==f.report.template get<std::string>("accepted_job"),"same_immutable_personal_job");
        f.blocker=state.executionBlockers[f.task];
        if(restart) {
            if(!f.restored) {
                if(!PartyMailFixture().Restore(bot,f.report.template get<uint32_t>("party_group"),f.leader))return;
                require(inventory()==f.report.template get<std::string>("inventory_after") &&
                    bot->GetMoney()==f.report.template get<uint32_t>("money_after"),"restart_native_possessions_unchanged");
                state.enforceEffects.store(true);f.restored=true;f.waitAt=NowMs();return;
            }
            if(!f.left) {
                require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"old_party_craft_permission_not_restored");
                require(!sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,Mask(Effect::Spell),60000,"isolated_craft_old_permission").Permitted(),
                    "restart_craft_dispatch_denied");
                require(operations()==1 && inventory()==f.report.template get<std::string>("inventory_after"),"restart_no_duplicate_craft");
                if(NowMs()-f.waitAt<10000)return;
                f.report.put("restart_unauthorized_wait_ms",NowMs()-f.waitAt);PartyMailFixture().Close();f.left=true;return;
            }
            if(saved->phase!=Phase::Completed)return;
            require(operations()==1 && bot->GetMoney()==f.report.template get<uint32_t>("money_before") &&
                inventory()==f.report.template get<std::string>("inventory_after"),"ordinary_settlement_without_second_cast");
            f.report.put("completed_revision",saved->revision);bot->SaveToDB();emit(true,"");return;
        }
        if(!f.authorized) {
            require(operations()==0 && inventory()==f.report.template get<std::string>("inventory_before"),"no_craft_without_party_permission");
            if(NowMs()-f.waitAt<5000)return;
            if(!sLivingActivityCoordinator.SelectPartyProfessionService(f.actor))return;
            if(!PartyMailFixture().Prepare(state,bot))return;
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->service==PartyServiceBinding::Service::Profession && binding->root==f.task,"existing_root_bound_to_party");
            f.report.put("accepted_revision",binding->acceptedRevision);
            require(sPlayerbotRendezvousManager.ResumePartyAssist(bot,leader,"isolated_craft_before_dispatch"),"native_pre_cast_recall");
            f.authorized=f.recalled=true;f.waitAt=NowMs();return;
        }
        if(!f.reauthorized) {
            require(operations()==0 && inventory()==f.report.template get<std::string>("inventory_before"),"recall_prevents_cast");
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"recalled_craft_permission_absent");
            if(NowMs()-f.waitAt<10000)return;
            if(!sLivingActivityCoordinator.SelectPartyProfessionService(f.actor))return;
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_craft_fresh_permission"))return;
            f.report.put("pre_cast_recall_wait_ms",NowMs()-f.waitAt);f.reauthorized=true;return;
        }
        if(saved->phase!=Phase::Verifying || !state.operations.empty() || sLivingActivityCoordinator.DefersNativeSave(f.actor))return;
        require(operations()==1 && bot->GetItemCount(118,true)==f.report.template get<uint32_t>("output_before")+1 &&
            bot->GetMoney()==f.report.template get<uint32_t>("money_before"),"one_native_craft_no_money_change");
        require(bot->GetSkillValuePure(SKILL_ALCHEMY)>f.report.template get<uint32_t>("skill_before"),"actual_native_skill_gain");
        f.report.put("checkpoint_revision",saved->revision);f.report.put("checkpoint_phase",Name(saved->phase));
        bot->SaveToDB();leader->SaveToDB();emit(true,"");
    } catch(const std::exception& error){emit(false,error.what());}
}
}
