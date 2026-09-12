// Copied realm only. Existing book, native admissions/claims/cast/save/settlement.
// The coordinator's ordinary due queue advances the task, not this fixture.
namespace {
    template<class FixtureState>
    void RunIsolatedManagedRecipeFixture(FixtureState& state,bool prepareOnly,bool restart,bool service=false) {
        if (state.fixtureFinished || NowMs()<state.fixtureNext) return;
        state.fixtureNext=NowMs()+500;auto& f=state.recipeLearningFixture;
        auto books=[&](Player& actor) {
            std::map<uint32_t,uint32_t> found;
            for (auto* item:actor.GetPlayerbotAI()->InventoryParseItems(std::to_string(f.item),
                service?IterateItemsMask(uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BAGS)|uint8_t(IterateItemsMask::ITERATE_ITEMS_IN_BANK)):IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
                found.emplace(item->GetGUIDLow(),item->GetCount());
            if (service) for (auto it=actor.GetMailBegin();it!=actor.GetMailEnd();++it) {
                const auto* mail=*it;if (!mail || mail->state==MAIL_STATE_DELETED || mail->expire_time<=time(nullptr)) continue;
                for (const auto& attachment:mail->items) if (attachment.item_template==f.item)
                    if (auto* item=actor.GetMItem(attachment.item_guid)) found.emplace(item->GetGUIDLow(),item->GetCount());
            }
            return found;
        };
        auto count=[](const auto& items) {uint32_t n=0;for (const auto& item:items) n+=item.second;return n;};
        auto emit=[&](const std::string& outcome,const std::string& failure,Player* actor) {
            boost::property_tree::ptree report,items;
            report.put("suite","activity-managed-recipe-v1");report.put("boot",state.boot);
            report.put("test_only",true);report.put("outcome",outcome);report.put("failure",failure);
            report.put("prepare_only",prepareOnly);report.put("restart",restart);report.put("task",f.task);
            report.put("service_workflow",service);
            report.put("actor",f.actor);report.put("item",f.item);report.put("recipe",f.recipe);report.put("skill",f.skill);
            report.put("skill_before",f.skillBefore);report.put("money_before",f.moneyBefore);report.put("count_before",f.countBefore);
            report.put("item_grants",0);report.put("money_grants",0);report.put("spell_grants",0);report.put("skill_grants",0);
            report.put("fixture_relocations",0);report.put("new_tasks",restart?0:1);
            report.put("blocker",f.blocker);report.put("checks",f.checks.size());
            report.put("general_execution_enabled",state.effective==Mode::Active);
            for (const auto& row:f.beforeStacks) {boost::property_tree::ptree item;item.put("guid",row.first);item.put("quantity",row.second);items.push_back({"",item});}
            report.add_child("original_books",items);report.add_child("assertions",f.checks);
            if (actor) {report.put("known_after",actor->HasSpell(f.recipe));report.put("count_after",count(books(*actor)));
                report.put("money_after",actor->GetMoney());report.put("skill_after",actor->GetSkillValuePure(f.skill));
                report.put("stopped",actor->IsStopped());report.put("mails_updated",actor->m_mailsUpdated);
                report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));}
            if (const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task)) {
                report.put("phase",Name(saved->phase));report.put("revision",saved->revision);
            }
            state.enforceEffects.store(f.priorEnforcement,std::memory_order_release);state.fixtureFinished=true;
            std::ofstream out("/isolated/evidence/activity-managed-recipe-"+state.boot+".json");out<<Json(report);out.close();
            if (!out) sLog.outError("Living managed recipe evidence write failed");
            sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        };
        auto require=[&](bool ok,const char* name) {
            boost::property_tree::ptree row;row.put("case",name);row.put("passed",ok);f.checks.push_back({"",row});
            if (!ok) throw std::runtime_error(name);
        };
        Player* actor=nullptr;
        try {
            if (!f.actor) {
                auto id=[](const char* key) {
                    const auto* value=std::getenv(key);const std::string text=value?value:"";
                    if (text.empty() || text.size()>9 || text.find_first_not_of("0123456789")!=std::string::npos)
                        throw std::runtime_error("explicit_managed_recipe_actor_and_item_required");
                    return uint32_t(std::stoul(text));
                };
                f.actor=id("LIVING_WOW_NATIVE_FIXTURE_ACTOR");f.item=id("LIVING_WOW_NATIVE_FIXTURE_ITEM");
                f.priorEnforcement=state.enforceEffects.load();state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            }
            if (sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
                !state.incoming.empty() || !state.resources.Protection().ready) return;
            if (!f.deadline) f.deadline=NowMs()+(service?600000:180000);
            actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
            if (NowMs()>f.deadline) {emit("failed","managed_recipe_deadline",actor);return;}
            if (!actor) {
                if (!f.requestedLogin && sRandomPlayerbotMgr.GetAllBots().size()<49) {f.requestedLogin=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
                f.blocker="native_actor_login_pending";return;
            }
            if (!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() || actor->GetTradeData() ||
                actor->GetMap()->IsDungeon()) {f.blocker="native_safety_wait";return;}
            if (!f.started) {
                require(!actor->isRealPlayer() && actor->GetPlayerbotAI() && actor->GetSession() &&
                    sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"copied_native_bot_actor");
                RecipeLearningJob job;
                require(BuildNativeRecipeLearningJob(*actor,f.item,job,f.blocker),"native_unknown_useful_recipe");
                f.recipe=job.recipe;f.skill=job.skill;f.skillBefore=actor->GetSkillValuePure(job.skill);f.moneyBefore=actor->GetMoney();
                f.beforeStacks=books(*actor);f.countBefore=count(f.beforeStacks);
                if (!service) require(f.countBefore>0,"existing_carried_book_not_granted");
                f.task=SourceId("recipe_learning","actor:"+std::to_string(f.actor)+":recipe:"+std::to_string(f.recipe));
                for (const auto& row:state.cache) if (row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                    require(restart && row.first==f.task,"no_other_accepted_task_displaced");
                state.enforceEffects.store(true,std::memory_order_release);
                if (restart) require(bool(sLivingActivityCoordinator.ReadSavedTask(f.task)),"same_task_restored_no_new_admission");
                else {
                    require(!sLivingActivityCoordinator.ReadSavedTask(f.task),"fresh_learning_task_identity");
                    const auto admitted=sLivingActivityCoordinator.AdmitRecipeLearning(f.actor,f.item);
                    require(admitted.code==AdmissionCode::Pending && admitted.task==f.task,"ordinary_native_learning_admission");
                }
                f.started=true;return;
            }
            const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
            if (!saved) {f.blocker="task_admission_pending";return;}
            const auto work=state.executionBlockers.find(f.task);
            f.blocker=work==state.executionBlockers.end()?saved->checkpoint.blocker:work->second;
            const auto diagnostic=std::to_string(saved->revision)+":"+f.blocker;
            if (f.diagnosticEvents<128 && diagnostic!=f.diagnosticKey) {
                f.diagnosticKey=diagnostic;++f.diagnosticEvents;
                boost::property_tree::ptree progress;
                progress.put("task",f.task);progress.put("actor",f.actor);progress.put("revision",saved->revision);
                progress.put("phase",Name(saved->phase));progress.put("step",saved->checkpoint.step);
                progress.put("blocker",f.blocker);progress.put("stopped",actor->IsStopped());
                progress.put("mails_updated",actor->m_mailsUpdated);progress.put("native_safety",NativeSafety(actor));
                progress.put("map",actor->GetMapId());progress.put("x",actor->GetPositionX());progress.put("y",actor->GetPositionY());
                sLog.outString("Living isolated recipe progress: %s",Json(progress).c_str());
            }
            if (prepareOnly && saved->phase==Phase::Preparing) {
                UnsettledClaimBatch claims;
                if (!sLivingActivityCoordinator.ReadTaskClaims(f.actor,f.task,saved->revision,claims,f.blocker) || claims.claims.empty()) return;
                require(claims.claims.size()==1 && claims.claims.front().itemEntry==f.item &&
                    f.beforeStacks.count(claims.claims.front().itemGuid) && claims.claims.front().state=="held","original_book_claim_saved_before_restart");
                require(!actor->HasSpell(f.recipe) && actor->GetItemCount(f.item,false)==f.countBefore,"prepared_book_not_used");
                emit("prepared","",actor);return;
            }
            if (saved->phase!=Phase::Completed) return;
            require(actor->HasSpell(f.recipe),"native_spell_learned");
            if (f.countBefore) require(count(books(*actor))+1==f.countBefore,"one_original_physical_book_consumed");
            else require(count(books(*actor))==0,"purchased_book_not_duplicated");
            require((service || actor->GetMoney()==f.moneyBefore) && actor->GetSkillValuePure(f.skill)==f.skillBefore,"no_unrelated_skill_change");
            require(!sLivingActivityCoordinator.DefersNativeSave(f.actor),"native_guarded_save_acknowledged");
            require(!state.executionTimes.count(f.task),"completed_task_removed_from_due_queue");
            f.blocker.clear();emit("passed","",actor);
        } catch (const std::exception& error) {emit("failed",error.what(),actor);}
    }
}
