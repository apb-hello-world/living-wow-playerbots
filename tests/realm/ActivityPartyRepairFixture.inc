// Copied realm only. Damage and the headless leader/session are explicit test
// inputs. All repair costs, travel, claims and outcomes use the normal executor.
namespace {
template<class State> void RunIsolatedPartyRepair(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0;uint64_t deadline=0;bool logged=false,damaged=false;
        std::string task,blocker,pausedEquipment;boost::property_tree::ptree setup;
        bool recalled=false,resumed=false;uint64_t recalledAt=0;uint32_t pausedMoney=0;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool recall=suite=="activity-party-repair-recall-v1";
    const bool checkpoint=suite=="activity-party-repair-checkpoint-v1";
    const bool restart=suite=="activity-party-repair-resume-v1";
    Player* bot=nullptr;
    auto emit=[&](bool passed,const std::string& why) {
        auto report=f.setup;report.put("suite",suite);report.put("boot",state.boot);
        report.put("actor",f.actor);report.put("leader",f.leader);report.put("task",f.task);
        report.put("outcome",passed?(checkpoint?"checkpoint":"passed"):"failed");report.put("failure",why);report.put("blocker",f.blocker);
        report.put("test_only",true);report.put("ordinary_due_queue",true);report.put("independent_audit_required",true);
        if(bot){report.put("money_after",bot->GetMoney());report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));}
        if(checkpoint && passed)state.enforceEffects.store(false); // Freeze this explicit test checkpoint, not a realm configuration change.
        if(!PartyMailFixture().done)PartyMailFixture().Close(checkpoint && passed);
        std::ofstream out("/isolated/evidence/activity-party-repair-"+state.boot+".json");out<<Json(report);out.close();
        if(checkpoint && passed) {
            std::ofstream copy("/isolated/evidence/party-repair-checkpoint.json");copy<<Json(report);copy.close();
        }
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
            if(restart) {
                std::ifstream input("/isolated/evidence/party-repair-checkpoint.json");
                boost::property_tree::read_json(input,f.setup);
                require(f.setup.template get<std::string>("suite")=="activity-party-repair-checkpoint-v1" &&
                    f.setup.template get<std::string>("outcome")=="checkpoint" &&
                    f.setup.template get<uint32_t>("actor")==f.actor && f.setup.template get<uint32_t>("leader")==f.leader,
                    "repair_requires_matching_native_checkpoint");
                f.task=f.setup.template get<std::string>("task");require(IsUuid(f.task),"repair_checkpoint_task_uuid");
                f.setup.put("checkpoint_boot",f.setup.template get<std::string>("boot"));
                f.damaged=true; // Resume never creates damage, equipment, money, a group or a replacement task.
            }
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
            f.setup.put("legacy_maintenance_strategy",bot->GetPlayerbotAI()->HasStrategy("rpg maintenance",BotState::BOT_STATE_NON_COMBAT));
            f.setup.put("native_equipment_durability",uint32_t(bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint8>("durability")->Get()));
            f.setup.put("legacy_can_repair",bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<bool>("can repair")->Get());
            if(!PartyMailFixture().Prepare(state,bot)) {f.blocker="repair_native_party_window_pending";return;}
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            if(!binding){
                f.blocker="repair_native_admission_pending";
                require(sPlayerbotRendezvousManager.IsPartyFreeTime(f.actor),"repair_window_closed_without_admission");
                return;
            }
            require(binding->service==PartyServiceBinding::Service::Repair,"repair_only_binding_required");
            f.task=binding->root;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if(!saved)return;
        f.blocker=state.executionBlockers[f.task];
        require(IsPartyRepairTask(*saved) && saved->actor==f.actor,"original_repair_identity_required");
        auto equipment=[&]() {
            boost::property_tree::ptree rows;
            for(uint8_t slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot) {
                auto* item=bot->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);if(!item)continue;
                boost::property_tree::ptree row;row.put("slot",uint32_t(slot));row.put("guid",item->GetGUIDLow());
                row.put("durability",item->GetUInt32Value(ITEM_FIELD_DURABILITY));rows.push_back({"",row});
            }
            return Json(rows);
        };
        if(checkpoint && saved->phase==Phase::Verifying && state.operations.empty() &&
            bot->GetMoney()<f.setup.template get<uint32_t>("money_before")) {
            require(HasNativeDamagedEquipment(*bot),"repair_checkpoint_is_incomplete");
            require(bool(sPlayerbotRendezvousManager.ReadPartyService(bot,*saved)),"repair_checkpoint_has_native_party_permission");
            f.setup.put("checkpoint_revision",saved->revision);f.setup.put("checkpoint_at_ms",NowMs());
            f.setup.put("checkpoint_money",bot->GetMoney());f.setup.put("checkpoint_equipment",equipment());
            f.setup.put("party_group",bot->GetGroup()->GetId());
            bot->SaveToDB();leader->SaveToDB();emit(true,"");return;
        }
        if(restart && !f.resumed) {
            require(!Terminal(saved->phase) && bot->GetMoney()==f.setup.template get<uint32_t>("checkpoint_money") &&
                equipment()==f.setup.template get<std::string>("checkpoint_equipment"),"repair_restart_preserves_native_possessions");
            auto& party=PartyMailFixture();
            if(!party.recovered) {
                if(!party.Restore(bot,f.setup.template get<uint32_t>("party_group"),f.leader))return;
                require(party.restoredReason=="realm_restart_errands_deferred","repair_restart_defers_old_consent");
            }
            state.enforceEffects.store(true);
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved) &&
                !sPlayerbotRendezvousManager.PartyServiceStatus(f.actor),"repair_restart_drops_transient_binding");
            const auto denied=sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,
                Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Equipment),60000,"isolated_repair_restart_probe");
            require(!denied.Permitted(),"repair_restart_cannot_use_old_permission");
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_repair_restart_permission")) {
                f.blocker="repair_restart_party_return_pending";return;
            }
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->root==f.task,"repair_restart_reuses_exact_root");
            f.setup.put("restart_old_permission_denied",true);f.setup.put("restart_possessions_unchanged",true);
            f.setup.put("restart_same_group",bot->GetGroup()->GetId());
            f.setup.put("restart_reauthorized_at_ms",NowMs());f.setup.put("restart_resume_revision",saved->revision);
            f.resumed=true;
        }
        if(recall && !f.recalled && saved->phase==Phase::Verifying && state.operations.empty() &&
            bot->GetMoney()<f.setup.template get<uint32_t>("money_before")) {
            require(HasNativeDamagedEquipment(*bot),"recall_before_all_repairs_complete");
            f.pausedMoney=bot->GetMoney();f.pausedEquipment=equipment();f.recalledAt=NowMs();
            f.setup.put("recall_revision",saved->revision);f.setup.put("recalled_at_ms",f.recalledAt);
            f.setup.put("money_at_recall",f.pausedMoney);
            require(sPlayerbotRendezvousManager.ResumePartyAssist(bot,leader,"isolated_repair_recall"),"native_repair_recall_accepted");
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"repair_recall_revokes_permission");
            f.recalled=true;return;
        }
        if(recall && f.recalled && !f.resumed) {
            require(bot->GetMoney()==f.pausedMoney && equipment()==f.pausedEquipment,"repair_recall_preserves_wallet_and_equipment");
            require(!Terminal(saved->phase),"repair_recall_preserves_unfinished_root");
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"repair_permission_not_implicitly_restored");
            const auto denied=sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,
                Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Equipment),60000,"isolated_recalled_repair_probe");
            require(!denied.Permitted(),"recalled_repair_cannot_acquire_effects");
            if(NowMs()<f.recalledAt+10000){f.blocker="repair_verified_permission_pause";return;}
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_repair_resume")) {
                f.blocker="repair_return_before_reauthorization";return;
            }
            const auto resumed=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(resumed && resumed->root==f.task && resumed->service==PartyServiceBinding::Service::Repair,
                "repair_fresh_permission_reuses_original_root");
            f.setup.put("reauthorized_at_ms",NowMs());f.setup.put("resume_revision",saved->revision);
            f.setup.put("money_at_reauthorization",bot->GetMoney());f.setup.put("recalled_effects_denied",true);
            f.setup.put("paused_equipment_unchanged",true);f.resumed=true;
        }
        if(saved->phase==Phase::Completed) {
            require(!restart || f.resumed,"repair_restart_fresh_permission_required");
            require(!recall || (f.recalled && f.resumed),"repair_recall_and_reauthorization_required");
            require(!HasNativeDamagedEquipment(*bot),"all_equipped_items_repaired");
            require(bot->GetMoney()<f.setup.template get<uint32_t>("money_before"),"real_money_spent");
            f.setup.put("final_revision",saved->revision);emit(true,"");
        }
    }catch(const std::exception& error){emit(false,error.what());}
}
}
