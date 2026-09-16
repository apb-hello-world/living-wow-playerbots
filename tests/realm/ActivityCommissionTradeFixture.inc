// Isolated accepted-order integration. The fixture supplies recorded missing
// reagents and customer UI consent only. The ordinary due queue must craft,
// journal the exchange and settle the order. No output, fee or receipt grants.
namespace {
template<class FixtureState>
void RunIsolatedCommissionTradeFixture(FixtureState& state,bool automaticOffer=false,bool brokerCase=false) {
    struct Fixture {
        uint32_t actor=0,customer=0;
        bool actorLogin=false,customerLogin=false,setup=false,admitted=false,offered=false,queuedRestart=false,windowOpened=false;
        uint64_t deadline=0,nextReport=0;
        TaskRequest request;
        ChatDirectorEvent brokerEvent;
        ChatDirectorActionProposal brokerProposal;
        bool brokerAccepted=false,partialStock=false,capacity=false,capacityPrepared=false,bankCapacity=false;
        bool cancelFirstOffer=false,cancelled=false;
        bool meeting=false,meetingPositioned=false;
        uint64_t cancelledAt=0;
        boost::property_tree::ptree input;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto write=[&](const std::string& name,const boost::property_tree::ptree& data) {
        std::ofstream out("/isolated/evidence/"+name);out<<Json(data);out.close();
        require(bool(out),"commission_trade_fixture_evidence_failed");
    };
    auto report=f.input;report.put("suite","activity-commission-trade-v1");report.put("boot",state.boot);
    report.put("test_only",true);report.put("independent_audit_required",true);
    try {
        if(!f.actor) {
            auto id=[&](const char* name) {
                const auto* raw=std::getenv(name);const std::string value=raw?raw:"";
                require(!value.empty() && value.size()<=9 && value.find_first_not_of("0123456789")==std::string::npos,
                    "explicit_commission_trade_actors_required");return uint32_t(std::stoul(value));
            };
            f.actor=id("LIVING_WOW_NATIVE_FIXTURE_ACTOR");f.customer=id("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            f.partialStock=brokerCase && bool(std::ifstream("/isolated/evidence/commission-partition-existing-stock"));
            f.bankCapacity=brokerCase && bool(std::ifstream("/isolated/evidence/commission-bank-full-bags"));
            f.capacity=f.bankCapacity || (brokerCase && bool(std::ifstream("/isolated/evidence/commission-capacity-full-bags")));
            f.cancelFirstOffer=automaticOffer && bool(std::ifstream("/isolated/evidence/commission-cancel-first-offer"));
            f.meeting=brokerCase && bool(std::ifstream("/isolated/evidence/commission-meeting-delivery"));
            require(!f.meeting || (!f.capacity && !f.partialStock && !f.cancelFirstOffer),"separate_meeting_journey_fixture_required");
            require(!f.capacity || f.partialStock,"capacity_fixture_requires_existing_stock");
            require(f.actor && f.customer && f.actor!=f.customer,"distinct_commission_trade_actors_required");
            std::ifstream prior("/isolated/evidence/commission-trade-input-v1.json",std::ios::ate);
            if(prior) {
                require(prior.tellg()>0 && prior.tellg()<=1024*1024,"bounded_original_trade_fixture_required");
                prior.seekg(0);boost::property_tree::read_json(prior,f.input);
                require(f.input.template get<std::string>("setup_boot")!=state.boot &&
                    f.input.template get<uint32_t>("actor")==f.actor && f.input.template get<uint32_t>("recipient")==f.customer &&
                    IsUuid(f.input.template get<std::string>("task")),"original_commission_fixture_actors_required");
                f.request.task.id=f.input.template get<std::string>("task");
                f.meetingPositioned=f.input.template get<bool>("meeting_positioned",false);
                require(f.meeting==f.input.template get<bool>("meeting_case",false),"same_meeting_fixture_required");
                f.setup=f.admitted=f.queuedRestart=true;
            }
            else require(!brokerCase || !std::ifstream("/isolated/evidence/commission-trade-proposal-v1.json"),
                "interrupted_broker_setup_requires_reconciliation");
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            state.professionCohortActors[0].store(f.customer,std::memory_order_release);
            f.deadline=NowMs()+900000;
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        require(NowMs()<f.deadline,"commission_trade_fixture_deadline_task_preserved");
        auto* actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);auto* customer=sRandomPlayerbotMgr.GetPlayerBot(f.customer);
        auto login=[&](Player* bot,uint32_t guid,bool& requested) {
            if(!bot && !requested && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                requested=true;sRandomPlayerbotMgr.AddRandomBot(guid);
            }
        };
        login(actor,f.actor,f.actorLogin);login(customer,f.customer,f.customerLogin);
        if(!actor || !customer)return;
        for(auto* bot:{actor,customer}) {
            require(bot->GetPlayerbotAI() && !bot->GetPlayerbotAI()->IsRealPlayer() && bot->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()),"copied_bot_customer_required");
            if(!bot->IsInWorld() || !bot->GetMap() || NativeSafety(bot) || bot->GetGroup() || bot->GetMap()->IsDungeon() ||
                sLivingActivityCoordinator.DefersNativeSave(bot->GetGUIDLow()))return;
        }
        state.enforceEffects.store(true,std::memory_order_release);
        if(f.queuedRestart) {
            // Resume the original queued order, never create a second one or
            // repeat reagent grants. Other restart phases need their own proof.
            const auto prior=sLivingActivityCoordinator.ReadSavedTask(f.request.task.id);
            CommissionJob job;std::string why;
            require(prior && prior->actor==f.actor &&
                DecodeCommissionJob(prior->checkpoint.data,job,why) &&
                EncodeCommissionContract(job.agreement)==f.input.template get<std::string>("agreement"),
                "original_queued_trade_task_required_for_fixture_resume");
            const bool crafted=automaticOffer && job.craftFinishedRevision &&
                (prior->phase==Phase::Preparing || prior->phase==Phase::Reconciling) &&
                prior->checkpoint.step=="commission_craft_ready";
            const bool interruptedOffer=automaticOffer && f.input.template get<bool>("offer_restart_test",false) &&
                job.craftFinishedRevision && (prior->phase==Phase::Executing || prior->phase==Phase::Reconciling) &&
                prior->checkpoint.step=="commission_trade_offer";
            const bool partitionRestart=automaticOffer && f.partialStock && f.input.template get<bool>("partition_restart_test",false) &&
                job.craftFinishedRevision && (prior->phase==Phase::Executing || prior->phase==Phase::Verifying || prior->phase==Phase::Reconciling) &&
                prior->checkpoint.step=="commission_output_partition";
            const bool capacityRestart=automaticOffer && f.capacity && f.input.template get<bool>("capacity_restart_test",false) &&
                job.craftFinishedRevision && prior->phase==Phase::Executing && prior->checkpoint.step==(f.bankCapacity?"profession_capacity_bank":"profession_capacity_sale");
            const bool meetingRestart=f.meeting && f.meetingPositioned && job.craftFinishedRevision &&
                (prior->phase==Phase::Traveling || prior->phase==Phase::Reconciling) && prior->checkpoint.step=="commission_meeting";
            require(crafted || interruptedOffer || partitionRestart || capacityRestart || meetingRestart || (prior->phase==Phase::Queued && !job.craftFinishedRevision),"only_queued_or_verified_craft_resume");
            auto operations=CharacterDatabase.PQuery("SELECT COUNT(*),SUM(kind='profession_craft' AND state='verified'),SUM(kind='commission_trade_offer' AND state='intent'),SUM(kind='commission_output_partition' AND state='intent'),SUM(kind='commission_output_partition' AND state='verified'),SUM(kind='commission_output_partition' AND state='rejected'),SUM(kind IN ('capacity_vendor_sale','bank_deposit') AND state='intent') FROM living_activity_operation WHERE task_id='%s'",
                prior->id.c_str());
            const bool partitionIntent=partitionRestart && operations && operations->Fetch()[3].GetUInt32()==1;
            const bool partitionSaved=partitionRestart && operations && operations->Fetch()[4].GetUInt32()==1;
            require(operations && operations->Fetch()[0].GetUInt32()==(capacityRestart?2u:partitionIntent?2u:partitionSaved?3u:interruptedOffer?2u:(crafted || meetingRestart)?1u:0u) &&
                (!(crafted || interruptedOffer || partitionRestart || capacityRestart || meetingRestart) || operations->Fetch()[1].GetUInt32()==1) &&
                (!capacityRestart || operations->Fetch()[6].GetUInt32()==1) &&
                (!partitionRestart || (partitionIntent!=partitionSaved && operations->Fetch()[5].GetUInt32()==(partitionSaved?1u:0u))) &&
                (!interruptedOffer || operations->Fetch()[2].GetUInt32()==1),"queued_fixture_cannot_replay_native_effects");
            if(crafted || interruptedOffer || partitionRestart || capacityRestart || meetingRestart) {
                require(actor->GetItemCount(2454,true)==(f.partialStock?2u:1u) &&
                    actor->GetMoney()==f.input.template get<uint32_t>("actor_money_before") &&
                    customer->GetMoney()==f.input.template get<uint32_t>("recipient_money_before") &&
                    customer->GetItemCount(2454,true)==f.input.template get<uint32_t>("recipient_items_before"),
                    "crafted_resume_preserves_original_item_and_money");
                UnsettledClaimBatch claims;
                if(!sLivingActivityCoordinator.ReadTaskClaims(f.actor,prior->id,prior->revision,claims,why))return;
                require(claims.complete && claims.claims.size()==(capacityRestart?2u:1u) &&
                    std::count_if(claims.claims.begin(),claims.claims.end(),[](const auto& c){return c.itemEntry==2454 &&
                        c.quantity==1 && c.location=="bags" && c.state=="held";})==1,"crafted_resume_claim_required");
                if(capacityRestart) {
                    require(!f.input.get_child("capacity_fixture_grants").empty(),"original_capacity_fixture_required");
                    f.capacityPrepared=true;f.input.put("capacity_intent_restart",true);
                }
                f.input.put("verified_craft_restart",true);
                if(meetingRestart) {
                    require(claims.claims.front().id==f.input.template get<std::string>("meeting_claim") &&
                        claims.claims.front().itemGuid==f.input.template get<uint32_t>("meeting_item"),"meeting_restart_same_native_output");
                    f.input.put("meeting_restart",true);
                }
                f.input.put("interrupted_offer_restart",interruptedOffer);
                if(partitionIntent)f.input.put("partition_intent_restart",true);
                if(partitionSaved)f.input.put("partition_verified_restart",true);
            }
            if(brokerCase) {
                f.request.task=*prior;f.brokerAccepted=true;
                f.brokerEvent.eventId=f.input.template get<std::string>("broker_event_id");f.brokerEvent.speakerGuid=f.customer;
                auto& p=f.brokerProposal;p.proposalId="accepted";p.botGuid=f.actor;p.targetGuid=f.customer;
                p.type="craft_commission";p.capabilityRef="spell:craft:2329:2454";p.delivery=f.meeting?"meeting":"direct";p.quantity=1;p.priceCopper=60;
                require(job.agreement.transaction=="wow-tx-"+f.brokerEvent.eventId+'-'+p.proposalId,"original_broker_restart_identity_required");
            }
            write("commission-trade-input-v1.json",f.input);
            f.queuedRestart=false;
        }
        if(!f.setup) {
            require(!actor->GetTradeData() && !customer->GetTradeData() && actor->GetTeam()==customer->GetTeam() &&
                actor->GetMapId()==customer->GetMapId() && customer->GetMoney()>=60,"native_customer_availability_required");
            for(const auto& row:state.cache)if((row.second.actor==f.actor || row.second.actor==f.customer) &&
                row.second.accepted && !Terminal(row.second.phase))require(false,"prior_actor_commitment_preserved");
            require(!state.resources.Protection().actorClaims.count(f.actor) &&
                !state.resources.Protection().actorClaims.count(f.customer),"prior_actor_claims_preserved");
            ProfessionJob recipe;std::string why;
            require(BuildNativeRequestedItemJob(*actor,2329,2454,1,recipe,why),"native_commission_recipe:"+why);
            require(actor->GetItemCount(2454,true)<=(f.partialStock?1u:0u),"bounded_existing_stock_no_output_removal");
            CommissionJob job;job.agreement={"lwc-"+std::to_string(NowMs()),"isolated-direct-v1",f.meeting?"meeting":"direct",
                EncodeProfessionJob(recipe),f.actor,f.customer,60,NowMs()};job.craft=job.agreement.recipe;
            job.agreement.transaction="isolated-direct-"+job.agreement.id;
            if(brokerCase) {
                f.brokerEvent.eventId="isolated-direct-"+std::to_string(NowMs());f.brokerEvent.speakerGuid=f.customer;
                auto& p=f.brokerProposal;p.proposalId="accepted";p.botGuid=f.actor;p.targetGuid=f.customer;
                p.type="craft_commission";p.capabilityRef="spell:craft:2329:2454";p.delivery=f.meeting?"meeting":"direct";p.quantity=1;p.priceCopper=60;
                auto& candidate=f.brokerEvent.candidates[f.actor];candidate.guid=f.actor;
                ChatDirectorCapability capability;capability.capabilityRef=p.capabilityRef;capability.type=p.type;
                capability.itemId=2454;capability.minQuantity=capability.maxQuantity=capability.quantity=1;
                capability.priceCopper=60;capability.deliveries={p.delivery};candidate.actionCapabilities.push_back(capability);
                job.agreement.transaction="wow-tx-"+f.brokerEvent.eventId+'-'+p.proposalId;
                job.agreement.id="lwc-"+std::to_string(std::hash<std::string>{}(job.agreement.transaction));
            }
            auto& task=f.request.task;task.source="commission_job";task.sourceKey=job.agreement.id;
            task.id=task.root=SourceId(task.source,task.sourceKey);task.actor=f.actor;task.kind=Kind::Commission;
            task.mode=Mode::Active;task.accepted=true;task.phase=Phase::Queued;task.priority=Priority::Human;
            task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeCommissionJob(job);f.request.receipt=NewId();
            require(ValidateNativeProfessionTask(*actor,task,why),"native_fixture_task:"+why);
            f.input.put("task",task.id);f.input.put("commission",job.agreement.id);f.input.put("actor",f.actor);
            f.input.put("recipient",f.customer);f.input.put("agreement",EncodeCommissionContract(job.agreement));
            f.input.put("setup_boot",state.boot);f.input.put("actor_money_before",actor->GetMoney());
            f.input.put("recipient_money_before",customer->GetMoney());f.input.put("recipient_items_before",customer->GetItemCount(2454,true));
            f.input.put("actor_items_before",actor->GetItemCount(2454,true));f.input.put("output_grants",0);f.input.put("money_grants",0);
            f.input.put("skill_grants",0);f.input.put("recipe_grants",0);f.input.put("fixture_consent",true);
            f.input.put("autonomous_travel_proof",false);f.input.put("broker_admission_proof",false);
            f.input.put("automatic_offer",automaticOffer);
            f.input.put("broker_case",brokerCase);
            f.input.put("meeting_case",f.meeting);
            f.input.put("capacity_test",f.capacity);
            f.input.put("capacity_bank_test",f.bankCapacity);
            f.input.put("capacity_restart_test",f.capacity && bool(std::ifstream(f.bankCapacity?
                "/isolated/evidence/commission-bank-pause-before-dispatch":"/isolated/evidence/commission-capacity-pause-before-dispatch")));
            f.input.put("cancel_first_offer",f.cancelFirstOffer);
            if(brokerCase)f.input.put("broker_event_id",f.brokerEvent.eventId);
            // Disclosed OLD-stock fixture, not a grant of the ordered output:
            // the verified craft must add one, and this original one must remain.
            f.input.put("preexisting_output_grants",f.partialStock && !actor->GetItemCount(2454,true)?1:0);
            f.input.put("preexisting_output_quantity",f.partialStock?1:0);
            f.input.put("partition_restart_test",f.partialStock && bool(std::ifstream("/isolated/evidence/commission-partition-pause-before-dispatch")));
            f.input.put("offer_restart_test",automaticOffer && bool(std::ifstream("/isolated/evidence/commission-offer-pause-before-dispatch")));
            f.input.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            boost::property_tree::ptree grants;
            for(uint32_t entry:{765u,2449u,3371u})if(!actor->GetItemCount(entry,true)) {
                ItemPosCountVec positions;require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)==EQUIP_ERR_OK,
                    "fixture_material_capacity_required");boost::property_tree::ptree row;row.put("entry",entry);row.put("quantity",1);
                grants.push_back({"",row});
            }
            f.input.add_child("material_grants",grants);
            f.input.put("recipient_original_x",customer->GetPositionX());f.input.put("recipient_original_y",customer->GetPositionY());
            f.input.put("recipient_original_z",customer->GetPositionZ());f.input.put("recipient_original_map",customer->GetMapId());
            write(brokerCase?"commission-trade-proposal-v1.json":"commission-trade-input-v1.json",f.input);
            for(const auto& row:grants) {
                const auto entry=row.second.template get<uint32_t>("entry");ItemPosCountVec positions;
                require(!actor->GetItemCount(entry,true) && actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)==EQUIP_ERR_OK,
                    "fixture_material_changed");
                require(actor->StoreNewItem(positions,entry,true) && actor->GetItemCount(entry,true)==1,"fixture_material_grant_failed");
            }
            if(f.input.template get<uint32_t>("preexisting_output_grants")) {
                ItemPosCountVec positions;
                require(actor->GetItemCount(2454,true)==0 && actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,2454,1)==EQUIP_ERR_OK &&
                    actor->StoreNewItem(positions,2454,true) && actor->GetItemCount(2454,true)==1,"fixture_preexisting_stock_failed");
            }
            actor->SaveToDB();actor->GetMotionMaster()->Clear();actor->StopMoving();
            customer->GetMotionMaster()->Clear();customer->StopMoving();
            customer->NearTeleportTo(actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ(),actor->GetOrientation());
            f.setup=true;return;
        }
        if(!f.admitted) {
            if(brokerCase) {
                auto changed=f.brokerProposal;changed.priceCopper=61;
                const auto invalid=sPlayerbotActionBroker.Create(changed,f.brokerEvent);
                require(!invalid.created && invalid.reasonCode=="craft_fee_changed","direct_broker_changed_fee_must_reject");
                const auto result=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
                require(result.created && result.reasonCode=="commission_queued","direct_broker_admission:"+result.reasonCode);
                const auto replay=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
                require(replay.created && replay.reasonCode=="commission_queued","direct_broker_pending_replay_required");
                f.input.put("broker_pending_replay",true);f.input.put("broker_changed_fee_rejected",true);
            } else {
                const auto result=sLivingActivityCoordinator.SubmitTask(f.request);
                require(result.code==AdmissionCode::Pending,"native_commission_admission:"+result.blocker);
            }
            f.admitted=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.request.task.id);
        if(!saved)return;
        if(brokerCase && !f.brokerAccepted) {
            CommissionJob actual,expected;std::string why;
            require(DecodeCommissionJob(saved->checkpoint.data,actual,why) &&
                DecodeCommissionJob(f.request.task.checkpoint.data,expected,why),"direct_broker_accepted_agreement_required");
            expected.agreement.acceptedAtMs=actual.agreement.acceptedAtMs;
            require(EncodeCommissionContract(expected.agreement)==EncodeCommissionContract(actual.agreement),"direct_broker_agreement_changed");
            f.input.put("agreement",EncodeCommissionContract(actual.agreement));f.input.put("broker_admission_proof",true);
            write("commission-trade-input-v1.json",f.input);f.brokerAccepted=true;
        }
        report=f.input;report.put("suite","activity-commission-trade-v1");report.put("boot",state.boot);
        report.put("test_only",true);report.put("independent_audit_required",true);
        report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
        if(NowMs()>=f.nextReport) {f.nextReport=NowMs()+30000;write("commission-trade-progress.json",report);}
        CommissionJob job;std::string why;
        require(DecodeCommissionJob(saved->checkpoint.data,job,why),"saved_commission_job_required");
        if(f.meeting && job.craftFinishedRevision && !f.meetingPositioned) {
            UnsettledClaimBatch held;if(!sLivingActivityCoordinator.ReadTaskClaims(f.actor,saved->id,saved->revision,held,why))return;
            require(held.complete && held.claims.size()==1 && held.claims.front().itemEntry==2454 &&
                held.claims.front().quantity==1 && held.claims.front().location=="bags","meeting_owned_output_required");
            float x=0,y=0,z=0;bool found=false;
            for(const auto offset:{std::pair<float,float>{40,0},{-40,0},{0,40},{0,-40}}) {
                x=actor->GetPositionX()+offset.first;y=actor->GetPositionY()+offset.second;
                z=actor->GetMap()->GetHeight(x,y,actor->GetPositionZ()+10.0f);
                if(std::isfinite(z) && std::abs(z-actor->GetPositionZ())<3.0f) {found=true;break;}
            }
            require(found,"meeting_safe_fixture_destination_required");
            f.input.put("meeting_start_x",actor->GetPositionX());f.input.put("meeting_start_y",actor->GetPositionY());
            f.input.put("meeting_start_z",actor->GetPositionZ());f.input.put("meeting_map",actor->GetMapId());
            f.input.put("meeting_target_x",x);f.input.put("meeting_target_y",y);f.input.put("meeting_target_z",z);
            f.input.put("meeting_claim",held.claims.front().id);f.input.put("meeting_item",held.claims.front().itemGuid);
            f.input.put("meeting_positioned",true);f.input.put("fixture_customer_placement",true);
            f.input.put("fixture_actor_relocations",0);
            // Disclosed fixture placement only. The customer will now remain
            // here: the real executor must bring the crafter and its output.
            customer->GetMotionMaster()->Clear();customer->StopMoving();customer->NearTeleportTo(x,y,z,customer->GetOrientation());
            customer->SaveToDB();actor->SaveToDB();f.meetingPositioned=true;
            write("commission-trade-input-v1.json",f.input);write("commission-meeting-positioned.json",f.input);return;
        }
        if(f.capacity && !f.capacityPrepared && job.craftFinishedRevision) {
            require(!actor->GetTradeData() && !customer->GetTradeData(),"capacity_fixture_before_any_trade_window");
            UnsettledClaimBatch held;
            if(!sLivingActivityCoordinator.ReadTaskClaims(f.actor,saved->id,saved->revision,held,why))return;
            require(held.complete && held.claims.size()==1 && held.claims.front().itemEntry==2454 &&
                held.claims.front().quantity==1,"capacity_fixture_exact_output_claim");
            auto* output=actor->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,held.claims.front().itemGuid));
            require(output && output->GetCount()==2,"capacity_fixture_partial_output_required");
            require(f.input.template get<uint32_t>("preexisting_output_grants")==0,"capacity_fixture_reuses_owned_stock");
            boost::property_tree::ptree fillReport;
            fillReport.put("task",saved->id);fillReport.put("actor",f.actor);fillReport.put("test_only",true);
            fillReport.put("money_before",actor->GetMoney());fillReport.put("output_count",output->GetCount());
            fillReport.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            uint32_t fillEntry=7073,fillQuantity=5;
            if(f.bankCapacity) {
                fillEntry=0;fillQuantity=20;
                for(const uint32_t entry:{4470u,2589u,2770u}) {
                    const auto* proto=sObjectMgr.GetItemPrototype(entry);
                    require(proto && proto->Class==ITEM_CLASS_TRADE_GOODS && proto->Stackable==20,"native_storable_fixture_item");
                    ai::ItemQualifier qualifier(entry);
                    auto* value=actor->GetPlayerbotAI()->GetAiObjectContext()->template GetValue<ai::ItemUsage>("item usage",qualifier.GetQualifier());
                    value->Reset();const auto usage=value->Get();
                    if(usage==ai::ItemUsage::ITEM_USAGE_BANK || usage==ai::ItemUsage::ITEM_USAGE_SKILL ||
                        usage==ai::ItemUsage::ITEM_USAGE_AH || usage==ai::ItemUsage::ITEM_USAGE_BROKEN_AH) {fillEntry=entry;break;}
                }
                require(fillEntry!=0,"native_fixture_storable_classification_required");
            } else {
                const auto* trash=sObjectMgr.GetItemPrototype(fillEntry);
                require(trash && trash->Quality==ITEM_QUALITY_POOR && trash->SellPrice==6 && trash->Stackable==5,"native_disclosed_capacity_fixture_item");
            }
            f.input.put("capacity_filler_entry",fillEntry);f.input.put("capacity_filler_quantity",fillQuantity);
            boost::property_tree::ptree grants;
            auto fill=[&](uint8_t bag,uint8_t slot) {
                const uint16_t pos=uint16_t(uint16_t(bag)<<8|slot);
                if(actor->GetItemByPos(pos))return;
                ItemPosCountVec positions;
                if(actor->CanStoreNewItem(bag,slot,positions,fillEntry,fillQuantity)!=EQUIP_ERR_OK || positions.size()!=1 ||
                    positions.front().pos!=pos || positions.front().count!=fillQuantity)return;
                auto* item=actor->StoreNewItem(positions,fillEntry,true);
                require(item && item->GetPos()==pos && item->GetCount()==fillQuantity,"capacity_fixture_stack_grant_failed");
                boost::property_tree::ptree row;row.put("guid",item->GetGUIDLow());row.put("entry",fillEntry);row.put("quantity",fillQuantity);
                row.put("bag",bag);row.put("slot",slot);grants.push_back({"",row});
            };
            for(uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)fill(INVENTORY_SLOT_BAG_0,slot);
            for(uint8_t bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag) {
                auto* container=static_cast<Bag*>(actor->GetItemByPos(INVENTORY_SLOT_BAG_0,bag));
                if(container)for(uint8_t slot=0;slot<container->GetBagSize();++slot)fill(bag,slot);
            }
            uint16_t available=0;
            require(!grants.empty() && grants.size()<=80 && !EmptyNativeParcelSlot(*actor,*output,1,available),"capacity_fixture_really_full");
            f.input.add_child("capacity_fixture_grants",grants);f.input.put("capacity_output_claim",held.claims.front().id);
            f.input.put("capacity_claim_revision",held.claims.front().revision);
            actor->SaveToDB();f.capacityPrepared=true;
            fillReport.put("split_blocked",true);fillReport.add_child("grants",grants);
            write("commission-capacity-filled-v1.json",fillReport);
            write("commission-trade-input-v1.json",f.input);write("commission-capacity-prepared-input.json",f.input);return;
        }
        if(saved->phase==Phase::Completed) {
            if(brokerCase) {
                const auto replay=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
                require(replay.created && replay.reasonCode=="commission_queued","direct_broker_completed_replay_required");
                auto changed=f.brokerProposal;changed.quantity=2;
                const auto conflict=sPlayerbotActionBroker.Create(changed,f.brokerEvent);
                require(!conflict.created && conflict.reasonCode=="commission_agreement_conflict","direct_broker_completed_contract_immutable");
                report.put("broker_completed_replay",true);report.put("broker_completed_conflict_rejected",true);
            }
            require(f.offered && (!f.cancelFirstOffer || f.cancelled) && actor->GetItemCount(2454,true)==(f.partialStock?1u:0u) &&
                customer->GetItemCount(2454,true)==f.input.template get<uint32_t>("recipient_items_before")+1 &&
                (f.capacity || actor->GetMoney()==f.input.template get<uint32_t>("actor_money_before")+60) &&
                customer->GetMoney()+60==f.input.template get<uint32_t>("recipient_money_before"),"native_commission_conservation_required");
            report.put("outcome","passed");report.put("actor_money_after",actor->GetMoney());
            report.put("recipient_money_after",customer->GetMoney());report.put("revision",saved->revision);
            report.put("craft_finished_revision",job.craftFinishedRevision);
            if(f.meeting) { actor->SaveToDB();customer->SaveToDB(); }
            write("activity-commission-trade-"+state.boot+".json",report);state.fixtureFinished=true;return;
        }
        require(!Terminal(saved->phase),"commission_trade_unexpected_terminal");
        if(f.meeting && saved->phase==Phase::Traveling && saved->checkpoint.step=="commission_meeting" &&
            std::ifstream("/isolated/evidence/commission-meeting-pause-before-approach")) {
            report.put("outcome","awaiting_restart_before_native_approach");
            report.put("meeting_revision",saved->revision);
            write("commission-meeting-restart-checkpoint.json",report);return;
        }
        if(f.capacity && saved->phase==Phase::Executing && saved->checkpoint.step==(f.bankCapacity?"profession_capacity_bank":"profession_capacity_sale") &&
            std::ifstream(f.bankCapacity?"/isolated/evidence/commission-bank-pause-before-dispatch":"/isolated/evidence/commission-capacity-pause-before-dispatch")) {
            report.put("outcome","awaiting_restart_before_capacity_sale");
            write("commission-capacity-intent-checkpoint.json",report);return;
        }
        if(f.partialStock && saved->checkpoint.step=="commission_output_partition" &&
            ((saved->phase==Phase::Executing && std::ifstream("/isolated/evidence/commission-partition-pause-before-dispatch")) ||
             (saved->phase==Phase::Verifying && std::ifstream("/isolated/evidence/commission-partition-pause-after-save")))) {
            if(saved->phase==Phase::Verifying) {
                auto proof=CharacterDatabase.PQuery("SELECT COUNT(*) FROM living_activity_operation WHERE task_id='%s' AND kind='commission_output_partition' AND state='verified'",saved->id.c_str());
                if(!proof || proof->Fetch()[0].GetUInt32()!=1)return;
            }
            report.put("outcome",saved->phase==Phase::Executing?"awaiting_restart_before_partition":"awaiting_restart_after_partition");
            write(saved->phase==Phase::Executing?"commission-partition-intent-checkpoint.json":"commission-partition-saved-checkpoint.json",report);return;
        }
        if(saved->phase==Phase::Executing && saved->checkpoint.step=="commission_trade_offer" &&
            std::ifstream("/isolated/evidence/commission-offer-pause-before-dispatch")) {
            report.put("outcome","awaiting_restart_before_offer_dispatch");
            write("commission-offer-restart-checkpoint.json",report);return;
        }
        // A customer window must not block the ordinary executor's restart
        // context validation. Recovery runs first, preserving the same order.
        if(!(saved->context==ReadNativeContext(*actor,state.policyRevision,state.boot)))return;
        if(!job.craftFinishedRevision || f.offered || saved->phase==Phase::Executing)return;
        if(f.meeting) {
            if(!actor->IsWithinDistInMap(customer,INTERACTION_DISTANCE))return;
            if(saved->phase!=Phase::Preparing && saved->phase!=Phase::WaitingExternal && saved->phase!=Phase::Verifying)return;
            const auto tx=f.input.template get<float>("meeting_target_x"),ty=f.input.template get<float>("meeting_target_y");
            const auto sx=f.input.template get<float>("meeting_start_x"),sy=f.input.template get<float>("meeting_start_y");
            require(std::hypot(customer->GetPositionX()-tx,customer->GetPositionY()-ty)<1.0f &&
                std::hypot(actor->GetPositionX()-sx,actor->GetPositionY()-sy)>25.0f,"native_meeting_approach_required");
            f.input.put("autonomous_travel_proof",true);f.input.put("meeting_arrival_revision",saved->revision);
            f.input.put("meeting_arrival_x",actor->GetPositionX());f.input.put("meeting_arrival_y",actor->GetPositionY());
            write("commission-meeting-arrived.json",f.input);
        }
        if(f.capacity && !f.windowOpened) {
            UnsettledClaimBatch held;if(!sLivingActivityCoordinator.ReadTaskClaims(f.actor,saved->id,saved->revision,held,why))return;
            if(held.claims.size()!=1)return;
            auto* output=actor->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,held.claims.front().itemGuid));
            if(!output || output->GetEntry()!=2454 || output->GetCount()!=1)return;
            if(!actor->IsWithinDistInMap(customer,TRADE_DISTANCE,false)) {
                // The customer fixture follows the bot's real service travel;
                // never move the crafter to a vendor or grant a service result.
                customer->GetMotionMaster()->Clear();customer->StopMoving();
                customer->NearTeleportTo(actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ(),actor->GetOrientation());return;
            }
        }
        require(actor->IsWithinDistInMap(customer,TRADE_DISTANCE,false),"fixture_customer_arrival_required");
        if(f.cancelled && NowMs()<f.cancelledAt+2500)return;
        if(automaticOffer && !f.windowOpened) {
            require(!actor->GetTradeData() && !customer->GetTradeData(),"fresh_customer_trade_required");
            WorldPacket initiate(CMSG_INITIATE_TRADE,8);initiate<<actor->GetObjectGuid();
            customer->GetSession()->HandleInitiateTradeOpcode(initiate);
            require(actor->GetTradeData() && customer->GetTradeData(),"native_trade_window_required");
            WorldPacket begin;actor->GetSession()->HandleBeginTradeOpcode(begin);
            WorldPacket fee(CMSG_SET_TRADE_GOLD,4);fee<<uint32_t(60);customer->GetSession()->HandleSetTradeGoldOpcode(fee);
            f.windowOpened=true;return;
        }
        if(automaticOffer && (!actor->GetTradeData() || !actor->GetTradeData()->GetItem(TradeSlots(0))))return;
        UnsettledClaimBatch claims;if(!sLivingActivityCoordinator.ReadTaskClaims(f.actor,saved->id,saved->revision,claims,why))return;
        require(claims.complete && claims.claims.size()==1,"exact_crafted_output_claim_required");
        const auto& claim=claims.claims.front();auto* item=actor->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,claim.itemGuid));
        require(claim.location=="bags" && claim.itemEntry==2454 && claim.quantity==1 && item && item->GetCount()==1,
            "exact_native_crafted_stack_required");
        if(!automaticOffer) {
        require(!actor->GetTradeData() && !customer->GetTradeData(),"fresh_customer_trade_required");
        WorldPacket initiate(CMSG_INITIATE_TRADE,8);initiate<<customer->GetObjectGuid();
        actor->GetSession()->HandleInitiateTradeOpcode(initiate);
        require(actor->GetTradeData() && customer->GetTradeData(),"native_trade_window_required");
        WorldPacket begin;customer->GetSession()->HandleBeginTradeOpcode(begin);
        WorldPacket offered(CMSG_SET_TRADE_ITEM,3);offered<<uint8_t(0)<<item->GetBagSlot()<<item->GetSlot();
        actor->GetSession()->HandleSetTradeItemOpcode(offered);
        WorldPacket fee(CMSG_SET_TRADE_GOLD,4);fee<<uint32_t(60);customer->GetSession()->HandleSetTradeGoldOpcode(fee);
        }
        require(actor->GetTradeData()->GetItem(TradeSlots(0))==item && customer->GetTradeData()->GetMoney()==60,
            "native_claimed_offer_required");
        if(f.cancelFirstOffer && !f.cancelled) {
            const auto actorMoney=actor->GetMoney(),customerMoney=customer->GetMoney();
            const auto outputCount=actor->GetItemCount(2454,true),customerCount=customer->GetItemCount(2454,true);
            require(!actor->GetTradeData()->IsAccepted() && !customer->GetTradeData()->IsAccepted(),"cancel_unaccepted_window_only");
            WorldPacket cancel(CMSG_CANCEL_TRADE);customer->GetSession()->HandleCancelTradeOpcode(cancel);
            require(!actor->GetTradeData() && !customer->GetTradeData() && actor->GetMoney()==actorMoney &&
                customer->GetMoney()==customerMoney && actor->GetItemCount(2454,true)==outputCount &&
                customer->GetItemCount(2454,true)==customerCount && actor->GetItemByGuid(item->GetObjectGuid())==item,
                "cancel_window_preserves_native_goods_and_money");
            f.cancelled=true;f.cancelledAt=NowMs();f.windowOpened=false;
            f.input.put("cancelled_offer_revision",saved->revision);f.input.put("cancelled_offer_claim",claim.id);
            f.input.put("cancelled_offer_claim_revision",claim.revision);f.input.put("cancelled_offer_item",claim.itemGuid);
            f.input.put("cancelled_offer_actor_money",actorMoney);f.input.put("cancelled_offer_recipient_money",customerMoney);
            f.input.put("cancelled_offer_preserved",true);
            write("commission-trade-input-v1.json",f.input);write("commission-cancelled-offer-checkpoint.json",f.input);return;
        }
        // ONLY the customer's acceptance. The coordinator owns the crafter's
        // final native acceptance, operation journal and verified settlement.
        WorldPacket accept(CMSG_ACCEPT_TRADE,4);accept<<uint32_t(0);customer->GetSession()->HandleAcceptTradeOpcode(accept);
        require(customer->GetTradeData() && customer->GetTradeData()->IsAccepted() && !actor->GetTradeData()->IsAccepted(),
            "customer_only_native_acceptance_required");f.offered=true;
    }catch(const std::exception& error) {
        report.put("outcome","failed");report.put("failure",error.what());report.put("task",f.request.task.id);
        write("activity-commission-trade-"+state.boot+".json",report);state.fixtureFinished=true;
    }
}
}
