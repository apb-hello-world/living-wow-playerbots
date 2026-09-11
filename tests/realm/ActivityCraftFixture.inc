// Copied realm only. Real known Alchemy recipe, ordinary timed native cast,
// exact native input/output and SQL receipt. The explicitly supplied variant
// may supply only missing reagents and records every grant. The settlement
// variant proves a supplied skill-job. The paid variant reuses the real vendor
// adapter before this same craft/settlement path; only herbs may be supplied.
// Neither fixture proves autonomous travel or auction/mail procurement.
namespace {
    class CraftFixtureReservation final : public NativeReservationAdapter {
    public:
        bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) override {
            ProfessionJob job;
            if (request.transition.task.sourceKey.find("isolated_craft_v1:")!=0 ||
                !DecodeProfessionJob(request.transition.task.checkpoint.data,job,blocker)) return false;
            for (const auto& change : request.changes) {
                const auto& claim=change.after;
                bool valid=claim.actor==actor.GetGUIDLow() && claim.task==request.transition.task.root &&
                    claim.location=="bags" && claim.itemGuid && !claim.copper;
                bool input=false;
                for (const auto& reagent : job.reagents)
                    input|=claim.itemEntry==reagent.entry && (claim.quantity==reagent.perAttempt ||
                        (job.purpose==ProfessionPurpose::SkillGain && reagent.entry==3371 &&
                         claim.quantity==5 && actor.GetItemCount(reagent.entry,false)>=5));
                valid=valid && ((claim.state=="held" && input) ||
                    (claim.state=="released" && (input || claim.itemEntry==job.outputEntry)));
                if (!valid) {blocker="isolated_craft_claim_mismatch";return false;}
            }
            blocker.clear();return !request.changes.empty();
        }
    };
}
void LivingActivityCoordinator::RunIsolatedCraftFixture() {
    MANGOS_ASSERT(OnWorldThread());RequireIsolatedNativeFixtureEnvironment();
    if (state->fixtureFinished || NowMs()<state->fixtureNext) return;
    state->fixtureNext=NowMs()+1000;
    if (sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty() || !state->resources.Protection().ready) return;
    auto& fixture=state->craftFixture;auto& request=fixture.request;auto& operation=fixture.operation;
    const std::string suite=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool interrupted=suite=="activity-craft-interrupted-v1";
    const bool paid=suite=="activity-profession-paid-v1";
    const bool banked=suite=="activity-profession-bank-v1";
    const bool settling=suite=="activity-profession-settlement-v1" || paid || banked;
    const bool supplied=suite=="activity-craft-supplied-v1" || interrupted || settling;
    if (!fixture.deadline) fixture.deadline=NowMs()+120000;
    auto emit=[&](bool passed,const std::string& failure) {
        if (passed) fixture.blocker.clear();
        boost::property_tree::ptree report;
        report.put("suite",suite);report.put("boot",state->boot);
        report.put("expected_operation_outcome",interrupted ? "rejected_without_effect" : "verified");
        report.put("test_only",true);report.put("outcome",passed ? "passed" : "failed");report.put("failure",failure);
        report.put("step",fixture.step);report.put("blocker",fixture.blocker);report.put("actor",request.task.actor);
        report.put("task",request.task.id);report.put("operation",operation.transition.receipt);report.put("native_launches",fixture.launches);
        report.put("recipe",2329);report.put("native_money_before",fixture.before.money);report.put("skill_before",fixture.before.skill);
        report.put("native_after_sampled",fixture.after.actor!=0);
        if (fixture.after.actor) {
            report.put("native_money_after",fixture.after.money);report.put("skill_after",fixture.after.skill);
        } // An uncollected sample is unknown, not zero gold/skill.
        report.put("paid_workflow_proof",passed && paid);report.put("profession_job_completion_proof",passed && settling);report.put("travel_proof",false);
        report.put("ordinary_profession_runner",paid);
        if (banked) {
            report.put("ordinary_bank_runner",true);report.put("bank_operation",fixture.bankOperation);
            report.put("bank_claim",fixture.bankClaim);report.put("bank_guid",fixture.bankGuid);
            report.put("bank_quantity",fixture.bankCount);report.put("bank_total_before",fixture.bankTotal);
            report.put("bank_service_positioning_fixture",true);
        }
        if (paid) {
            report.put("vendor_operation",state->operationFixtureRequest.transition.receipt);
            report.put("vendor_dispatches",state->operationFixtureCalls);
            report.put("native_price",state->vendorFixtureQuote.copper);
            report.put("money_before_purchase",state->vendorFixtureQuote.moneyBefore);
            report.put("purchased_entry",state->vendorFixtureQuote.entry);
            report.put("purchased_quantity",state->vendorFixtureQuote.quantity);
            report.add_child("vendor_checks",state->admissionFixtureChecks);
            auto* bot=sRandomPlayerbotMgr.GetPlayerBot(request.task.actor);
            if (bot && state->vendorFixturePositionChanged && bot->IsInWorld() && !NativeSafety(bot) && !DefersNativeSave(bot->GetGUIDLow())) {
                bot->NearTeleportTo(state->vendorFixtureX,state->vendorFixtureY,state->vendorFixtureZ,state->vendorFixtureO);
                report.put("fixture_position_restored",true);
            } else report.put("fixture_position_restored",false);
        }
        report.put("settlement_receipt",settling ? request.receipt : "");
        report.put("gold_grants",0);report.put("spell_grants",0);report.put("skill_grants",0);
        report.put("general_execution_enabled",false);report.add_child("fixture_material_grants",fixture.grants);
        report.add_child("selection_rejections",fixture.selection);report.add_child("checks",fixture.checks);
        report.add_child("native_snapshot",fixture.snapshot);
        if (request.task.actor) report.put("actor_diagnostic",ActorJson(request.task.actor));
        std::ofstream evidence("/isolated/evidence/activity-craft-"+state->boot+".json");evidence<<Json(report);evidence.close();
        if (!evidence) sLog.outError("Living isolated craft evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        state->gameplayFixtureActor.store(0,std::memory_order_release);state->fixtureFinished=true;
    };
    auto require=[&](bool value,const char* name) {
        boost::property_tree::ptree check;check.put("case",name);check.put("passed",value);fixture.checks.push_back({"",check});
        if (!value) throw std::runtime_error(name);
    };
    if (NowMs()>fixture.deadline) {emit(false,"fixture_deadline");return;}
    try {
        if (!request.task.actor) {
            fixture.selection.clear();Player* selected=nullptr;ProfessionJob job;unsigned bestMissing=10000;
            auto skipped=[&](const char* reason){fixture.selection.put(reason,fixture.selection.get<unsigned>(reason,0)+1);};
            for (const auto& binding : state->bindings) {
                auto* candidate=sRandomPlayerbotMgr.GetPlayerBot(binding.first);
                if (!candidate || !candidate->GetPlayerbotAI() || !candidate->IsInWorld() || NativeSafety(candidate) ||
                    candidate->GetMap()->IsDungeon() || candidate->IsNonMeleeSpellCasted(false,true,true)) {skipped("native_safety");continue;}
                if (candidate->GetGroup() || candidate->GetTradeData() || DefersNativeSave(binding.first) ||
                    state->authority.Read(binding.first).lease.actor) {skipped("existing_commitment");continue;}
                if (!candidate->HasSpell(2329)) {skipped("recipe_not_known");continue;}
                if (banked && (candidate->GetMapId()!=1 || candidate->GetItemCount(765,false)>1 ||
                    candidate->GetItemCount(765,true)!=candidate->GetItemCount(765,false))) {
                    skipped("bank_fixture_map_or_existing_stock");continue;
                }
                ProfessionJob next;next.recipe=2329;next.skill=SKILL_ALCHEMY;
                next.purpose=settling ? ProfessionPurpose::SkillGain : ProfessionPurpose::Intermediate;
                next.outputQuantity=1;next.attemptLimit=1;
                const auto native=InspectNativeProfessionRecipe(*candidate,next);
                if (!native.blocker.empty() || native.operation!=ProfessionOperation::CreateItem) {skipped("native_recipe_prerequisite");continue;}
                next.outputEntry=native.outputEntry;next.reagents=native.reagents;next.initialSkill=native.skillValue;
                CraftFrame frame;std::string blocker;
                if (settling) {
                    next.targetSkill=native.skillValue+1;
                    if (!MatchNativeProfessionRecipe(next,native,blocker)) {skipped("skill_goal_not_useful");continue;}
                }
                if (!ReadNativeCraftFrame(*candidate,next,frame,blocker)) {skipped("inventory_snapshot");continue;}
                unsigned missing=0;bool eligible=true;
                for (const auto& reagent : next.reagents) {
                    unsigned stacks=0,owned=0;
                    for (const auto& stack : frame.stacks) if (stack.entry==reagent.entry) {
                        ++stacks;owned=stack.count;
                        if (state->resources.Protection().ProtectedItem(binding.first,stack.guid,stack.entry) ||
                            sPlayerbotActionBroker.IsItemReserved(stack.guid)) eligible=false;
                    }
                    if (stacks>1 || sGuildSupplies.ReservedEntry(binding.first,reagent.entry)) eligible=false;
                    missing+=owned>=reagent.perAttempt ? 0 : reagent.perAttempt-owned;
                }
                if (!eligible) {skipped("protected_or_mixed_input");continue;}
                if (!supplied && missing) {skipped("missing_owned_material");continue;}
                if (missing<bestMissing) {selected=candidate;job=next;bestMissing=missing;}
            }
            if (!selected) {
                if (banked && !state->supportFixtureLookupStarted) {
                    state->supportFixtureLookupStarted=true;
                    CharacterDatabase.AsyncQuery([this](QueryResult* rows) {
                        MANGOS_ASSERT(OnWorldThread());RequireIsolatedNativeFixtureEnvironment();
                        if (!rows || state->fixtureFinished || sWorld.GetActiveAndQueuedSessionCount() ||
                            sRandomPlayerbotMgr.GetAllBots().size()>=49) return;
                        do {const auto* f=rows->Fetch();if (!sPlayerbotAIConfig.IsInRandomAccountList(f[1].GetUInt32())) continue;
                            state->supportFixtureRequestedActor=f[0].GetUInt32();sRandomPlayerbotMgr.AddRandomBot(f[0].GetUInt32());break;
                        } while(rows->NextRow());
                    },"SELECT c.guid,c.account FROM characters c JOIN character_spell s ON s.guid=c.guid AND s.spell=2329 AND s.disabled=0 "
                        "JOIN character_skills k ON k.guid=c.guid AND k.skill=171 AND k.value BETWEEN 1 AND 9 "
                        "WHERE c.online=0 AND c.map=1 AND NOT EXISTS(SELECT 1 FROM group_member g WHERE g.memberGuid=c.guid) "
                        "AND NOT EXISTS(SELECT 1 FROM living_activity_task t WHERE t.actor_guid=c.guid AND t.mode='active' "
                        "AND t.phase NOT IN ('completed','cancelled','failed')) ORDER BY c.guid LIMIT 4");
                }
                fixture.blocker="no_safe_known_recipe_actor_with_eligible_materials";return;
            }
            // Test-only hold; native Unit/Spell updates continue. No grant or
            // role/profession rewrite can occur in a production image.
            state->gameplayFixtureActor.store(selected->GetGUIDLow(),std::memory_order_release);
            selected->GetMotionMaster()->Clear();selected->StopMoving();
            request.task.actor=selected->GetGUIDLow();
            for (const auto& reagent : job.reagents) {
                const auto owned=selected->GetItemCount(reagent.entry,false);
                // Explicit surplus fixture: five physical vials, one consumed,
                // four preserved by the real completion path. All grants logged.
                const auto required=settling && reagent.entry==3371 ? 5u : reagent.perAttempt;
                if (owned>=required) continue;
                const auto missing=required-owned;
                require(supplied && missing<=10,"explicit_bounded_fixture_material_supply");
                ItemPosCountVec destinations;
                require(selected->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,reagent.entry,missing)==EQUIP_ERR_OK,
                    "fixture_material_capacity");
                auto* added=selected->StoreNewItem(destinations,reagent.entry,true);
                boost::property_tree::ptree grant;grant.put("entry",reagent.entry);grant.put("quantity",missing);
                grant.put("count_before",owned);grant.put("count_after",selected->GetItemCount(reagent.entry,false));
                grant.put("native_item",added ? added->GetGUIDLow() : 0);fixture.grants.push_back({"",grant});
                require(added && selected->GetItemCount(reagent.entry,false)==owned+missing,"fixture_material_supply_native");
            }
            request.task.source="profession_job";request.task.sourceKey="isolated_craft_v1:"+state->boot;
            request.task.id=request.task.root=SourceId(request.task.source,request.task.sourceKey);
            request.task.kind=Kind::Profession;request.task.mode=Mode::Active;request.task.accepted=true;
            request.task.priority=Priority::Delivery;request.task.phase=Phase::Queued;
            request.task.context=ReadNativeContext(*selected,state->policyRevision,state->boot);
            request.task.createdAtMs=request.task.updatedAtMs=NowMs();request.receipt=NewId();
            request.task.checkpoint.step="profession_prepare";request.task.checkpoint.data=EncodeProfessionJob(job);
            require(ReadNativeCraftFrame(*selected,job,fixture.before,fixture.blocker),"fixture_before_native_frame");
            selected->SaveToDB();
            if (banked) {
                // Explicit copied fixture positioning, not autonomous travel.
                const auto spawnId=selected->GetTeam()==ALLIANCE ? 46223u : 6598u;
                const auto entry=selected->GetTeam()==ALLIANCE ? 4155u : 3309u;
                const auto* spawn=sObjectMgr.GetCreatureData(spawnId);
                require(spawn && spawn->id==entry && spawn->mapid==selected->GetMapId(),"bank_fixture_valid_native_spawn");
                selected->NearTeleportTo(spawn->posX,spawn->posY,spawn->posZ,spawn->orientation);
            }
            return;
        }
        auto* bot=sRandomPlayerbotMgr.GetPlayerBot(request.task.actor);
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported()) {fixture.blocker="native_fixture_actor_transfer";return;}
        const bool original=state->enforceEffects.exchange(true,std::memory_order_acq_rel);
        struct Restore {State& state;bool original;~Restore(){state.enforceEffects.store(original,std::memory_order_release);}} restore{*state,original};
        NativeCraftOperation craft;CraftFixtureReservation reserve;const auto effects=craft.OperationEffects();
        if (!fixture.step) {
            if (banked) {
                if (!NativeNearbyBanker(*bot)) {fixture.blocker="bank_fixture_native_arrival_pending";return;}
                request.task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
            }
            require(SubmitTask(request).code==AdmissionCode::Pending,"craft_task_admitted");fixture.step=1;return;
        }
        const auto saved=state->cache.find(request.task.id);require(saved!=state->cache.end(),"craft_task_receipt_present");
        if (fixture.step==10) request.task=saved->second; // Journal-driven bank substep; exact task identity stays fixed.
        if (fixture.step==5) {
            if (saved->second.phase==Phase::Executing) {fixture.blocker="native_cast_or_receipt_pending";return;}
            require(saved->second.phase==Phase::Verifying && !DefersNativeSave(bot->GetGUIDLow()) &&
                !state->operations.count(operation.transition.receipt),"craft_native_result_and_save_acknowledged");
            request.task=saved->second;
        }
        require(saved->second.revision==request.task.revision,"craft_exact_saved_revision");
        auto followRunnerWrite=[&]() {
            for (const auto& write : state->pending) if (write.task.id==request.task.id) {
                request.expectedRevision=request.task.revision;request.task=write.task;request.receipt=write.admissionReceipt;
                return true;
            }
            return false;
        };
        auto advance=[&](Phase phase) {
            request.expectedRevision=request.task.revision++;request.receipt=NewId();
            request.task.phase=phase;request.task.updatedAtMs=NowMs();
            require(SubmitTask(request).code==AdmissionCode::Pending,"craft_fixture_phase_queued");
        };
        if (fixture.step==1) {
            if (paid) {
                fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
                if (!followRunnerWrite()) return;
                require(request.task.phase==Phase::Preparing,"paid_runner_resumes_preparation_after_purchase");
            } else advance(Phase::Preparing);
            fixture.step=banked ? 10 : 2;return;
        }
        if (fixture.step==10) {
            if (!fixture.bankGuid) {
                auto items=bot->GetPlayerbotAI()->InventoryParseItems("765",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
                require(items.size()==1,"bank_fixture_one_owned_herb_stack");auto* item=*items.begin();
                require(item && item->GetEntry()==765 && !state->resources.Protection().ProtectedItem(request.task.actor,item->GetGUIDLow(),765),
                    "bank_fixture_stock_not_reserved");
                ItemPosCountVec destinations;uint8_t affected=0;
                require(bot->CanBankItem(NULL_BAG,NULL_SLOT,destinations,item,false,affected)==EQUIP_ERR_OK && destinations.size()==1,
                    "bank_fixture_native_deposit_capacity");
                fixture.bankGuid=item->GetGUIDLow();fixture.bankCount=item->GetCount();fixture.bankTotal=bot->GetItemCount(765,true);
                bot->RemoveItem(item->GetBagSlot(),item->GetSlot(),true);bot->BankItem(destinations,item,true);bot->SaveToDB();
                require(bot->GetItemCount(765,false)==0 && bot->GetItemCount(765,true)==fixture.bankTotal,
                    "bank_fixture_only_relocated_owned_herbs");return;
            }
            if (!fixture.bankClaim.empty()) {
                const auto* claim=state->resources.Inspect(fixture.bankClaim);
                if (claim && claim->location=="bags" && saved->second.phase==Phase::Verifying && !DefersNativeSave(request.task.actor)) {
                    auto* item=bot->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,fixture.bankGuid));
                    require(item && !Player::IsBankPos(item->GetBagSlot(),item->GetSlot()) && item->GetCount()==fixture.bankCount &&
                        bot->GetItemCount(765,true)==fixture.bankTotal && claim->itemGuid==fixture.bankGuid &&
                        claim->quantity==fixture.bankCount && claim->state=="held","ordinary_bank_runner_preserved_item_and_claim");
                    fixture.inheritedInputs.push_back(claim->id);
                    ProfessionJob job;std::string blocker;
                    require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker) &&
                        ReadNativeCraftFrame(*bot,job,fixture.before,blocker),"craft_frame_refreshed_after_verified_bank_move");
                    const auto count=state->nativeDispatches;
                    NativeBankQuote quote;NativeBankWithdrawal adapter(quote);
                    require(!DispatchSavedOperation(fixture.bankOperation,{},adapter).executed && state->nativeDispatches==count,
                        "bank_duplicate_dispatch_does_not_move_again");
                    advance(Phase::Preparing);fixture.step=2;return;
                }
            }
            const auto nativeBank=bot->GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BANK);
            require(std::any_of(nativeBank.begin(),nativeBank.end(),[&](const Item* item) {
                return item && item->GetGUIDLow()==fixture.bankGuid && item->GetEntry()==765 &&
                    item->GetCount()==fixture.bankCount && Player::IsBankPos(item->GetBagSlot(),item->GetSlot());
            }),"native_bank_mask_reads_actual_banked_stack");
            fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
            for (const auto& pending : state->operations) if (pending.second.request.transition.task.id==request.task.id &&
                pending.second.request.kind=="bank_withdraw") {
                fixture.bankOperation=pending.first;fixture.bankClaim=pending.second.request.bankTransfer.id;
            }
            followRunnerWrite();return;
        }
        if (fixture.step==2) {
            ProfessionSnapshot prepared;
            if (!ReadProfessionSnapshot(request.task.actor,request.task.id,request.task.revision,prepared,fixture.blocker)) return;
            require(NextProfessionStep(request.task,prepared).step==ProfessionStep::Execute &&
                prepared.attempts.empty() && prepared.outputPerAttempt==1,
                "craft_initial_native_snapshot_selects_exact_ready_attempt");
            if (paid) {
                fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
                if (!followRunnerWrite()) return;
                fixture.inputs=fixture.inheritedInputs;
                for (const auto& write : state->pending) if (write.task.id==request.task.id)
                    for (const auto& change : write.claims) {
                        require(change.after.itemEntry!=3371 && change.after.quantity==1,
                            "paid_runner_reserves_only_missing_herbs_not_paid_bundle");
                        fixture.inputs.push_back(change.after.id);
                    }
                require(fixture.inputs.size()==3,"paid_runner_preserves_paid_claim_and_reserves_both_herbs");
                const auto count=state->pending.size();
                AdvanceProfessionJob(request.task.actor,request.task.id);
                require(state->pending.size()==count,"paid_runner_duplicate_reservation_does_not_repeat_write");
                fixture.step=3;return;
            }
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_craft_fixture");
            require(grant.Permitted(),"craft_reservation_authority");
            auto& reservation=fixture.reservation;reservation.transition=request;reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision;reservation.transition.receipt=NewId();reservation.transition.task.updatedAtMs=NowMs();
            reservation.authorization=grant.action;ProfessionJob job;std::string blocker;
            require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker),"craft_saved_job_decoded");
            for (const auto& reagent : job.reagents) for (const auto& item : fixture.before.stacks) if (item.entry==reagent.entry) {
                const ResourceClaim* inherited=nullptr;
                for (const auto& id : fixture.inheritedInputs) {
                    const auto* existing=state->resources.Inspect(id);
                    require(existing && existing->state=="held" && existing->task==request.task.root &&
                        existing->actor==request.task.actor,"paid_input_still_owned_by_same_root");
                    if (existing->itemGuid==item.guid && existing->itemEntry==reagent.entry) {
                        require(!inherited && existing->quantity>=reagent.perAttempt && existing->quantity<=item.count,
                            "paid_input_exact_native_claim_not_replaced");
                        inherited=existing;
                    }
                }
                if (inherited) {fixture.inputs.push_back(inherited->id);continue;}
                ResourceClaim claim;claim.id=NewId();claim.task=request.task.root;claim.actor=request.task.actor;
                claim.itemGuid=item.guid;claim.itemEntry=item.entry;
                claim.quantity=settling && reagent.entry==3371 ? 5u : reagent.perAttempt;
                claim.location="bags";claim.state="held";
                reservation.changes.push_back({claim,0});
                fixture.inputs.push_back(claim.id);
            }
            require(SubmitResourceReservation(reservation,reserve).code==AdmissionCode::Pending,"craft_exact_inputs_reserved");
            request=reservation.transition;fixture.step=3;return;
        }
        if (fixture.step==3) {
            if (paid) {
                // Real coordinator owns the request shape and reads the saved
                // recipe, current native frame and inherited purchase claim.
                fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
                std::string receipt;
                for (const auto& row : state->operations) if (row.second.request.transition.task.id==request.task.id) receipt=row.first;
                if (receipt.empty()) return;
                require(!state->operations.at(receipt).dispatched,"paid_coordinator_prepares_exact_native_attempt");
                require(PrepareProfessionAttempt(request.task.actor,request.task.id,request.task.revision,receipt).code==
                    AdmissionCode::Pending,"paid_duplicate_preparation_shares_one_operation");
                require(!DispatchProfessionAttempt(request.task.actor,receipt).executed,"paid_unsaved_intent_cannot_cast");
                operation=state->operations.at(receipt).request;
                require(operation.consumption.size()==fixture.inputs.size(),"paid_coordinator_selects_saved_input_claims");
                for (const auto& use : operation.consumption)
                    require(std::find(fixture.inputs.begin(),fixture.inputs.end(),use.before.id)!=fixture.inputs.end(),
                        "paid_coordinator_preserves_existing_claim_identity");
                request=operation.transition;fixture.step=4;return;
            }
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_craft_fixture");
            require(grant.Permitted(),"craft_intent_authority");
            operation.transition=request;operation.transition.expectedRevision=request.task.revision;
            ++operation.transition.task.revision;operation.transition.receipt=NewId();operation.transition.task.updatedAtMs=NowMs();
            operation.transition.task.phase=Phase::Executing;operation.authorization=grant.action;
            operation.kind=craft.OperationKind();operation.effects=effects;operation.persistence=craft.PersistencePolicy();
            operation.beforeState="{\"recipe\":2329,\"skill\":"+std::to_string(fixture.before.skill)+",\"money\":"+std::to_string(fixture.before.money)+'}';
            ProfessionJob job;std::string blocker;require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker),"craft_intent_job_decoded");
            operation.itemGain={job.outputEntry,1};
            for (const auto& id : fixture.inputs) {
                const auto* claim=state->resources.Inspect(id);require(claim && claim->state=="held","craft_input_claim_saved");
                require(claim->quantity<=10,"craft_bounded_fixture_input_claim");
                const auto need=std::find_if(job.reagents.begin(),job.reagents.end(),[&](const auto& r){return r.entry==claim->itemEntry;});
                require(need!=job.reagents.end() && need->perAttempt<=claim->quantity,"craft_claim_matches_exact_recipe_consumption");
                operation.consumption.push_back({*claim,need->perAttempt});
            }
            const auto admitted=SubmitOperationIntent(operation,craft);fixture.blocker=admitted.blocker;
            require(admitted.code==AdmissionCode::Pending,"craft_intent_saved_before_spell_start");
            require(!DispatchSavedOperation(operation.transition.receipt,grant,craft).executed,"craft_unsaved_intent_cannot_cast");
            request=operation.transition;fixture.step=4;return;
        }
        if (fixture.step==4) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_craft_fixture");
            require(grant.Permitted(),"craft_saved_execution_grant");
            DispatchResult result;
            if (paid) {
                const bool before=state->operations.at(operation.transition.receipt).dispatched;
                fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
                const auto& pending=state->operations.at(operation.transition.receipt);
                result.executed=!before && pending.dispatched && pending.craftAwaiting;
            } else {result=DispatchSavedOperation(operation.transition.receipt,grant,craft);fixture.blocker=result.admission.blocker;}
            if (result.executed) ++fixture.launches;
            require(result.executed && !result.outcomeQueued && fixture.launches==1,"craft_native_cast_started_not_completed");
            require(!CharacterDatabase.HasOpenTransaction() && DefersNativeSave(bot->GetGUIDLow()),"craft_no_cross_tick_transaction_and_save_held");
            require(!(paid ? DispatchProfessionAttempt(request.task.actor,operation.transition.receipt) :
                DispatchSavedOperation(operation.transition.receipt,grant,craft)).executed,"craft_pending_cast_not_replayed");
            require(!bot->GetPlayerbotAI()->HasSpellItems(2329,nullptr),"bound_native_craft_cannot_skip_real_reagents");
            if (interrupted) {
                const auto& pending=state->operations.at(operation.transition.receipt);
                require(pending.craft && pending.craft->Capture()->Phase()==CraftCapturePhase::Casting &&
                    bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
                    bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)->m_spellInfo->Id==2329,
                    "craft_interruption_targets_actual_pending_native_spell");
                // Fault injection only: use native cancellation before resource
                // effects. No simulated success, combat flag clearing, or SQL edit.
                bot->InterruptSpell(CURRENT_GENERIC_SPELL,false);
                const auto cancelled=pending.craft->Capture()->ReadFinished();
                require(cancelled && cancelled->nativeFinished && !cancelled->nativeSucceeded &&
                    !cancelled->effectEntered && !cancelled->createdCalls,
                    "craft_native_interruption_observed_without_effect");
            }
            fixture.step=5;return;
        }
        if (fixture.step==5) {
            ProfessionHistory history;
            if (!ReadProfessionHistory(request.task.actor,request.task.id,request.task.revision,history,fixture.blocker)) return;
            require(history.complete && !history.unresolvedOperation && history.attempts.size()==1,
                "craft_saved_history_read_and_decoded_through_bounded_queue");
            const auto& proof=history.attempts.front();
            require(proof.committed && proof.receipt.id==operation.transition.receipt &&
                proof.receipt.state==(interrupted ? OperationState::Rejected : OperationState::Verified) &&
                proof.nativeEffectVerified==!interrupted,"craft_history_matches_actual_saved_native_outcome");
            ProfessionSnapshot snapshot;
            require(ReadProfessionSnapshot(request.task.actor,request.task.id,request.task.revision,snapshot,fixture.blocker),
                "craft_fresh_native_snapshot_uses_saved_history");
            const auto decision=NextProfessionStep(request.task,snapshot);
            fixture.snapshot.put("skill",snapshot.skill);fixture.snapshot.put("output_per_attempt",snapshot.outputPerAttempt);
            fixture.snapshot.put("tools",snapshot.tools);fixture.snapshot.put("at_station",snapshot.atStation);
            fixture.snapshot.put("known_recipe",snapshot.knownRecipe);fixture.snapshot.put("useful_for_another_attempt",snapshot.useful);
            fixture.snapshot.put("blocker",snapshot.blocker);fixture.snapshot.put("next_step",unsigned(decision.step));
            require(decision.step==(interrupted ? ProfessionStep::Defer : ProfessionStep::Finalize) &&
                decision.verifiedAttempts==(interrupted ? 0u : 1u) && decision.verifiedOutput==(interrupted ? 0u : 1u),
                "craft_next_step_recognizes_success_or_exhausted_cancellation");
            require(snapshot.skill==bot->GetSkillValuePure(SKILL_ALCHEMY) && snapshot.outputPerAttempt==1 &&
                snapshot.tools && snapshot.atStation,"craft_snapshot_native_skill_tools_station_output");
            if (settling) require(!snapshot.useful && snapshot.blocker=="profession_target_met_requires_settlement",
                "profession_met_target_stops_new_work_without_erasing_native_facts");
            auto staleHistory=history;++staleHistory.revision;ProfessionSnapshot rejected;std::string staleBlocker;
            require(!InspectNativeProfessionSnapshot(*bot,request.task,staleHistory,NowMs(),rejected,staleBlocker) &&
                !rejected.complete && staleBlocker=="profession_snapshot_history_not_current",
                "craft_snapshot_rejects_stale_history_before_native_planning");
            auto staleTask=request.task;staleTask.context.boot=NewId();
            require(!InspectNativeProfessionSnapshot(*bot,staleTask,history,NowMs(),rejected,staleBlocker) &&
                !rejected.complete && staleBlocker=="profession_snapshot_context_changed",
                "craft_snapshot_rejects_old_boot_authority");
            ProfessionJob job;std::string blocker;require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker),"craft_result_job_decoded");
            require(ReadNativeCraftFrame(*bot,job,fixture.after,blocker),"craft_after_native_frame");
            require(fixture.before.money==fixture.after.money,"craft_money_conserved");
            fixture.reservation.changes.clear();unsigned gained=0;
            if (interrupted) require(SameCraftFrame(fixture.before,fixture.after),"interrupted_craft_inventory_skill_and_money_unchanged");
            for (const auto& input : operation.consumption) {
                const auto* claim=state->resources.Inspect(input.before.id);
                if (interrupted) {
                    require(claim && claim->state=="held" && claim->revision==input.before.revision,"interrupted_craft_inputs_not_consumed");
                    auto released=*claim;released.state="released";++released.revision;
                    fixture.reservation.changes.push_back({released,claim->revision});
                } else {
                    const auto remaining=input.before.quantity-input.used;
                    require(claim && claim->state==(remaining ? "held" : "consumed") && claim->revision==input.before.revision+1 &&
                        claim->quantity==(remaining ? remaining : input.used),"craft_input_consumed_once_in_saved_receipt");
                    if (remaining) {
                        auto released=*claim;released.state="released";++released.revision;
                        fixture.reservation.changes.push_back({released,claim->revision});
                    }
                }
            }
            for (const auto& stack : NativeGainStacks(*bot,operation.itemGain)) {
                const auto* claim=state->resources.Inspect(ItemGainClaimId(operation.transition.receipt,stack.guid));
                if (!claim) continue;
                require(claim->state=="held" && claim->quantity<=stack.count,"craft_output_saved_and_claimed");gained+=claim->quantity;
                auto released=*claim;released.state="released";++released.revision;fixture.reservation.changes.push_back({released,claim->revision});
            }
            require(gained==(interrupted ? 0u : 1u),"craft_exact_native_output_claimed");
            require(!DispatchSavedOperation(operation.transition.receipt,{},craft).executed,"craft_saved_operation_not_replayed");
            if (settling) {
                if (paid) {
                    fixture.blocker=AdvanceProfessionJob(request.task.actor,request.task.id).blocker;
                    require(followRunnerWrite() && request.task.phase==Phase::Completed,"profession_verified_job_settlement_queued");
                } else {
                    request.expectedRevision=request.task.revision;request.receipt=NewId();
                    require(SettleProfessionJob(request.task.actor,request.task.id,request.expectedRevision,request.receipt).code==
                        AdmissionCode::Pending,"profession_verified_job_settlement_queued");
                    ++request.task.revision;
                }
                require(SettleProfessionJob(request.task.actor,request.task.id,request.expectedRevision,request.receipt).code==
                    AdmissionCode::Pending,"profession_duplicate_settlement_shares_pending_write");
                for (const auto& change : fixture.reservation.changes)
                    require(state->resources.Inspect(change.after.id)->state=="held","profession_claim_held_until_saved_completion");
                fixture.step=9;return;
            }
            advance(Phase::Preparing);fixture.step=6;return;
        }
        if (fixture.step==6) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_craft_fixture");
            require(grant.Permitted(),"craft_fixture_release_authority");
            auto& reservation=fixture.reservation;reservation.transition=request;reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision;reservation.transition.receipt=NewId();reservation.transition.task.updatedAtMs=NowMs();
            reservation.authorization=grant.action;
            require(SubmitResourceReservation(reservation,reserve).code==AdmissionCode::Pending,"craft_fixture_output_release_queued");
            request=reservation.transition;fixture.step=7;return;
        }
        if (fixture.step==7) {
            for (const auto& change : fixture.reservation.changes) {
                const auto* claim=state->resources.Inspect(change.after.id);
                require(claim && claim->state=="released" && claim->revision==change.after.revision,"craft_fixture_output_release_saved");
            }
            request.expectedRevision=request.task.revision++;request.task.phase=Phase::Cancelled;request.task.updatedAtMs=NowMs();
            request.task.checkpoint.blocker="isolated_craft_operation_fixture_finished";
            state->Queue(request.task,request.expectedRevision,"isolated_craft_fixture_cleanup");state->nextWork=0;fixture.step=8;return;
        }
        if (fixture.step==9) {
            require(settling && saved->second.phase==Phase::Completed &&
                saved->second.checkpoint.step=="profession_completed" && saved->second.checkpoint.blocker.empty(),
                "profession_skill_job_completed_through_saved_settlement");
            require(SettleProfessionJob(request.task.actor,request.task.id,request.expectedRevision,request.receipt).code==
                AdmissionCode::Saved,"profession_duplicate_completion_is_acknowledged_without_replay");
            UnsettledClaimBatch remaining;std::string blocker;
            require(state->resources.ReadUnsettled(request.task.id,remaining,blocker) && remaining.complete && remaining.claims.empty(),
                "profession_completed_job_has_no_outstanding_claims");
            for (const auto& change : fixture.reservation.changes) {
                const auto* claim=state->resources.Inspect(change.after.id);
                require(claim && SameResourceClaim(*claim,change.after),"profession_saved_completion_releases_exact_output_claim");
            }
            ProfessionJob job;CraftFrame current;
            require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker) && ReadNativeCraftFrame(*bot,job,current,blocker) &&
                SameCraftFrame(current,fixture.after),"profession_settlement_preserves_native_goods_skill_and_money");
            require(!AcquireSavedTask(request.task.id,saved->second.revision,effects,60000,"isolated_craft_fixture").Permitted(),
                "profession_completed_job_cannot_acquire_an_execution_lease");
        } else require(saved->second.phase==Phase::Cancelled,"craft_fixture_closed_without_claiming_whole_job_completion");
        emit(true,"");
    } catch (const std::exception& error) {emit(false,error.what());}
}
