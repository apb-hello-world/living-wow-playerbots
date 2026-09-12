// Compiled only into the marked isolated realm. Uses the ordinary profession
// due queue and native bank/cast/persistence adapters. One explicitly recorded
// test tool may be supplied; reagents, gear, money and skill are never granted.
namespace {
template<class FixtureState>
void RunIsolatedEnchantFixture(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;auto& f=state.enchantFixture;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](const std::string& outcome,const std::string& failure,Player* actor) {
        boost::property_tree::ptree report;
        report.put("suite","activity-enchant-v1");report.put("boot",state.boot);report.put("test_only",true);
        report.put("outcome",outcome);report.put("failure",failure);report.put("actor",f.actor);report.put("task",f.task);
        report.put("original_boot",f.originalBoot);report.put("resumed_same_task",f.resuming);
        report.put("ordinary_due_queue",true);report.put("independent_audit_required",true);
        report.put("general_execution_enabled",state.effective==Mode::Active);report.put("blocker",f.blocker);
        report.add_child("setup",f.setup);
        if(actor && f.started) {
            report.put("money_after",actor->GetMoney());report.put("skill_after",actor->GetSkillValuePure(f.job.skill));
            report.put("tool_count_after",actor->GetItemCount(6218,true));
            EnchantSubject after;std::string why;
            if(ReadNativeEnchantSubject(*actor,f.job,after,why)) {
                boost::property_tree::ptree value;std::istringstream input(EnchantSubjectJson(after));
                boost::property_tree::read_json(input,value);report.add_child("subject_after",value);
            }
            report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
        }
        if(const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task)) {
            report.put("revision",saved->revision);report.put("phase",Name(saved->phase));
        }
        std::ofstream out("/isolated/evidence/activity-enchant-"+state.boot+".json");out<<Json(report);out.close();
        state.fixtureFinished=true;
        if(!out)sLog.outError("Living enchanting evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* actor=nullptr;
    try {
        if(!f.actor) {
            const auto* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const std::string id=raw?raw:"";
            require(!id.empty() && id.size()<=9 && id.find_first_not_of("0123456789")==std::string::npos,"explicit_enchant_actor_required");
            f.actor=uint32_t(std::stoul(id));require(f.actor!=0,"explicit_enchant_actor_required");
            f.recipe=7443;f.task=SourceId("profession_job","isolated_enchant_v1:"+state.boot);
            f.originalBoot=state.boot;
            const auto* original=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
            if(original && *original) {require(IsUuid(original),"exact_enchant_resume_task_required");f.task=original;f.resuming=true;}
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.deadline)f.deadline=NowMs()+600000;
        actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit("failed","enchant_deadline_with_preserved_task",actor);return;}
        if(!actor) {
            if(!f.requested && sRandomPlayerbotMgr.GetAllBots().size()<49){f.requested=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
            f.blocker="native_actor_login_pending";return;
        }
        if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() || actor->GetTradeData() ||
            actor->GetMap()->IsDungeon()){f.blocker="native_safety_wait";return;}
        if(!f.started) {
            require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer() && actor->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"copied_native_enchanter_required");
            if(f.resuming) {
                const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
                require(saved && saved->actor==f.actor && saved->mode==Mode::Active && saved->accepted &&
                    saved->source=="profession_job" && saved->sourceKey.compare(0,20,"isolated_enchant_v1:")==0 &&
                    !Terminal(saved->phase),"original_unfinished_enchant_job_required");
                f.originalBoot=saved->sourceKey.substr(20);require(IsUuid(f.originalBoot),"original_enchant_boot_required");
                std::ifstream setup("/isolated/evidence/enchant-setup-"+f.task+".json",std::ios::ate);
                require(setup && setup.tellg()>0 && setup.tellg()<32768,"original_enchant_setup_required");
                setup.seekg(0);boost::property_tree::read_json(setup,f.setup);
                require(f.setup.get<uint32_t>("actor")==f.actor && f.setup.get<std::string>("task")==f.task &&
                    f.setup.get<std::string>("job")==saved->checkpoint.data &&
                    DecodeProfessionJob(saved->checkpoint.data,f.job,f.blocker),"original_enchant_identity_changed");
                f.subject=EnchantCodec::Subject(f.setup.get_child("subject_before"));
                f.before.actor=f.actor;f.before.money=f.setup.get<uint32_t>("money_before");f.before.skill=f.setup.get<uint32_t>("skill_before");
                f.toolBefore=f.setup.get<uint32_t>("tool_before");f.toolGrant=f.setup.get<uint32_t>("tool_grant");
                f.toolGuid=f.setup.get<uint32_t>("tool_guid");
                require(ReadNativeEnchantSpec(*actor,f.job,f.enchant,f.blocker),"original_enchant_recipe_required");
                state.enforceEffects.store(true,std::memory_order_release);f.started=true;return;
            }
            for(const auto& row:state.cache) if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_enchant_actor_obligation_preserved");
            require(BuildNativeSkillGainJob(*actor,f.recipe,f.job,f.blocker),"native_enchant_recipe:"+f.blocker);
            require(f.job.operation==ProfessionOperation::EnchantItem && f.job.skill==SKILL_ENCHANTING && !f.job.outputEntry,
                "distinct_native_enchant_operation_required");
            require(ReadNativeCraftFrame(*actor,f.job,f.before,f.blocker) &&
                ReadNativeEnchantSubject(*actor,f.job,f.subject,f.blocker) && ReadNativeEnchantSpec(*actor,f.job,f.enchant,f.blocker),
                "native_enchant_before_state:"+f.blocker);
            f.toolBefore=actor->GetItemCount(6218,true);
            require(f.toolBefore==0 || actor->HasItemCount(6218,1,false),"banked_tool_requires_real_preparation");
            f.setup.put("actor",f.actor);f.setup.put("task",f.task);f.setup.put("job",EncodeProfessionJob(f.job));
            f.setup.put("money_before",f.before.money);f.setup.put("skill_before",f.before.skill);
            f.setup.put("tool_entry",6218);f.setup.put("tool_before",f.toolBefore);f.setup.put("tool_grant",f.toolBefore?0:1);
            f.setup.put("reagent_grants",0);f.setup.put("gear_grants",0);f.setup.put("money_grants",0);
            f.setup.put("spell_grants",0);f.setup.put("skill_grants",0);f.setup.put("fixture_relocations",0);
            boost::property_tree::ptree target;std::istringstream input(EnchantSubjectJson(f.subject));
            boost::property_tree::read_json(input,target);f.setup.add_child("subject_before",target);
            boost::property_tree::ptree stock,stacks;
            for(const auto& reagent:f.job.reagents) {
                boost::property_tree::ptree r;r.put("entry",reagent.entry);r.put("required",reagent.perAttempt);
                r.put("count",actor->GetItemCount(reagent.entry,true));stock.push_back({"",r});
                for(auto* item:actor->GetPlayerbotAI()->InventoryParseItems(std::to_string(reagent.entry),
                    IterateItemsMask(uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BAGS)|uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BANK)))) {
                    require(item && item->GetOwnerGuid()==actor->GetObjectGuid(),"original_owned_reagent_required");
                    boost::property_tree::ptree s;s.put("guid",item->GetGUIDLow());s.put("entry",item->GetEntry());
                    s.put("count",item->GetCount());s.put("bag",item->GetContainer()?item->GetContainer()->GetGUIDLow():0);
                    s.put("slot",uint32_t(item->GetSlot()));stacks.push_back({"",s});
                }
            }
            f.setup.add_child("original_reagents",stock);
            f.setup.add_child("original_reagent_stacks",stacks);
            // A disk intent exists before the single test-only native tool grant.
            std::ofstream intent("/isolated/evidence/enchant-setup-intent-"+f.task+".json");intent<<Json(f.setup);intent.close();
            require(bool(intent),"enchant_fixture_intent_saved");
            if(!f.toolBefore) {
                ItemPosCountVec positions;require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,6218,1)==EQUIP_ERR_OK,"enchant_fixture_tool_capacity");
                auto* tool=actor->StoreNewItem(positions,6218,true);require(tool!=nullptr,"enchant_fixture_tool_granted");
                f.toolGuid=tool->GetGUIDLow();f.toolGrant=1;actor->SaveToDB();
            }
            f.setup.put("tool_guid",f.toolGuid);f.setup.put("tool_grant",f.toolGrant);
            std::ofstream setup("/isolated/evidence/enchant-setup-"+f.task+".json");setup<<Json(f.setup);setup.close();
            require(bool(setup),"enchant_fixture_setup_saved");
            TaskRequest request;auto& task=request.task;task.id=task.root=f.task;task.actor=f.actor;
            task.source="profession_job";task.sourceKey="isolated_enchant_v1:"+state.boot;task.kind=Kind::Profession;
            task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Delivery;
            task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeProfessionJob(f.job);request.receipt=NewId();
            state.enforceEffects.store(true,std::memory_order_release);
            const auto admitted=sLivingActivityCoordinator.SubmitTask(request);
            require(admitted.code==AdmissionCode::Pending,"enchant_task_admission:"+admitted.blocker);
            f.started=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if(!saved){f.blocker="enchant_admission_pending";return;}
        const auto found=state.executionBlockers.find(f.task);
        f.blocker=found==state.executionBlockers.end()?saved->checkpoint.blocker:found->second;
        const auto key=std::to_string(saved->revision)+":"+f.blocker;
        if(key!=f.lastDiagnostic && f.diagnostics<128) {
            f.lastDiagnostic=key;++f.diagnostics;
            sLog.outString("Living enchant progress: actor=%u task=%s revision=%llu phase=%s step=%s blocker=%s",f.actor,f.task.c_str(),
                static_cast<unsigned long long>(saved->revision),Name(saved->phase),saved->checkpoint.step.c_str(),f.blocker.c_str());
        }
        require(!Terminal(saved->phase) || saved->phase==Phase::Completed,"enchant_ended_without_verified_success");
        if(saved->phase!=Phase::Completed)return;
        EnchantSubject after;
        require(ReadNativeEnchantSubject(*actor,f.job,after,f.blocker),"native_enchant_after_available");
        require(after.item.guid==f.subject.item.guid && after.enchantments[0].id==f.enchant.id &&
            actor->GetSkillValuePure(f.job.skill)>f.before.skill,"native_enchant_and_skill_changed");
        require(!sLivingActivityCoordinator.DefersNativeSave(f.actor) && !state.executionTimes.count(f.task),"enchant_native_save_and_queue_settled");
        f.blocker.clear();emit("passed","",actor);
    } catch(const std::exception& error){emit("failed",error.what(),actor);}
}
}
