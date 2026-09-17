// Copied realm only. Damage and the headless leader/session are explicit test
// inputs. All repair costs, travel, claims and outcomes use the normal executor.
namespace {
template<class State> void RunIsolatedPartyRepair(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0;uint64_t deadline=0;bool logged=false,damaged=false;
        std::string task,blocker;boost::property_tree::ptree setup;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    Player* bot=nullptr;
    auto emit=[&](bool passed,const std::string& why) {
        auto report=f.setup;report.put("suite","activity-party-repair-v1");report.put("boot",state.boot);
        report.put("actor",f.actor);report.put("leader",f.leader);report.put("task",f.task);
        report.put("outcome",passed?"passed":"failed");report.put("failure",why);report.put("blocker",f.blocker);
        report.put("test_only",true);report.put("ordinary_due_queue",true);report.put("independent_audit_required",true);
        if(bot){report.put("money_after",bot->GetMoney());report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));}
        if(!PartyMailFixture().done)PartyMailFixture().Close();
        std::ofstream out("/isolated/evidence/activity-party-repair-"+state.boot+".json");out<<Json(report);out.close();
        state.fixtureFinished=true;
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    try {
        if(!f.actor) {
            const auto* actor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");
            const auto* leader=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(actor && leader,"explicit_repair_pair_required");
            f.actor=uint32_t(std::stoul(actor));f.leader=uint32_t(std::stoul(leader));
            require(f.actor && f.leader && f.actor!=f.leader,"distinct_repair_pair_required");
            state.gameplayFixtureActor.store(f.actor);state.professionCohortActors.back().store(f.leader);
            f.deadline=NowMs()+600000;
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready)return;
        bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit(false,"repair_deadline_preserved_task");return;}
        if(!f.logged) {
            f.logged=true;
            for(auto id:{f.actor,f.leader})if(!sRandomPlayerbotMgr.GetPlayerBot(id))sRandomPlayerbotMgr.AddRandomBot(id);
        }
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(f.leader);
        if(!bot || !leader || !bot->IsInWorld() || !leader->IsInWorld() || NativeSafety(bot) || NativeSafety(leader))return;
        if(!f.damaged) {
            require(!bot->GetGroup() && !leader->GetGroup(),"existing_native_groups_preserved");
            require(bot->GetPlayerbotAI() && bot->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()),"copied_bot_required");
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_repair_actor_task_preserved");
            boost::property_tree::ptree items;uint32_t count=0;
            for(uint8_t slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot) {
                auto* item=bot->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);
                if(!item || item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)<4)continue;
                const auto maximum=item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
                const auto before=item->GetUInt32Value(ITEM_FIELD_DURABILITY);
                const uint32_t loss=before>maximum/3?before-maximum/3:0;
                boost::property_tree::ptree row;row.put("guid",item->GetGUIDLow());row.put("entry",item->GetEntry());
                row.put("slot",uint32_t(slot));row.put("maximum",maximum);row.put("before",before);row.put("damage_input",loss);
                items.push_back({"",row});++count;
            }
            require(count>=2,"multiple_equipped_durable_items_required");
            f.setup.add_child("equipment",items);f.setup.put("money_before",bot->GetMoney());
            f.setup.put("money_grants",0);f.setup.put("item_grants",0);f.setup.put("repair_only_window_input",true);
            std::ofstream intent("/isolated/evidence/party-repair-setup-"+state.boot+".json");intent<<Json(f.setup);intent.close();
            require(bool(intent),"repair_fixture_intent_saved");
            for(const auto& row:items)bot->DurabilityPointsLoss(bot->GetItemByPos(INVENTORY_SLOT_BAG_0,
                uint8_t(row.second.template get<uint32_t>("slot"))),row.second.template get<int32_t>("damage_input"));
            bot->SaveToDB();
            for(const auto* name:{"durability inventory","durability","repair cost","min repair cost","can repair"})
                if(auto* value=bot->GetPlayerbotAI()->GetAiObjectContext()->GetUntypedValue(name))value->Reset();
            f.damaged=true;state.enforceEffects.store(true);return;
        }
        if(f.task.empty()) {
            if(!PartyMailFixture().Prepare(state,bot)) {f.blocker="repair_native_party_window_pending";return;}
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            if(!binding){f.blocker="repair_native_admission_pending";return;}
            require(binding->service==PartyServiceBinding::Service::Repair,"repair_only_binding_required");
            f.task=binding->root;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if(!saved)return;
        f.blocker=state.executionBlockers[f.task];
        require(IsPartyRepairTask(*saved) && saved->actor==f.actor,"original_repair_identity_required");
        if(saved->phase==Phase::Completed) {
            require(!HasNativeDamagedEquipment(*bot),"all_equipped_items_repaired");
            require(bot->GetMoney()<f.setup.template get<uint32_t>("money_before"),"real_money_spent");
            f.setup.put("final_revision",saved->revision);emit(true,"");
        }
    }catch(const std::exception& error){emit(false,error.what());}
}
}
