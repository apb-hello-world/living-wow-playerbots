// Isolated binary only. No fabricated craft/send receipts, item or money
// grants. The recorded agreement is fixture consent, not a human request.
// Ordinary preparation, timed native crafting and mail own the actual work.
namespace {
template<class FixtureState>
void RunIsolatedCommissionMailFixture(FixtureState& state) {
    struct Fixture {
        uint32_t actor=0,recipient=0,money=0,output=0;
        uint64_t deadline=0;
        bool login=false,setup=false,admitted=false;
        TaskRequest request;
        std::string blocker;
        boost::property_tree::ptree input;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool sent,const std::string& why,Player* actor) {
        auto report=f.input;
        report.put("suite","activity-commission-mail-v1");report.put("boot",state.boot);
        report.put("test_only",true);report.put("outcome",sent?"sent_checkpoint":"failed");
        report.put("failure",why);report.put("blocker",f.blocker);report.put("task",f.request.task.id);
        report.put("actor",f.actor);report.put("recipient",f.recipient);
        report.put("independent_audit_required",true);report.put("commission_completion_proof",false);
        report.put("customer_received_proof",false);report.put("fee_collected_proof",false);
        if(actor) {
            report.put("money_after",actor->GetMoney());report.put("output_after",actor->GetItemCount(2454,true));
            report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
            report.add_child("inventory_after",GuildMailFixtureInventory(*actor));
        }
        std::ofstream out("/isolated/evidence/activity-commission-mail-"+state.boot+".json");
        out<<Json(report);out.close();state.fixtureFinished=true;
        if(!out)sLog.outError("Living commission fixture evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* actor=nullptr;
    try {
        if(!f.actor) {
            auto id=[&](const char* name) {
                const auto* raw=std::getenv(name);const std::string value=raw?raw:"";
                require(!value.empty() && value.size()<=9 && value.find_first_not_of("0123456789")==std::string::npos,
                    "explicit_commission_fixture_actors_required");
                const auto n=uint32_t(std::stoul(value));require(n!=0,"explicit_commission_fixture_actors_required");return n;
            };
            f.actor=id("LIVING_WOW_NATIVE_FIXTURE_ACTOR");f.recipient=id("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(f.actor!=f.recipient,"distinct_commission_customer_required");
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            // If ordinary population loading brings the customer online, keep
            // its AI from collecting this parcel before the independent audit.
            state.professionCohortActors[0].store(f.recipient,std::memory_order_release);
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.deadline)f.deadline=NowMs()+900000;
        actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit(false,"commission_fixture_deadline_task_preserved",actor);return;}
        if(!actor) {
            if(!f.login && sRandomPlayerbotMgr.GetAllBots().size()<49){f.login=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
            f.blocker="commission_fixture_actor_login_pending";return;
        }
        require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer() && actor->GetSession() &&
            sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"native_fixture_bot_required");
        if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() ||
            actor->GetTradeData() || actor->GetMap()->IsDungeon()){f.blocker="commission_fixture_safety_wait";return;}
        state.enforceEffects.store(true,std::memory_order_release);
        if(!f.setup) {
            require(!std::ifstream("/isolated/evidence/commission-mail-input-v1.json"),
                "existing_commission_fixture_must_be_reconciled_not_recreated");
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_commission_actor_obligation_preserved");
            auto customer=CharacterDatabase.PQuery("SELECT account,online,money FROM characters WHERE guid=%u",f.recipient);
            require(customer && !customer->Fetch()[1].GetUInt32() &&
                sPlayerbotAIConfig.IsInRandomAccountList(customer->Fetch()[0].GetUInt32()) &&
                actor->GetTeam()==sObjectMgr.GetPlayerTeamByGUID(ObjectGuid(HIGHGUID_PLAYER,f.recipient)),
                "offline_same_faction_native_bot_customer_required");
            require(actor->HasSpell(2329) && actor->GetSkillValuePure(171)>0 && actor->GetMoney()>=30,
                "existing_recipe_profession_and_postage_required");
            ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=actor->GetSkillValuePure(171);
            recipe.outputEntry=2454;recipe.outputQuantity=1;recipe.purpose=ProfessionPurpose::RequestedItem;
            recipe.reagents={{765,1},{2449,1},{3371,1}};
            // Bound this first native parcel scenario to already-owned inputs.
            // Separate paid sourcing scenarios retain their own evidence.
            for(const auto& reagent:recipe.reagents)
                require(actor->GetItemCount(reagent.entry,false)>=reagent.perAttempt,"owned_native_commission_inputs_required");
            CommissionJob job;job.agreement={"lwc-"+std::to_string(NowMs()),"isolated-commission-mail-v1","mail",
                EncodeProfessionJob(recipe),f.actor,f.recipient,60,NowMs()};job.craft=job.agreement.recipe;
            auto& task=f.request.task;task.source="commission_job";task.sourceKey=job.agreement.id;
            task.id=task.root=SourceId(task.source,task.sourceKey);task.actor=f.actor;task.kind=Kind::Commission;
            task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Human;
            task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeCommissionJob(job);f.request.receipt=NewId();
            require(ValidateNativeProfessionTask(*actor,task,f.blocker),"native_commission_recipe:"+f.blocker);
            f.money=actor->GetMoney();f.output=actor->GetItemCount(2454,true);
            f.input.put("task",task.id);f.input.put("commission",job.agreement.id);f.input.put("actor",f.actor);
            f.input.put("recipient",f.recipient);f.input.put("agreement",EncodeCommissionContract(job.agreement));
            f.input.put("money_before",f.money);f.input.put("output_before",f.output);
            f.input.put("customer_money_before",customer->Fetch()[2].GetUInt32());
            f.input.put("item_grants",0);f.input.put("money_grants",0);f.input.put("skill_grants",0);
            f.input.put("recipe_grants",0);f.input.put("fixture_consent",true);f.input.put("ordinary_due_queue",true);
            f.input.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            std::ofstream intent("/isolated/evidence/commission-mail-input-v1.json");intent<<Json(f.input);intent.close();
            require(bool(intent),"commission_fixture_intent_saved");
            const auto sql="INSERT INTO organic_economy_commission(commission_id,bot_guid,player_guid,recipe_spell_id,"
                "output_item_entry,quantity,materials_source,service_fee_copper,state,authoritative_payload,expires_at) VALUES("+
                SqlValue(job.agreement.id)+','+std::to_string(f.actor)+','+std::to_string(f.recipient)+
                ",2329,2454,1,'bot',60,'crafting',"+SqlValue(EncodeCommissionContract(job.agreement))+",DATE_ADD(NOW(),INTERVAL 1 DAY))";
            CharacterDatabase.Execute(sql.c_str());f.setup=true;return;
        }
        if(!f.admitted) {
            const auto result=sLivingActivityCoordinator.SubmitTask(f.request);f.blocker=result.blocker;
            require(result.code==AdmissionCode::Pending,"commission_fixture_admission:"+f.blocker);f.admitted=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.request.task.id);
        if(!saved){f.blocker="commission_fixture_admission_pending";return;}
        const auto blocker=state.executionBlockers.find(saved->id);
        f.blocker=blocker==state.executionBlockers.end()?saved->checkpoint.blocker:blocker->second;
        require(!Terminal(saved->phase),"commission_fixture_premature_terminal_state");
        ProfessionHistory history;CommissionDeliveryProof proof;
        if(!sLivingActivityCoordinator.ReadProfessionHistory(f.actor,saved->id,saved->revision,history,f.blocker) ||
            !InspectCommissionDelivery(*saved,history,proof,f.blocker))return;
        if(proof.send.empty())return;
        require(!sLivingActivityCoordinator.DefersNativeSave(f.actor),"commission_fixture_native_save_pending");
        require(actor->GetMoney()==f.money-30 && actor->GetItemCount(2454,true)==f.output,
            "commission_fixture_native_stock_or_postage_mismatch");
        f.input.put("send_operation",proof.send);f.input.put("mail",proof.mail);
        f.input.put("sent_item",proof.quote.item);f.input.put("native_send_verified",true);
        emit(true,"",actor);
    } catch(const std::exception& error){emit(false,error.what(),actor);}
}
}
