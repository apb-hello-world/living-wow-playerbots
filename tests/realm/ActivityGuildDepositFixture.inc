// Copied realm only. Eight labeled ore items are fixture input; the ordinary
// coordinator must deliver exactly four through a real guild-bank operation.
// No gold, rank, permission, location or bank content is granted by this fixture.
namespace {
template<class FixtureState>
void RunIsolatedGuildDepositFixture(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;auto& f=state.guildDepositFixture;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool passed,const std::string& why,Player* actor) {
        auto report=f.setup;report.put("suite","activity-guild-deposit-v1");report.put("boot",state.boot);
        report.put("task",f.task);report.put("actor",f.actor);report.put("test_only",true);
        report.put("original_boot",f.originalBoot);report.put("resumed_same_task",f.resuming);
        report.put("outcome",passed?"passed":"failed");report.put("failure",why);report.put("blocker",f.blocker);
        report.put("ordinary_due_queue",true);report.put("independent_audit_required",true);
        if(actor && f.started) {
            report.put("money_after",actor->GetMoney());report.put("bags_after",actor->GetItemCount(f.job.entry,false));
            report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
            auto* guild=sGuildMgr.GetGuildById(f.job.guild);
            if(guild){const auto counts=guild->GetBankItemCounts();const auto it=counts.find(f.job.entry);
                report.put("bank_after",it==counts.end()?0:it->second);}
        }
        std::ofstream out("/isolated/evidence/activity-guild-deposit-"+state.boot+".json");out<<Json(report);out.close();
        state.fixtureFinished=true;if(!out)sLog.outError("Living guild deposit evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* actor=nullptr;
    try {
        if(!f.actor) {
            const auto* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const std::string id=raw?raw:"";
            require(!id.empty() && id.size()<=9 && id.find_first_not_of("0123456789")==std::string::npos,"explicit_guild_actor_required");
            f.actor=uint32_t(std::stoul(id));require(f.actor!=0,"explicit_guild_actor_required");f.originalBoot=state.boot;
            const auto* original=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
            if(original && *original){require(IsUuid(original),"exact_guild_resume_task_required");f.task=original;f.resuming=true;}
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.deadline)f.deadline=NowMs()+600000;
        actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit(false,"guild_deposit_deadline_with_preserved_task",actor);return;}
        if(!actor) {
            if(!f.requested && sRandomPlayerbotMgr.GetAllBots().size()<49){f.requested=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
            f.blocker="guild_deposit_native_login_pending";return;
        }
        if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() || actor->GetTradeData() ||
            actor->GetMap()->IsDungeon()){f.blocker="guild_deposit_native_safety_wait";return;}
        if(!f.started) {
            require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer() && actor->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"copied_native_guild_bot_required");
            if(f.resuming) {
                const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
                require(saved && saved->actor==f.actor && ValidateGuildDeliveryTask(*saved,f.blocker) &&
                    DecodeGuildDeliveryJob(saved->checkpoint.data,f.job,f.blocker),"original_guild_job_required");
                std::ifstream setup("/isolated/evidence/guild-deposit-setup-"+f.task+".json",std::ios::ate);
                require(setup && setup.tellg()>0 && setup.tellg()<32768,"original_guild_setup_required");
                setup.seekg(0);boost::property_tree::read_json(setup,f.setup);
                f.originalBoot=f.setup.template get<std::string>("original_boot");
                require(IsUuid(f.originalBoot) && f.setup.template get<uint32_t>("actor")==f.actor &&
                    f.setup.template get<std::string>("task")==f.task &&
                    f.setup.template get<std::string>("job")==saved->checkpoint.data,"original_guild_identity_changed");
                state.enforceEffects.store(true,std::memory_order_release);f.started=f.admitted=true;return;
            }
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_guild_actor_obligation_preserved");
            auto* guild=sGuildMgr.GetGuildById(actor->GetGuildId());
            require(guild && guild->GetPurchasedTabs(),"existing_native_guild_bank_required");
            bool allowed=false;for(uint8_t tab=0;tab<guild->GetPurchasedTabs();++tab)
                allowed|=guild->IsMemberHaveRights(f.actor,tab,GUILD_BANK_RIGHT_DEPOSIT_ITEM);
            require(allowed,"existing_native_deposit_permission_required");
            require(!actor->GetItemCount(2770,true),"fixture_ore_must_not_mix_with_existing_stock");
            ItemPosCountVec positions;
            require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,2770,8)==EQUIP_ERR_OK,"guild_fixture_input_capacity");
            const auto counts=guild->GetBankItemCounts();const auto stock=counts.find(2770);
            const uint32_t bank=stock==counts.end()?0:stock->second;require(bank<100000,"fixture_bank_count_bound");
            f.job={NowMs()*256+guild->GetId(),guild->GetId(),f.actor,2770,4,0,"isolated_guild_"+state.boot,false};
            f.task=SourceId("guild_delivery",GuildDeliverySourceKey(f.job,f.actor));
            f.setup.put("actor",f.actor);f.setup.put("task",f.task);f.setup.put("original_boot",f.originalBoot);
            f.setup.put("job",EncodeGuildDeliveryJob(f.job));f.setup.put("bank_before",bank);f.setup.put("bags_before",0);
            f.setup.put("money_before",actor->GetMoney());f.setup.put("item_grants",8);f.setup.put("requested_quantity",4);
            f.setup.put("money_grants",0);f.setup.put("permission_grants",0);f.setup.put("fixture_relocations",0);
            std::ofstream intent("/isolated/evidence/guild-deposit-intent-"+f.task+".json");intent<<Json(f.setup);intent.close();
            require(bool(intent),"guild_fixture_intent_saved");
            auto* item=actor->StoreNewItem(positions,2770,true);require(item && item->GetCount()==8,"guild_fixture_input_supplied");
            f.setup.put("item_guid",item->GetGUIDLow());actor->SaveToDB();
            CharacterDatabase.PExecute("INSERT INTO guild_society_supply_goal(goal_id,guild_id,goal_type,item_entry,required_quantity,state,created_at,updated_at,provenance,request_kind,purpose) VALUES('%s',%u,'supply',2770,%u,'active',%u,%u,'isolated_fixture','item','Isolated native partial-stack deposit test')",
                f.job.goal.c_str(),f.job.guild,bank+4,uint32_t(time(nullptr)),uint32_t(time(nullptr)));
            CharacterDatabase.PExecute("INSERT INTO guild_society_supply_delivery(delivery_id,guild_id,goal_id,donor_guid,carrier_guid,item_guid,item_entry,quantity,created_at,updated_at) VALUES(%llu,%u,'%s',%u,%u,%u,2770,4,%u,%u)",
                (unsigned long long)f.job.delivery,f.job.guild,f.job.goal.c_str(),f.actor,f.actor,item->GetGUIDLow(),uint32_t(time(nullptr)),uint32_t(time(nullptr)));
            std::ofstream setup("/isolated/evidence/guild-deposit-setup-"+f.task+".json");setup<<Json(f.setup);setup.close();
            require(bool(setup),"guild_fixture_setup_saved");
            state.enforceEffects.store(true,std::memory_order_release);f.started=true;return;
        }
        if(!f.admitted) {
            GuildDeliveryJob native;
            if(!sGuildSupplies.ReadDeliveryJob(f.job.delivery,f.actor,native,f.blocker))return;
            require(EncodeGuildDeliveryJob(native)==EncodeGuildDeliveryJob(f.job),"guild_fixture_native_intent_changed");
            const auto admitted=sLivingActivityCoordinator.AdmitGuildDelivery(f.actor,native);f.blocker=admitted.blocker;
            if(admitted.code==AdmissionCode::NotReady)return;
            require(admitted.code==AdmissionCode::Pending || admitted.code==AdmissionCode::Saved,"guild_fixture_admission:"+f.blocker);
            f.admitted=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);if(!saved){f.blocker="guild_deposit_admission_pending";return;}
        const auto found=state.executionBlockers.find(f.task);f.blocker=found==state.executionBlockers.end()?saved->checkpoint.blocker:found->second;
        const auto key=std::to_string(saved->revision)+":"+f.blocker;
        if(key!=f.lastDiagnostic && f.diagnostics<128){f.lastDiagnostic=key;++f.diagnostics;
            sLog.outString("Living guild deposit progress: actor=%u task=%s revision=%llu phase=%s step=%s blocker=%s",f.actor,f.task.c_str(),
                (unsigned long long)saved->revision,Name(saved->phase),saved->checkpoint.step.c_str(),f.blocker.c_str());}
        require(!Terminal(saved->phase) || saved->phase==Phase::Completed,"guild_job_ended_without_verified_success");
        if(saved->phase!=Phase::Completed)return;
        require(actor->GetItemCount(2770,false)==4 && actor->GetMoney()==f.setup.template get<uint32_t>("money_before"),"guild_partial_stack_and_wallet_conserved");
        require(!sLivingActivityCoordinator.DefersNativeSave(f.actor) && !sLivingActivityCoordinator.DefersGuildMutation(f.job.guild) &&
            !state.executionTimes.count(f.task),"guild_native_save_and_queue_settled");
        f.blocker.clear();emit(true,"",actor);
    } catch(const std::exception& error){emit(false,error.what(),actor);}
}
}
