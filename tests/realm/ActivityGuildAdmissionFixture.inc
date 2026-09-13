// Copied realm only. Admit an EXISTING native delivery through the ordinary
// guild producer. This probe logs in a named bot and enables the not-yet-cut-over
// Active policy for this copied test; it never supplies goods or calls admission.
namespace {
template<class FixtureState>
void RunIsolatedGuildAdmissionFixture(FixtureState& state) {
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;auto& f=state.guildDepositFixture;
    auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool passed,const std::string& why) {
        auto report=f.setup;report.put("suite","activity-guild-admission-v1");report.put("boot",state.boot);
        report.put("outcome",passed?"passed":"failed");report.put("failure",why);report.put("blocker",f.blocker);
        report.put("task",f.task);report.put("actor",f.actor);report.put("test_only",true);
        report.put("item_grants",0);report.put("money_grants",0);report.put("permission_grants",0);
        report.put("fixture_relocations",0);report.put("manual_admission_calls",0);report.put("ordinary_ai_unpaused",true);
        report.put("isolated_active_policy_override",true);report.put("independent_audit_required",true);
        std::ofstream out("/isolated/evidence/activity-guild-admission-"+state.boot+".json");out<<Json(report);out.close();
        state.fixtureFinished=true;if(!out)sLog.outError("Living guild admission evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    try {
        if(!f.actor) {
            const auto* raw=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");const std::string actor=raw?raw:"";
            const auto* ref=std::getenv("LIVING_WOW_NATIVE_FIXTURE_DELIVERY");const std::string delivery=ref?ref:"";
            require(!actor.empty() && actor.size()<=9 && actor.find_first_not_of("0123456789")==std::string::npos,"explicit_guild_actor_required");
            require(!delivery.empty() && delivery.size()<=20 && delivery.find_first_not_of("0123456789")==std::string::npos,"explicit_existing_delivery_required");
            f.actor=uint32_t(std::stoul(actor));f.job.delivery=std::stoull(delivery);f.deadline=NowMs()+600000;
            require(f.actor && f.job.delivery && sPlayerbotAIConfig.IsInRandomAccountList(
                sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER,f.actor))),"existing_random_bot_account_required");
            // Deliberately do NOT install gameplayFixtureActor/cohort holds.
            // Normal AI, native safety and the shared authority all keep running.
        }
        if(sWorld.GetActiveAndQueuedSessionCount())return;
        if(NowMs()>f.deadline){emit(false,"automatic_guild_delivery_deadline_with_preserved_work");return;}
        if(!state.loaded || !state.incoming.empty() || !state.resources.Protection().ready)return;
        auto* actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        if(!actor) {
            if(!f.requested && sRandomPlayerbotMgr.GetAllBots().size()<49){f.requested=true;sRandomPlayerbotMgr.AddRandomBot(f.actor);}
            f.blocker="automatic_delivery_actor_login_pending";return;
        }
        if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor)){f.blocker="automatic_delivery_native_safety_wait";return;}
        require(actor->GetSession() && actor->GetPlayerbotAI() && !actor->isRealPlayer(),"existing_native_bot_required");
        if(!f.started) {
            GuildDeliveryJob native;
            if(!sGuildSupplies.ReadDeliveryJob(f.job.delivery,f.actor,native,f.blocker))return;
            require(!native.money,"existing_item_delivery_required");f.job=native;
            f.task=SourceId("guild_delivery",GuildDeliverySourceKey(native,f.actor));
            require(!sLivingActivityCoordinator.ReadSavedTask(f.task),"existing_unmanaged_delivery_required");
            auto* guild=sGuildMgr.GetGuildById(native.guild);require(guild && guild->GetPurchasedTabs(),"existing_native_guild_bank_required");
            f.setup.put("job",EncodeGuildDeliveryJob(native));f.setup.put("money_before",actor->GetMoney());
            f.setup.put("bags_before",actor->GetItemCount(native.entry,false));
            const auto counts=guild->GetBankItemCounts();const auto found=counts.find(native.entry);
            f.setup.put("bank_before",found==counts.end()?0:found->second);f.setup.put("started_at_ms",NowMs());
            f.setup.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            std::ofstream input("/isolated/evidence/guild-admission-input-"+state.boot+".json");input<<Json(f.setup);input.close();
            require(bool(input),"guild_admission_input_evidence_required");f.started=true;
        }
        // The ordinary producer observes these same policy/authority fields.
        // Production's unaccepted cutover guard is NOT changed by this fixture.
        state.effective=Mode::Active;state.enforceEffects.store(true,std::memory_order_release);
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if(!saved){f.blocker="ordinary_guild_producer_admission_pending";return;}
        require(saved->checkpoint.data==EncodeGuildDeliveryJob(f.job),"automatic_delivery_identity_changed");
        const auto progress=state.executionBlockers.find(f.task);
        f.blocker=progress==state.executionBlockers.end()?saved->checkpoint.blocker:progress->second;
        const auto key=std::to_string(saved->revision)+":"+f.blocker;
        if(key!=f.lastDiagnostic && f.diagnostics<128) {
            f.lastDiagnostic=key;++f.diagnostics;
            sLog.outString("Living guild admission progress: actor=%u task=%s revision=%llu phase=%s step=%s blocker=%s",
                f.actor,f.task.c_str(),(unsigned long long)saved->revision,Name(saved->phase),saved->checkpoint.step.c_str(),f.blocker.c_str());
        }
        if(!Terminal(saved->phase))return;
        require(saved->phase==Phase::Completed && saved->checkpoint.step=="guild_delivery_completed","automatic_delivery_not_bank_completed");
        f.setup.put("completed_revision",saved->revision);f.setup.put("completed_at_ms",saved->updatedAtMs);
        f.setup.put("money_after",actor->GetMoney());f.setup.put("bags_after",actor->GetItemCount(f.job.entry,false));
        auto* guild=sGuildMgr.GetGuildById(f.job.guild);require(guild,"native_guild_disappeared");
        const auto counts=guild->GetBankItemCounts();const auto found=counts.find(f.job.entry);
        f.setup.put("bank_after",found==counts.end()?0:found->second);
        f.setup.add_child("inventory_after",GuildMailFixtureInventory(*actor));emit(true,"");
    }catch(const std::exception& error){emit(false,error.what());}
}
}
