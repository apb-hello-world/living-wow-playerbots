// Explicit copied-realm fixture for the REAL vendor adapter and journal.
// The test positions one safely available bot at a database-backed service;
// it grants no money, spell, skill or item. Native purchase costs/goods remain
// owned after the test. This proves purchase, not travel or a completed craft.
namespace {
    class VendorFixtureReservation final : public NativeReservationAdapter {
    public:
        explicit VendorFixtureReservation(const NativeVendorQuote& quote) : quote(quote) {}
        bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) override {
            const auto& task=request.transition.task;
            bool valid=IsProfessionJob(task) && (task.sourceKey.find("isolated_vendor_v1:")==0 ||
                (task.sourceKey.find("isolated_craft_v1:")==0 &&
                 std::string(std::getenv("LIVING_WOW_NATIVE_FIXTURE"))=="activity-profession-paid-v1")) &&
                actor.GetGUIDLow()==quote.actor && !request.changes.empty();
            for (const auto& change : request.changes) {
                const auto& claim=change.after;
                valid=valid && claim.actor==quote.actor && claim.task==task.root &&
                    ((claim.location=="money" && claim.copper==quote.copper && claim.state=="held") ||
                    (claim.location=="bags" && claim.itemEntry==quote.entry && claim.state=="released"));
            }
            if (!valid) blocker="isolated_vendor_reservation_mismatch";
            return valid;
        }
    private:
        NativeVendorQuote quote;
    };
}

