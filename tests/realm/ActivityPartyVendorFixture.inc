// Explicit isolated party input. No item/gold grants or eligibility overrides.
namespace {
template<class State> void RunIsolatedPartyVendor(State& state) {
    struct Fixture {
        uint32_t actor=0,leader=0,money=0;uint64_t deadline=0,recalledAt=0;
        bool logged=false,prepared=false,resumed=false,recalled=false,reauthorized=false;
        std::string task,blocker,pausedInventory;boost::property_tree::ptree report;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool restart=suite=="activity-party-vendor-resume-v1";
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    Player* bot=nullptr;
    auto inventory=[&]() {
        std::string rows="[";bool first=true;
        auto items=bot->GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        items.sort([](Item* a,Item* b){return a->GetGUIDLow()<b->GetGUIDLow();});
        for(auto* item:items) {
            boost::property_tree::ptree row;row.put("guid",item->GetGUIDLow());row.put("entry",item->GetEntry());
            row.put("quantity",item->GetCount());row.put("position",item->GetPos());
            if(!first)rows+=',';
            first=false;rows+=Json(row);
        }
        return rows+"]";
    };
    auto emit=[&](bool passed,const std::string& why) {
        f.report.put("suite",suite);f.report.put("boot",state.boot);f.report.put("actor",f.actor);
        f.report.put("leader",f.leader);f.report.put("task",f.task);f.report.put("failure",why);f.report.put("blocker",f.blocker);
        f.report.put("outcome",passed?(restart?"passed":"checkpoint"):"failed");
        f.report.put("test_only",true);f.report.put("independent_audit_required",true);
        if(bot){f.report.put("money_after",bot->GetMoney());f.report.put("inventory_after",inventory());
            f.report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));}
        if(passed)state.enforceEffects.store(false);
        if(!PartyMailFixture().done)PartyMailFixture().Close(passed && !restart);
        std::ofstream out("/isolated/evidence/activity-party-vendor-"+state.boot+".json");out<<Json(f.report);out.close();
        if(passed && !restart){std::ofstream copy("/isolated/evidence/party-vendor-checkpoint.json");copy<<Json(f.report);}
        state.fixtureFinished=true;
        sLog.outString("Living isolated vendor fixture: %s",Json(f.report).c_str());
    };
    try {
        if(!f.actor) {
            const auto* actor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");
            const auto* leader=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(actor && leader,"explicit_vendor_pair_required");
            f.actor=uint32_t(std::stoul(actor));f.leader=uint32_t(std::stoul(leader));
            require(f.actor && f.leader && f.actor!=f.leader,"distinct_vendor_pair_required");
            state.gameplayFixtureActor.store(f.actor);state.professionCohortActors.back().store(f.leader);
            f.deadline=NowMs()+600000;
            if(restart) {
                std::ifstream input("/isolated/evidence/party-vendor-checkpoint.json");boost::property_tree::read_json(input,f.report);
                require(f.report.template get<std::string>("outcome")=="checkpoint" &&
                    f.report.template get<std::string>("suite")=="activity-party-vendor-checkpoint-v1" &&
                    f.report.template get<uint32_t>("actor")==f.actor && f.report.template get<uint32_t>("leader")==f.leader,
                    "matching_vendor_checkpoint_required");
                f.task=f.report.template get<std::string>("task");require(IsUuid(f.task),"vendor_checkpoint_uuid_required");
                f.report.put("checkpoint_boot",f.report.template get<std::string>("boot"));f.prepared=true;
            }
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready)return;
        bot=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(NowMs()>f.deadline){emit(false,"vendor_deadline_preserved_obligation");return;}
        if(!f.logged){f.logged=true;for(auto id:{f.actor,f.leader})
            if(!sRandomPlayerbotMgr.GetPlayerBot(id))sRandomPlayerbotMgr.AddRandomBot(id);}
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(f.leader);
        if(!bot || !leader || !bot->IsInWorld() || !leader->IsInWorld() || NativeSafety(bot) || NativeSafety(leader))return;
        if(!f.prepared) {
            require(!bot->GetGroup() && !leader->GetGroup(),"existing_vendor_groups_preserved");
            for(const auto& row:state.cache)if(row.second.actor==f.actor && row.second.mode==Mode::Active && !Terminal(row.second.phase))
                require(false,"existing_vendor_actor_obligation_preserved");
            PartyVendorJob planned;
            require(PlanNativePartyVendorBatch(*bot,planned,f.blocker),"native_disposable_batch_required:"+f.blocker);
            require(planned.items.size()>=3,"three_existing_disposable_stacks_required");
            f.report.put("accepted_manifest",EncodePartyVendorJob(planned));f.report.put("inventory_before",inventory());
            f.report.put("money_before",bot->GetMoney());f.report.put("money_grants",0);f.report.put("item_grants",0);
            bot->SaveToDB();f.prepared=true;state.enforceEffects.store(true);
            std::ofstream setup("/isolated/evidence/party-vendor-setup-"+state.boot+".json");setup<<Json(f.report);return;
        }
        if(f.task.empty()) {
            if(!PartyMailFixture().Prepare(state,bot)){f.blocker="vendor_native_party_window_pending";return;}
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            if(!binding){f.blocker="vendor_native_admission_pending";return;}
            require(binding->service==PartyServiceBinding::Service::Vendor,"vendor_only_binding_required");f.task=binding->root;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);if(!saved)return;
        PartyVendorJob job;
        require(IsPartyVendorTask(*saved) && saved->actor==f.actor && DecodePartyVendorJob(saved->checkpoint.data,job),"original_vendor_root_required");
        const auto next=job.next;job.next=0;
        require(EncodePartyVendorJob(job)==f.report.template get<std::string>("accepted_manifest"),"accepted_vendor_items_immutable");
        f.blocker=state.executionBlockers[f.task];
        if(!restart && next==1 && saved->phase==Phase::Verifying && state.operations.empty()) {
            f.report.put("checkpoint_revision",saved->revision);f.report.put("checkpoint_at_ms",NowMs());
            f.report.put("checkpoint_money",bot->GetMoney());f.report.put("checkpoint_inventory",inventory());
            f.report.put("party_group",bot->GetGroup()->GetId());bot->SaveToDB();leader->SaveToDB();emit(true,"");return;
        }
        if(restart && !f.resumed) {
            require(next==1 && bot->GetMoney()==f.report.template get<uint32_t>("checkpoint_money") &&
                inventory()==f.report.template get<std::string>("checkpoint_inventory"),"vendor_restart_possessions_unchanged");
            auto& party=PartyMailFixture();
            if(!party.recovered){if(!party.Restore(bot,f.report.template get<uint32_t>("party_group"),f.leader))return;
                require(party.restoredReason=="realm_restart_errands_deferred","vendor_old_consent_deferred");}
            state.enforceEffects.store(true);
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved),"vendor_old_permission_not_restored");
            require(!sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,Mask(Effect::Inventory)|Mask(Effect::Money),
                60000,"isolated_vendor_old_permission").Permitted(),"vendor_old_effects_denied");
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_vendor_restart_permission"))return;
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->root==f.task,"vendor_restart_same_root");
            f.report.put("restart_old_permission_denied",true);f.report.put("restart_possessions_unchanged",true);
            f.report.put("restart_reauthorized_at_ms",NowMs());f.report.put("restart_resume_revision",saved->revision);f.resumed=true;
        }
        if(restart && f.resumed && !f.recalled && next==2 && saved->phase==Phase::Verifying && state.operations.empty()) {
            f.money=bot->GetMoney();f.pausedInventory=inventory();f.recalledAt=NowMs();
            f.report.put("recall_revision",saved->revision);f.report.put("recalled_at_ms",f.recalledAt);f.report.put("money_at_recall",f.money);
            require(sPlayerbotRendezvousManager.ResumePartyAssist(bot,leader,"isolated_vendor_recall"),"native_vendor_recall_required");
            f.recalled=true;return;
        }
        if(f.recalled && !f.reauthorized) {
            require(next==2 && !Terminal(saved->phase) && bot->GetMoney()==f.money && inventory()==f.pausedInventory,
                "vendor_recall_preserves_unsold_goods_and_money");
            require(!sPlayerbotRendezvousManager.ReadPartyService(bot,*saved) &&
                !sLivingActivityCoordinator.AcquireSavedTask(f.task,saved->revision,Mask(Effect::Inventory)|Mask(Effect::Money),
                    60000,"isolated_vendor_recall_probe").Permitted(),"vendor_recall_effects_denied");
            if(NowMs()<f.recalledAt+10000)return;
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_vendor_reauthorization"))return;
            const auto binding=sPlayerbotRendezvousManager.PartyServiceStatus(f.actor);
            require(binding && binding->root==f.task,"vendor_recall_reuses_original_root");
            f.report.put("reauthorized_at_ms",NowMs());f.report.put("resume_revision",saved->revision);
            f.report.put("money_at_reauthorization",bot->GetMoney());f.report.put("recalled_effects_denied",true);
            f.report.put("paused_inventory_unchanged",true);f.reauthorized=true;
        }
        if(saved->phase==Phase::Completed) {
            require(restart && f.recalled && f.reauthorized && next==job.items.size(),"vendor_all_interruptions_required");
            require(bot->GetMoney()>f.report.template get<uint32_t>("money_before"),"vendor_native_payment_required");
            f.report.put("final_revision",saved->revision);emit(true,"");
        }
    }catch(const std::exception& error){emit(false,error.what());}
}
}
