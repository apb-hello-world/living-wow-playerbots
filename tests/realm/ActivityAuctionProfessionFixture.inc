// Marked copied realm only. One existing native auction, real payment, attached
// material, collection and craft under one job. The due queue does all work.
// A bounded prerequisite recipe/herb setup is explicit test data, NOT proof of
// organic learning or gathering. Money, auction stock and skills are not granted.
namespace {
    template<class FixtureState>
    void RunIsolatedAuctionProfessionFixture(FixtureState& state,bool restart,bool partial=false) {
        if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
        state.fixtureNext=NowMs()+500;auto& f=state.auctionProfessionFixture;
        Player* actor=nullptr;
        auto require=[&](bool okay,const std::string& name) {
            boost::property_tree::ptree row;row.put("case",name);row.put("passed",okay);f.checks.push_back({"",row});
            if(!okay)throw std::runtime_error(name);
        };
        auto emit=[&](const char* outcome,const std::string& failure) {
            boost::property_tree::ptree report;
            report.put("suite","activity-profession-auction-v1");report.put("boot",state.boot);
            report.put("outcome",outcome);report.put("failure",failure);report.put("test_only",true);
            report.put("restart",restart);report.put("task",f.task);report.put("actor",f.actor);
            report.put("partial_purchases",partial);
            report.put("recipe",2330);report.put("required_auction_material",2447);
            report.put("money_before",f.moneyBefore);report.put("skill_before",f.skillBefore);
            report.put("output_before",f.outputBefore);report.put("blocker",f.blocker);
            report.put("money_grants",0);report.put("skill_grants",0);report.put("auction_stock_grants",0);
            report.put("fixture_relocations",0);report.put("organic_recipe_learning_proof",false);
            report.put("native_receipts_require_independent_sql_audit",true);
            report.add_child("setup",f.setup);report.add_child("checks",f.checks);
            report.put("general_execution_enabled",state.effective==Mode::Active);
            if(actor) {
                report.put("money_after",actor->GetMoney());report.put("skill_after",actor->GetSkillValuePure(SKILL_ALCHEMY));
                report.put("output_after",actor->GetItemCount(118,true));
                report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
            }
            if(const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task)) {
                report.put("phase",Name(saved->phase));report.put("revision",saved->revision);
            }
            state.enforceEffects.store(f.priorEnforcement,std::memory_order_release);state.fixtureFinished=true;
            std::ofstream out("/isolated/evidence/activity-auction-profession-"+state.boot+".json");
            out<<Json(report);out.close();if(!out)sLog.outError("Living auction profession evidence write failed");
            sLog.outString("Living isolated fixture: %s",Json(report).c_str());
            // Keep the test-only actor hold until explicit restart, preserving
            // native outcomes for independent inspection instead of a new job.
        };
        try {
            if(!f.actor) {
                const char* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const std::string value=raw?raw:"";
                require(!value.empty() && value.size()<=9 && value.find_first_not_of("0123456789")==std::string::npos,
                    "explicit_copied_auction_actor_required");
                f.actor=uint32_t(std::stoul(value));require(f.actor!=0,"nonzero_copied_actor");
                f.priorEnforcement=state.enforceEffects.load();state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            }
            if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
                !state.incoming.empty() || !state.resources.Protection().ready)return;
            if(!f.deadline)f.deadline=NowMs()+600000;
            actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
            if(NowMs()>f.deadline){emit("failed","auction_profession_deadline");return;}
            if(!actor) {
                if(!f.requestedLogin && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                    f.requestedLogin=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);
                }
                f.blocker="native_actor_login_pending";return;
            }
            if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() ||
                actor->GetTradeData() || actor->GetMap()->IsDungeon()) {f.blocker="native_safety_wait";return;}
            if(!f.started) {
                require(!actor->isRealPlayer() && actor->GetPlayerbotAI() && actor->GetSession() &&
                    sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"copied_native_bot_actor");
                f.moneyBefore=actor->GetMoney();f.skillBefore=actor->GetSkillValuePure(SKILL_ALCHEMY);
                f.outputBefore=actor->GetItemCount(118,true);
                if(restart) {
                    const char* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");f.task=raw?raw:"";
                    require(IsUuid(f.task),"explicit_original_task_required");
                    const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);ProfessionJob job;
                    require(saved && saved->actor==f.actor && saved->sourceKey.find("isolated_auction_profession_v1:")==0 &&
                        DecodeProfessionJob(saved->checkpoint.data,job,f.blocker) && job.recipe==2330 && job.skill==SKILL_ALCHEMY &&
                        !Terminal(saved->phase),"same_unfinished_native_task_restored");
                    f.setup.put("skill_at_resume",f.skillBefore);f.skillBefore=job.initialSkill;
                    f.setup.put("original_setup_boot",saved->sourceKey.substr(std::string("isolated_auction_profession_v1:").size()));
                    f.setup.put("spell_grants",0);f.setup.put("item_grants",0);f.setup.put("new_tasks",0);
                } else {
                    for(const auto& row:state.cache) if(row.second.actor==f.actor && row.second.mode==Mode::Active)
                        require(Terminal(row.second.phase),"no_accepted_work_displaced");
                    require(f.skillBefore>0 && f.skillBefore<10 && actor->HasSpell(2259) && actor->GetMoney()>=500,
                        "existing_apprentice_and_earned_wallet");
                    require(actor->GetItemCount(2447,true)==0 && actor->GetItemCount(3371,true)>0,
                        "missing_herb_and_existing_native_vial");
                    for(auto it=actor->GetMailBegin();it!=actor->GetMailEnd();++it)
                        for(const auto& attachment:(*it)->items)
                            require(attachment.item_template!=2447 && attachment.item_template!=765 && attachment.item_template!=3371,
                                "no_ambiguous_existing_material_mail");
                    if(!NativeAuctionSourceAvailable(*actor,2447,1,f.blocker))return;
                    const auto owned=actor->GetItemCount(765,true);
                    require(owned==0 || actor->GetItemCount(765,false)>=1,"no_fixture_bank_item_copy");
                    if(partial) {
                        require(!owned,"partial_purchase_starts_without_either_herb");
                        boost::property_tree::ptree prepared;
                        boost::property_tree::read_json("/isolated/evidence/partial-auction-setup-v1.json",prepared);
                        require(prepared.get<std::string>("outcome")=="prepared" &&
                            prepared.get<std::string>("environment")=="isolated-migration" &&
                            prepared.get<uint32_t>("buyer")==f.actor && prepared.get<uint32_t>("entry")==765 &&
                            prepared.get<uint32_t>("quantity")==1 && prepared.get<uint32_t>("itemGrants")==0 &&
                            prepared.get<uint32_t>("moneyGrants")==0 && prepared.get<uint32_t>("skillGrants")==0 &&
                            prepared.get<uint32_t>("spellGrants")==0 && prepared.get<uint32_t>("fixtureListings")==1,
                            "explicit_existing_item_escrow_fixture_required");
                        const auto* material=sAuctionMgr.GetAItem(prepared.get<uint32_t>("item"));
                        require(material && material->GetEntry()==765 && material->GetCount()==1,
                            "fixture_stock_loaded_as_real_auction_item");
                        if(!NativeAuctionSourceAvailable(*actor,765,1,f.blocker))return;
                        f.setup.add_child("partial_auction_fixture",prepared);
                    }
                    ItemPosCountVec destinations;
                    if(!owned && !partial)require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,765,1)==EQUIP_ERR_OK,
                        "bounded_fixture_material_capacity");
                    f.setup.put("spell_grants",actor->HasSpell(2330)?0:1);f.setup.put("recipe_granted",actor->HasSpell(2330)?0:2330);
                    f.setup.put("item_grants",owned || partial ? 0:1);f.setup.put("item_entry",765);f.setup.put("new_tasks",1);
                    f.task=SourceId("profession_job","isolated_auction_profession_v1:"+state.boot);
                    const auto intentPath="/isolated/evidence/auction-profession-setup-"+state.boot+".json";
                    require(!std::ifstream(intentPath).good(),"setup_never_replayed_after_uncertain_effect");
                    boost::property_tree::ptree intent=f.setup;intent.put("actor",f.actor);intent.put("task",f.task);
                    intent.put("money_before",f.moneyBefore);intent.put("skill_before",f.skillBefore);
                    std::ofstream intentFile(intentPath);intentFile<<Json(intent);intentFile.close();
                    require(bool(intentFile),"setup_intent_saved_before_prerequisite_grants");
                    if(!actor->HasSpell(2330))actor->learnSpell(2330,false);
                    if(!owned && !partial) {
                        auto* item=actor->StoreNewItem(destinations,765,true);
                        require(item && actor->GetItemCount(765,false)==1,"explicit_one_silverleaf_prerequisite");
                        f.setup.put("item_guid",item->GetGUIDLow());
                    }
                    require(actor->GetMoney()==f.moneyBefore && actor->GetSkillValuePure(SKILL_ALCHEMY)==f.skillBefore,
                        "setup_did_not_grant_money_or_skill");
                    ProfessionJob job;require(BuildNativeSkillGainJob(*actor,2330,job,f.blocker) && job.outputEntry==118,
                        "native_known_useful_recipe");
                    require(job.reagents==std::vector<ProfessionReagent>({{765,1},{2447,1},{3371,1}}),"exact_native_recipe_reagents");
                    actor->SaveToDB();state.enforceEffects.store(true,std::memory_order_release);
                    TaskRequest request;auto& task=request.task;task.id=task.root=f.task;task.actor=f.actor;
                    task.source="profession_job";task.sourceKey="isolated_auction_profession_v1:"+state.boot;
                    task.kind=Kind::Profession;task.mode=Mode::Active;task.phase=Phase::Queued;
                    task.accepted=true;task.priority=Priority::Delivery;
                    task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);
                    task.createdAtMs=task.updatedAtMs=NowMs();task.checkpoint.step="profession_prepare";
                    task.checkpoint.data=EncodeProfessionJob(job);request.receipt=NewId();
                    const auto admission=sLivingActivityCoordinator.SubmitTask(request);
                    f.blocker=admission.blocker;
                    require(admission.code==AdmissionCode::Pending,"ordinary_native_task_admission:"+admission.blocker);
                }
                state.enforceEffects.store(true,std::memory_order_release);f.started=true;return;
            }
            const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
            if(!saved){f.blocker="task_admission_pending";return;}
            const auto found=state.executionBlockers.find(f.task);
            f.blocker=found==state.executionBlockers.end()?saved->checkpoint.blocker:found->second;
            const auto key=std::to_string(saved->revision)+":"+f.blocker;
            if(f.diagnostics<128 && key!=f.lastDiagnostic) {
                ++f.diagnostics;f.lastDiagnostic=key;boost::property_tree::ptree progress;
                progress.put("task",f.task);progress.put("actor",f.actor);progress.put("revision",saved->revision);
                progress.put("phase",Name(saved->phase));progress.put("step",saved->checkpoint.step);progress.put("blocker",f.blocker);
                progress.put("map",actor->GetMapId());progress.put("x",actor->GetPositionX());progress.put("y",actor->GetPositionY());
                progress.put("native_safety",NativeSafety(actor));progress.put("stopped",actor->IsStopped());
                sLog.outString("Living isolated auction progress: %s",Json(progress).c_str());
            }
            if(saved->phase!=Phase::Completed)return;
            require(actor->GetSkillValuePure(SKILL_ALCHEMY)>f.skillBefore,"native_skill_gain");
            require(restart || actor->GetItemCount(118,true)>f.outputBefore,"native_recipe_output_or_original_receipt_audit_required");
            require(restart || actor->GetMoney()<f.moneyBefore,"earned_money_spent_on_real_purchase");
            require(!sLivingActivityCoordinator.DefersNativeSave(f.actor),"native_transaction_acknowledged");
            require(!state.executionTimes.count(f.task),"completed_job_removed_from_due_queue");
            f.blocker.clear();emit("passed","");
        } catch(const std::exception& error) {emit("failed",error.what());}
    }
}