void LivingActivityCoordinator::RunIsolatedVendorFixture() {
    MANGOS_ASSERT(OnWorldThread());
    RequireIsolatedNativeFixtureEnvironment();
    const bool paid=std::string(std::getenv("LIVING_WOW_NATIVE_FIXTURE"))=="activity-profession-paid-v1";
    state->vendorFixtureFaults=std::string(std::getenv("LIVING_WOW_NATIVE_FIXTURE"))=="activity-vendor-failure-v1";
    if (state->fixtureFinished || NowMs()<state->fixtureNext) return;
    state->fixtureNext=NowMs()+1000;
    if (sWorld.GetActiveAndQueuedSessionCount()) return;
    if (!state->loaded || state->ioPending || !state->pending.empty() || !state->incoming.empty() ||
        !state->resources.Protection().ready) return;
    if (!state->admissionFixtureDeadline) state->admissionFixtureDeadline=NowMs()+180000;
    auto& request=state->admissionFixtureRequest;
    auto& operation=state->operationFixtureRequest;
    auto& quote=state->vendorFixtureQuote;
    auto emit=[&](bool passed,const std::string& reason) {
        boost::property_tree::ptree report;
        report.put("suite",paid ? "activity-profession-paid-v1" : state->vendorFixtureFaults ? "activity-vendor-failure-v1" : "activity-vendor-v1"); report.put("boot",state->boot);
        report.put("test_only",true); report.put("outcome",passed ? "passed" : "failed");
        report.put("failure",reason); report.put("last_blocker",state->vendorFixtureBlocker);
        report.put("fixture_step",state->admissionFixtureStep);
        report.put("actor",request.task.actor); report.put("task",request.task.id);
        report.put("operation",operation.transition.receipt); report.put("native_dispatches",state->operationFixtureCalls);
        report.put("general_execution_enabled",false); report.put("paid_crafting_completion_proof",false);
        unsigned suppliedItems=0;
        for (const auto& grant : state->craftFixture.grants) suppliedItems+=grant.second.get<unsigned>("quantity");
        report.put("travel_proof",false); report.put("gold_granted",0); report.put("items_granted",suppliedItems);
        report.put("spell_grants",0); report.put("fixture_position_changes",state->vendorFixturePositionChanged ? 1 : 0);
        report.add_child("fixture_material_grants",state->craftFixture.grants);
        report.put("native_money_before",quote.moneyBefore); report.put("native_price",quote.copper);
        report.put("entry",quote.entry); report.put("purchased_quantity",quote.quantity);
        report.put("native_count_before",state->vendorFixtureCountBefore);
        report.add_child("selection_rejections",state->vendorFixtureSelection);
        report.put("fault_injection_requested",state->vendorFixtureFaults);
        if (state->vendorFixtureFaults) {
            report.put("native_save_attempts",state->nativeSaveFixtureAttempts);
            report.put("lost_acknowledgement_injected",state->nativeSaveFixtureLostAck);
            report.add_child("native_save_checks",state->nativeSaveFixtureChecks);
        }
        auto* bot=sRandomPlayerbotMgr.GetPlayerBot(request.task.actor);
        if (bot) {
            report.put("native_money_after",bot->GetMoney());
            report.put("native_count_after",bot->GetItemCount(quote.entry,false));
            const bool safe=bot->IsInWorld() && !NativeSafety(bot) && !DefersNativeSave(bot->GetGUIDLow());
            if (state->vendorFixturePositionChanged && safe) {
                bot->NearTeleportTo(state->vendorFixtureX,state->vendorFixtureY,state->vendorFixtureZ,state->vendorFixtureO);
                report.put("fixture_position_restored",true);
            } else report.put("fixture_position_restored",false);
        }
        report.add_child("checks",state->admissionFixtureChecks);
        std::ofstream evidence("/isolated/evidence/activity-vendor-"+state->boot+".json");
        evidence<<Json(report); evidence.close();
        if (!evidence) sLog.outError("Living isolated vendor evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
        state->gameplayFixtureActor.store(0,std::memory_order_release); state->fixtureFinished=true;
    };
    if (NowMs()>state->admissionFixtureDeadline) {emit(false,"fixture_deadline");return;}
    auto require=[&](bool value,const char* name) {
        boost::property_tree::ptree check;check.put("case",name);check.put("passed",value);
        state->admissionFixtureChecks.push_back({"",check});
        if (!value) throw std::runtime_error(name);
    };
    try {
        if (!request.task.actor) {
            // Explicit fixture content from this copied TBC database. Resolve
            // its actual spawn and validate native service/faction/stock later.
            // Both factions/continents have a real vial vendor in this copied
            // fixture database. Native stock/access validation still runs at
            // arrival and purchase; these are test destinations, not planner policy.
            // One bounded current snapshot, not one repeated check/history row
            // per second. Guild membership alone is not a service commitment:
            // native reservations and the actual executor still validate it.
            state->vendorFixtureSelection.clear();
            auto skipped=[&](const char* reason) {
                state->vendorFixtureSelection.put(reason,state->vendorFixtureSelection.get<unsigned>(reason,0)+1);
            };
            for (const auto& binding : state->bindings) {
                auto* bot=sRandomPlayerbotMgr.GetPlayerBot(binding.first);
                if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld()) {skipped("native_actor_unavailable");continue;}
                const bool alliance=bot->GetTeam()==ALLIANCE;
                const auto spawnId=bot->GetMapId()==1 ? (alliance ? 46560u : 4687u) :
                    bot->GetMapId()==0 ? (alliance ? 79831u : 17628u) : 0u;
                const auto entry=bot->GetMapId()==1 ? (alliance ? 4229u : 3348u) : (alliance ? 1313u : 4610u);
                const auto* spawn=spawnId ? sObjectMgr.GetCreatureData(spawnId) : nullptr;
                if (!spawn || spawn->id!=entry || bot->GetMapId()!=spawn->mapid) {skipped("service_map_or_faction");continue;}
                if (NativeSafety(bot)) {skipped("native_safety");continue;}
                if (bot->GetGroup() || bot->GetTradeData() || state->authority.Read(binding.first).lease.actor) {
                    skipped("existing_commitment");continue;
                }
                if (bot->GetMoney()<20 || sPlayerbotActionBroker.ReservedCopper(bot->GetGUIDLow())) {
                    skipped("money_or_reserved_trade");continue;
                }
                if (!bot->HasSpell(2329)) {skipped("required_recipe_not_known");continue;}
                if (bot->GetItemCount(3371,true)) {skipped("required_material_already_owned");continue;}
                ProfessionJob job;job.recipe=2329;job.skill=SKILL_ALCHEMY;
                job.purpose=paid ? ProfessionPurpose::SkillGain : ProfessionPurpose::Intermediate;
                job.outputQuantity=1;job.attemptLimit=1;
                const auto native=InspectNativeProfessionRecipe(*bot,job);
                if (!native.blocker.empty() || native.operation!=ProfessionOperation::CreateItem) {
                    skipped("native_recipe_prerequisite");continue;
                }
                job.outputEntry=native.outputEntry;job.reagents=native.reagents;job.initialSkill=native.skillValue;
                if (paid) {
                    job.targetSkill=native.skillValue+1;std::string blocker;CraftFrame frame;
                    if (!MatchNativeProfessionRecipe(job,native,blocker) || !ReadNativeCraftFrame(*bot,job,frame,blocker)) {
                        skipped("skill_goal_or_inventory_unavailable");continue;
                    }
                    bool protectedInput=false;
                    for (const auto& reagent : job.reagents) {
                        unsigned stacks=0;
                        for (const auto& item : frame.stacks) if (item.entry==reagent.entry) {
                            ++stacks;protectedInput|=state->resources.Protection().ProtectedItem(binding.first,item.guid,item.entry)!=0 ||
                                sPlayerbotActionBroker.IsItemReserved(item.guid);
                        }
                        protectedInput|=stacks>1 || sGuildSupplies.ReservedEntry(binding.first,reagent.entry)!=0;
                    }
                    if (protectedInput) {skipped("protected_or_mixed_input");continue;}
                }
                request.task.source="profession_job";
                request.task.sourceKey=(paid ? "isolated_craft_v1:" : "isolated_vendor_v1:")+state->boot;
                request.task.id=request.task.root=SourceId(request.task.source,request.task.sourceKey);
                request.task.actor=binding.first;request.task.kind=Kind::Profession;request.task.accepted=true;
                request.task.priority=Priority::Delivery;request.task.mode=Mode::Active;request.task.phase=Phase::Queued;
                request.task.checkpoint.step="profession_prepare";request.task.checkpoint.data=EncodeProfessionJob(job);
                request.task.createdAtMs=request.task.updatedAtMs=NowMs();request.receipt=NewId();
                state->vendorFixtureX=bot->GetPositionX();state->vendorFixtureY=bot->GetPositionY();
                state->vendorFixtureSpawn=spawnId;state->vendorFixtureEntry=entry;
                state->vendorFixtureZ=bot->GetPositionZ();state->vendorFixtureO=bot->GetOrientation();
                state->gameplayFixtureActor.store(binding.first,std::memory_order_release);
                bot->GetMotionMaster()->Clear();bot->StopMoving();
                bot->NearTeleportTo(spawn->posX,spawn->posY,spawn->posZ,spawn->orientation);
                state->vendorFixturePositionChanged=true;
                return; // Let the native map/grid complete the relocation.
            }
            state->vendorFixtureBlocker="no_safe_known_recipe_actor_with_missing_vendor_material";return;
        }
        auto* bot=sRandomPlayerbotMgr.GetPlayerBot(request.task.actor);
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported()) {state->vendorFixtureBlocker="fixture_actor_transfer";return;}
        require(bot->GetPlayerbotAI() && !NativeSafety(bot) && !bot->GetGroup(),"fixture_actor_safe");
        const bool original=state->enforceEffects.exchange(true,std::memory_order_acq_rel);
        struct Restore {State& state;bool original;~Restore(){state.enforceEffects.store(original,std::memory_order_release);}}
            restore{*state,original};
        const uint32_t effects=Mask(Effect::Money)|Mask(Effect::Inventory);
        if (!state->admissionFixtureStep) {
            const auto vendor=ObjectGuid(HIGHGUID_UNIT,state->vendorFixtureEntry,state->vendorFixtureSpawn).GetRawValue();
            const auto* material=sObjectMgr.GetItemPrototype(3371);
            require(material && material->BuyCount,"fixture_native_vendor_material_exists");
            if (!InspectNativeVendorQuote(*bot,vendor,3371,material->BuyCount,quote,state->vendorFixtureBlocker)) return;
            if (paid) {
                ProfessionJob job;std::string blocker;
                require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker),"paid_job_recipe_decoded");
                for (const auto& reagent : job.reagents) {
                    if (reagent.entry==3371) continue; // Must really be bought, never supplied.
                    const auto owned=bot->GetItemCount(reagent.entry,false);
                    if (owned>=reagent.perAttempt) continue;
                    const auto missing=reagent.perAttempt-owned;
                    require((reagent.entry==765 || reagent.entry==2449) && missing==1,"paid_fixture_only_supplies_missing_herbs");
                    ItemPosCountVec destinations;
                    require(bot->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,reagent.entry,missing)==EQUIP_ERR_OK,
                        "paid_fixture_herb_capacity");
                    auto* added=bot->StoreNewItem(destinations,reagent.entry,true);
                    boost::property_tree::ptree grant;grant.put("entry",reagent.entry);grant.put("quantity",missing);
                    grant.put("count_before",owned);grant.put("count_after",bot->GetItemCount(reagent.entry,false));
                    grant.put("native_item",added ? added->GetGUIDLow() : 0);state->craftFixture.grants.push_back({"",grant});
                    require(added && bot->GetItemCount(reagent.entry,false)==owned+missing,"paid_fixture_herb_supplied_natively");
                }
            }
            request.task.context=ReadNativeContext(*bot,state->policyRevision,state->boot);
            state->vendorFixtureCountBefore=bot->GetItemCount(3371,false);bot->SaveToDB();
            const auto admitted=SubmitTask(request);state->vendorFixtureBlocker=admitted.blocker;
            require(admitted.code==AdmissionCode::Pending,"native_profession_intent_admitted");
            state->admissionFixtureStep=1;return;
        }
        const auto saved=state->cache.find(request.task.id);
        require(saved!=state->cache.end() && saved->second.revision==request.task.revision,"exact_fixture_receipt_saved");
        require(request.task.context==ReadNativeContext(*bot,state->policyRevision,state->boot),"fixture_context_current");
        auto advance=[&](Phase phase) {
            request.expectedRevision=request.task.revision++;request.receipt=NewId();
            request.task.phase=phase;request.task.updatedAtMs=NowMs();
            const auto result=SubmitTask(request);state->vendorFixtureBlocker=result.blocker;
            require(result.code==AdmissionCode::Pending,"fixture_preparation_transition_queued");
        };
        if (state->admissionFixtureStep==1) {advance(Phase::Preparing);state->admissionFixtureStep=2;return;}
        VendorFixtureReservation reserve(quote);
        NativeProfessionPurchasePrerequisites prerequisites;NativeVendorPurchase purchase(quote,prerequisites);
        if (state->admissionFixtureStep==2) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_vendor_fixture");
            require(grant.Permitted(),"vendor_reservation_authority_granted");
            auto& reservation=state->reservationFixtureRequest;
            reservation.transition=request;reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision;reservation.transition.receipt=NewId();
            reservation.transition.task.updatedAtMs=NowMs();reservation.authorization=grant.action;
            ResourceClaim claim;claim.id=NewId();claim.task=request.task.id;claim.actor=request.task.actor;
            claim.copper=quote.copper;claim.location="money";claim.state="held";
            reservation.changes={{claim,0}};
            require(SubmitResourceReservation(reservation,reserve).code==AdmissionCode::Pending,"exact_native_price_reserved");
            request=reservation.transition;state->admissionFixtureStep=3;return;
        }
        if (state->admissionFixtureStep==3) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_vendor_fixture");
            require(grant.Permitted(),"vendor_intent_authority_granted");
            if (operation.transition.receipt.empty()) {
                const auto* claim=state->resources.Inspect(state->reservationFixtureRequest.changes.front().after.id);
                require(claim && claim->state=="held","native_price_claim_acknowledged");
                operation.transition=request;operation.transition.expectedRevision=request.task.revision;
                ++operation.transition.task.revision;operation.transition.receipt=NewId();
                operation.transition.task.updatedAtMs=NowMs();operation.transition.task.phase=Phase::Executing;
                operation.kind="vendor_purchase";operation.effects=effects;operation.persistence=NativePersistence::Inventory;
                operation.beforeState=EncodeNativeVendorQuote(quote);operation.consumption={{*claim,quote.copper}};
                operation.itemGain={quote.entry,quote.quantity};
            }
            operation.authorization=grant.action;
            // The real budget provider schedules its existing bounded async
            // read. No permissive fixture substitute and no new DB worker.
            if (!purchase.PrepareDispatch(*bot,operation,state->vendorFixtureBlocker)) return;
            const auto admitted=SubmitOperationIntent(operation,purchase);state->vendorFixtureBlocker=admitted.blocker;
            require(admitted.code==AdmissionCode::Pending,"native_vendor_operation_intent_saved_before_purchase");
            require(!DispatchSavedOperation(operation.transition.receipt,grant,purchase).executed,
                "unacknowledged_vendor_intent_cannot_purchase");
            request=operation.transition;state->admissionFixtureStep=4;return;
        }
        if (state->admissionFixtureStep==4) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_vendor_fixture");
            require(grant.Permitted(),"saved_vendor_operation_granted");
            const auto result=DispatchSavedOperation(operation.transition.receipt,grant,purchase);
            state->vendorFixtureBlocker=result.admission.blocker;
            if (!result.executed && result.admission.code==AdmissionCode::Pending) return;
            if (result.executed) ++state->operationFixtureCalls;
            require(result.executed && result.outcomeQueued && state->operationFixtureCalls==1,"native_vendor_dispatched_once");
            require(!DispatchSavedOperation(operation.transition.receipt,grant,purchase).executed,"pending_purchase_not_replayed");
            require(state->pending.size()==1 && state->pending.front().nativeSave &&
                state->pending.front().task.phase==Phase::Verifying,"native_acquired_items_and_money_save_queued");
            if (state->vendorFixtureFaults) {
                require(state->vendorFixtureCountBefore==0,"failure_fixture_has_no_preexisting_material_stack");
                state->pending.front().nativeSave->InjectIsolatedProofFailures();
                require(DefersNativeSave(bot->GetGUIDLow()),"native_item_save_hold_prevents_later_save_overtaking");
                bot->SaveToDB();
            }
            request.task=state->pending.front().task;state->admissionFixtureStep=5;return;
        }
        if (state->admissionFixtureStep==5) {
            require(!DefersNativeSave(bot->GetGUIDLow()) && state->operations.empty(),"native_purchase_receipt_acknowledged");
            if (state->vendorFixtureFaults) {
                require(state->nativeSaveFixtureAttempts==4 && state->nativeSaveFixtureLostAck &&
                    state->nativeSaveFixtureChecks.size()==4,"native_item_two_rollbacks_commit_and_lost_ack_reconciled");
                for (const auto& check : state->nativeSaveFixtureChecks)
                    require(check.second.get<bool>("native_money_claim_operation_and_receipt_agree") &&
                        check.second.get<bool>("native_items_and_claims_agree"),"native_item_and_money_transactions_conserved_on_every_attempt");
            }
            require(bot->GetMoney()==quote.moneyBefore-quote.copper &&
                bot->GetItemCount(quote.entry,false)==state->vendorFixtureCountBefore+quote.quantity,"native_purchase_conserved");
            const auto* money=state->resources.Inspect(operation.consumption.front().before.id);
            require(money && money->state=="consumed" && money->revision==2,"native_money_claim_consumed_once");
            require(!DispatchSavedOperation(operation.transition.receipt,{},purchase).executed,"saved_purchase_not_replayed");
            // Check every exact acquired claim before releasing the fixture's
            // ownership. Goods remain in native inventory; no refund/delete.
            state->reservationFixtureRequest.changes.clear();uint32_t acquired=0;
            for (const auto& stack : NativeGainStacks(*bot,{quote.entry,quote.quantity})) {
                const auto* claim=state->resources.Inspect(ItemGainClaimId(operation.transition.receipt,stack.guid));
                if (!claim) continue;
                require(claim->task==request.task.root && claim->actor==request.task.actor && claim->state=="held" &&
                    claim->itemGuid==stack.guid && claim->quantity<=stack.count,"saved_acquisition_matches_native_stack");
                acquired+=claim->quantity;auto released=*claim;released.state="released";++released.revision;
                state->reservationFixtureRequest.changes.push_back({released,claim->revision});
                if (paid) state->craftFixture.inheritedInputs.push_back(claim->id);
            }
            require(acquired==quote.quantity,"all_native_purchase_units_have_saved_claims");
            if (paid) {
                require(state->craftFixture.inheritedInputs.size()==1 && quote.entry==3371 && quote.quantity==5,
                    "paid_purchase_has_one_exact_native_bundle_claim");
                auto& craft=state->craftFixture;ProfessionJob job;std::string blocker;
                require(DecodeProfessionJob(request.task.checkpoint.data,job,blocker) &&
                    ReadNativeCraftFrame(*bot,job,craft.before,blocker),"paid_craft_handoff_reads_purchased_native_goods");
                // Same accepted job, same acquired claim, no cancellation or
                // release. The ordinary craft adapter consumes one real vial.
                craft.request=request;craft.step=1;craft.deadline=0;
                require(craft.before.money==quote.moneyBefore-quote.copper,"paid_job_retains_actual_purchase_cost");
                state->admissionFixtureStep=9;return;
            }
            advance(Phase::Preparing);state->admissionFixtureStep=6;return;
        }
        if (state->admissionFixtureStep==6) {
            const auto grant=AcquireSavedTask(request.task.id,request.task.revision,effects,60000,"isolated_vendor_fixture");
            require(grant.Permitted(),"fixture_release_authority");
            auto& reservation=state->reservationFixtureRequest;
            reservation.transition=request;reservation.transition.expectedRevision=request.task.revision;
            ++reservation.transition.task.revision;reservation.transition.receipt=NewId();
            reservation.transition.task.updatedAtMs=NowMs();reservation.authorization=grant.action;
            require(SubmitResourceReservation(reservation,reserve).code==AdmissionCode::Pending,"fixture_goods_claim_release_queued");
            request=reservation.transition;state->admissionFixtureStep=7;return;
        }
        if (state->admissionFixtureStep==7) {
            for (const auto& change : state->reservationFixtureRequest.changes) {
                const auto* claim=state->resources.Inspect(change.after.id);
                require(claim && claim->state=="released" && claim->revision==change.after.revision,"fixture_goods_release_saved");
            }
            request.expectedRevision=request.task.revision++;request.task.phase=Phase::Cancelled;
            request.task.updatedAtMs=NowMs();request.task.checkpoint.blocker="isolated_purchase_fixture_finished";
            state->Queue(request.task,request.expectedRevision,"isolated_vendor_fixture_cleanup");state->nextWork=0;
            state->admissionFixtureStep=8;return;
        }
        require(saved->second.phase==Phase::Cancelled,"fixture_cancelled_without_claiming_craft_completion");
        state->enforceEffects.store(original,std::memory_order_release);emit(true,"");
    } catch (const std::exception& error) {emit(false,error.what());}
}
