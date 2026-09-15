// Isolated binary only. No fabricated craft/send receipts or money grants.
// V1 supplies missing herbs only. V2 supplies a recorded bounded batch of
// reagents, never output, spells, skills or money, to cross a native stack edge.
// The recorded agreement is fixture consent, not a human request.
// Ordinary preparation, timed native crafting and mail own the actual work.
#include "playerbot/PlayerbotChatDirector.h"
namespace {
template<class FixtureState>
void RunIsolatedCommissionReplayFixture(FixtureState& state) {
    if(state.fixtureFinished || !state.loaded || state.ioPending || !state.pending.empty() || !state.incoming.empty() ||
        !state.resources.Protection().ready || sWorld.GetActiveAndQueuedSessionCount())return;
    boost::property_tree::ptree report;report.put("boot",state.boot);report.put("suite","activity-commission-replay-v1");
    report.put("test_only",true);report.put("independent_audit_required",true);
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    try {
        std::ifstream in("/isolated/evidence/commission-mail-input-v1.json",std::ios::ate);
        require(in.good() && in.tellg()>0 && in.tellg()<=1024*1024,"bounded_original_broker_input_required");
        in.seekg(0);boost::property_tree::ptree input;boost::property_tree::read_json(in,input);
        require(input.get<bool>("broker_case") && input.get<std::string>("setup_boot")!=state.boot,"original_broker_restart_required");
        const auto task=input.get<std::string>("task"),commission=input.get<std::string>("commission");
        require(!sLivingActivityCoordinator.ReadSavedTask(task),"terminal_order_must_be_outside_active_cache");
        state.enforceEffects.store(true,std::memory_order_release);
        bool readable=false;const auto saved=sLivingActivityCoordinator.ReadSavedCommission(commission,&readable);
        require(readable && saved && saved->id==task && (saved->phase==Phase::Completed ||
            (saved->phase==Phase::Failed && saved->checkpoint.blocker=="commission_parcel_returned_items_preserved")),
            "original_settled_order_required");
        report.put("terminal_phase",saved->phase==Phase::Completed?"completed":"failed");
        auto snapshot=[&]() {
            auto row=CharacterDatabase.PQuery("SELECT revision,(SELECT money FROM characters WHERE guid=%u),"
                "(SELECT money FROM characters WHERE guid=%u),(SELECT COUNT(*) FROM living_activity_operation WHERE task_id='%s') "
                "FROM living_activity_task WHERE task_id='%s'",input.get<uint32_t>("actor"),input.get<uint32_t>("recipient"),task.c_str(),task.c_str());
            require(bool(row),"native_replay_snapshot_required");boost::property_tree::ptree p;
            p.put("revision",row->Fetch()[0].GetUInt64());p.put("sender_money",row->Fetch()[1].GetUInt32());
            p.put("customer_money",row->Fetch()[2].GetUInt32());p.put("operations",row->Fetch()[3].GetUInt32());return p;
        };
        const auto before=snapshot();
        ChatDirectorEvent event;event.eventId=input.get<std::string>("broker_event");event.speakerGuid=input.get<uint32_t>("recipient");
        ChatDirectorActionProposal p;p.proposalId="accepted";p.botGuid=input.get<uint32_t>("actor");p.targetGuid=event.speakerGuid;
        p.type="craft_commission";p.capabilityRef="spell:craft:2329:2454";p.quantity=1;p.priceCopper=60;p.delivery="mail";
        const auto result=sPlayerbotActionBroker.Create(p,event);
        require(result.created && result.reasonCode=="commission_queued","completed_broker_replay_must_acknowledge_original");
        p.quantity=2;const auto conflict=sPlayerbotActionBroker.Create(p,event);
        require(!conflict.created && conflict.reasonCode=="commission_agreement_conflict","completed_broker_changed_order_must_reject");
        const auto after=snapshot();require(Json(before)==Json(after),"replay_cannot_mutate_native_order_money_or_operations");
        require(!sLivingActivityCoordinator.ReadSavedTask(task),"replay_cannot_restore_execution");
        report.put("task",task);report.put("commission",commission);report.put("outcome","passed");
        report.put("new_grants",0);report.put("new_native_operations",0);report.put("execution_restored",false);
        report.add_child("before",before);report.add_child("after",after);
    } catch(const std::exception& e){report.put("outcome","failed");report.put("failure",e.what());}
    std::ofstream out("/isolated/evidence/commission-broker-replay-"+state.boot+".json");out<<Json(report);out.close();
    state.fixtureFinished=true;sLog.outString("Living isolated broker replay: %s",Json(report).c_str());
}

template<class FixtureState>
void RunIsolatedCommissionMailFixture(FixtureState& state,bool parcelCase=false,bool capacityCase=false,bool brokerCase=false) {
    struct Fixture {
        uint32_t actor=0,recipient=0,money=0,output=0,quantity=1;
        uint64_t deadline=0,nextDiagnostic=0;
        bool login=false,setup=false,admitted=false,capacityFilled=false;
        bool customerLogin=false,brokerAccepted=false;
        ChatDirectorEvent brokerEvent;
        ChatDirectorActionProposal brokerProposal;
        TaskRequest request;
        std::string blocker;
        boost::property_tree::ptree input;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+(capacityCase?0:500);
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto emit=[&](bool sent,const std::string& why,Player* actor) {
        auto report=f.input;
        report.put("suite",capacityCase?"activity-commission-capacity-v1":parcelCase?"activity-commission-parcel-v2":"activity-commission-mail-v1");report.put("boot",state.boot);
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
        report.put("verification_boot",state.boot);
        std::ofstream out("/isolated/evidence/activity-commission-mail-"+state.boot+".json");
        out<<Json(report);out.close();state.fixtureFinished=true;
        if(!out)sLog.outError("Living commission fixture evidence write failed");
        if(sent && out) {
            const char* path="/isolated/evidence/commission-mail-sent-v1.json";
            if(!std::ifstream(path)) {std::ofstream checkpoint(path);checkpoint<<Json(report);}
        }
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
            std::ifstream prior("/isolated/evidence/commission-mail-input-v1.json",std::ios::ate);
            if(prior) {
                require(prior.tellg()>0 && prior.tellg()<=1024*1024,"bounded_commission_input_required");
                prior.seekg(0);boost::property_tree::ptree input;boost::property_tree::read_json(prior,input);
                require(input.template get<std::string>("setup_boot")!=state.boot &&
                    input.template get<uint32_t>("actor")==f.actor && input.template get<uint32_t>("recipient")==f.recipient &&
                    IsUuid(input.template get<std::string>("task")),"original_commission_recovery_actors_required");
                // Resume the accepted obligation, including an uncertain send.
                // No new consent row, task, material grants or native clicks.
                f.input=input;f.request.task.id=input.template get<std::string>("task");
                f.money=input.template get<uint32_t>("money_before");f.output=input.template get<uint32_t>("output_before");
                f.quantity=input.template get<uint32_t>("requested_quantity",1);
                f.setup=f.admitted=true;
            }
        }
        if(!f.setup) {
            require(!brokerCase || !std::ifstream("/isolated/evidence/commission-broker-proposal-v1.json"),
                "broker_setup_interrupted_requires_reconciliation_not_new_grants");
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
            if(parcelCase) {
                const auto* proto=sObjectMgr.GetItemPrototype(2454);
                require(proto && proto->GetMaxStackSize()>1 && proto->GetMaxStackSize()<=20,
                    "bounded_native_potion_stack_required");
                f.quantity=proto->GetMaxStackSize();
                require(actor->GetItemCount(2454,false)>0 && actor->GetItemCount(2454,false)<f.quantity &&
                    actor->GetItemCount(2454,true)==actor->GetItemCount(2454,false) && actor->GetMoney()>=60,
                    "owned_partial_output_stack_required_no_output_grants");
            }
            ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=actor->GetSkillValuePure(171);
            recipe.outputEntry=2454;recipe.outputQuantity=f.quantity;recipe.purpose=ProfessionPurpose::RequestedItem;
            recipe.attemptLimit=std::max(recipe.attemptLimit,f.quantity);
            recipe.reagents={{765,1},{2449,1},{3371,1}};
            require(parcelCase || capacityCase || actor->GetItemCount(3371,false)>=1,"owned_native_commission_vial_required");
            if(capacityCase)require(actor->GetItemCount(2454,false)>0 && actor->GetItemCount(2454,false)<20,
                "capacity_fixture_owned_output_surplus_required");
            boost::property_tree::ptree grants;
            for(uint32_t entry:{765u,2449u,3371u})if((parcelCase || capacityCase || entry!=3371) && actor->GetItemCount(entry,true)<f.quantity) {
                const auto owned=actor->GetItemCount(entry,true),missing=f.quantity-owned;
                ItemPosCountVec positions;
                require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,missing)==EQUIP_ERR_OK,
                    "commission_fixture_herb_capacity_required");
                boost::property_tree::ptree row;row.put("entry",entry);row.put("quantity",missing);
                if(parcelCase || capacityCase)row.put("owned_before",owned);grants.push_back({"",row});
            }
            CommissionJob job;job.agreement={"lwc-"+std::to_string(NowMs()),"isolated-commission-mail-v1","mail",
                EncodeProfessionJob(recipe),f.actor,f.recipient,60,NowMs()};job.craft=job.agreement.recipe;
            job.agreement.transaction="isolated-mail-"+job.agreement.id;
            if(brokerCase) {
                require(BuildNativeRequestedItemJob(*actor,2329,2454,1,recipe,f.blocker),"native_broker_recipe_required");
                f.brokerEvent.eventId="isolated-mail-"+std::to_string(NowMs());
                f.brokerEvent.speakerGuid=f.recipient;
                auto& p=f.brokerProposal;p.proposalId="accepted";p.botGuid=f.actor;p.targetGuid=f.recipient;
                p.type="craft_commission";p.capabilityRef="spell:craft:2329:2454";
                p.delivery="mail";p.quantity=1;p.priceCopper=60;
                auto& candidate=f.brokerEvent.candidates[f.actor];candidate.guid=f.actor;
                ChatDirectorCapability capability;capability.capabilityRef=p.capabilityRef;capability.type=p.type;
                capability.itemId=2454;capability.minQuantity=capability.maxQuantity=capability.quantity=1;
                capability.priceCopper=60;capability.deliveries={"mail"};candidate.actionCapabilities.push_back(capability);
                job.agreement.transaction="wow-tx-"+f.brokerEvent.eventId+'-'+p.proposalId;
                job.agreement.id="lwc-"+std::to_string(std::hash<std::string>{}(job.agreement.transaction));
                job.agreement.recipe=job.craft=EncodeProfessionJob(recipe);
            }
            auto& task=f.request.task;task.source="commission_job";task.sourceKey=job.agreement.id;
            task.id=task.root=SourceId(task.source,task.sourceKey);task.actor=f.actor;task.kind=Kind::Commission;
            task.mode=Mode::Active;task.phase=Phase::Queued;task.accepted=true;task.priority=Priority::Human;
            task.context=ReadNativeContext(*actor,state.policyRevision,state.boot);task.createdAtMs=task.updatedAtMs=NowMs();
            task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeCommissionJob(job);f.request.receipt=NewId();
            require(ValidateNativeProfessionTask(*actor,task,f.blocker),"native_commission_recipe:"+f.blocker);
            f.money=actor->GetMoney();f.output=actor->GetItemCount(2454,true);
            f.input.put("setup_boot",state.boot);f.input.put("task",task.id);f.input.put("commission",job.agreement.id);f.input.put("actor",f.actor);
            f.input.put("recipient",f.recipient);f.input.put("agreement",EncodeCommissionContract(job.agreement));
            f.input.put("money_before",f.money);f.input.put("output_before",f.output);
            if(parcelCase){f.input.put("requested_quantity",f.quantity);f.input.put("output_grants",0);}
            if(capacityCase){f.input.put("capacity_case",true);f.input.put("output_grants",0);}
            f.input.put("customer_money_before",customer->Fetch()[2].GetUInt32());
            f.input.put("item_grants",grants.size());f.input.add_child("material_grants",grants);
            f.input.put("earned_materials_proof",false);f.input.put("money_grants",0);f.input.put("skill_grants",0);
            f.input.put("recipe_grants",0);f.input.put("fixture_consent",true);f.input.put("ordinary_due_queue",true);
            if(brokerCase){f.input.put("broker_case",true);f.input.put("broker_event",f.brokerEvent.eventId);}
            f.input.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            std::ofstream intent(brokerCase?"/isolated/evidence/commission-broker-proposal-v1.json":
                "/isolated/evidence/commission-mail-input-v1.json");intent<<Json(f.input);intent.close();
            require(bool(intent),"commission_fixture_intent_saved");
            for(const auto& row:grants) {
                const auto entry=row.second.template get<uint32_t>("entry"),quantity=row.second.template get<uint32_t>("quantity");
                const auto owned=row.second.template get<uint32_t>("owned_before",0);ItemPosCountVec positions;
                require(actor->GetItemCount(entry,true)==owned &&
                    actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,quantity)==EQUIP_ERR_OK,
                    "commission_fixture_input_changed");
                auto* herb=actor->StoreNewItem(positions,entry,true);
                require(herb && actor->GetItemCount(entry,true)==owned+quantity,"commission_fixture_input_grant_failed");
            }
            actor->SaveToDB();
            // Use the same atomic contract/task admission as a validated player
            // request. The fixture no longer pre-inserts its own acceptance row.
            f.setup=true;return;
        }
        if(!f.admitted) {
            CommissionJob accepted;std::string why;
            require(DecodeCommissionJob(f.request.task.checkpoint.data,accepted,why),"fixture_original_agreement_required");
            if(brokerCase) {
                auto* customer=sRandomPlayerbotMgr.GetPlayerBot(f.recipient);
                if(!customer) {
                    if(!f.customerLogin){f.customerLogin=true;sRandomPlayerbotMgr.AddRandomBot(f.recipient);}
                    f.blocker="broker_customer_login_pending";return;
                }
                require(customer->GetPlayerbotAI() && !customer->GetPlayerbotAI()->IsRealPlayer() && customer->GetSession() &&
                    sPlayerbotAIConfig.IsInRandomAccountList(customer->GetSession()->GetAccountId()),"native_bot_customer_required");
                if(!customer->IsInWorld() || NativeSafety(customer) || customer->GetGroup() || customer->GetTradeData()) {
                    f.blocker="broker_customer_safety_wait";return;
                }
                auto invalid=f.brokerProposal;invalid.priceCopper=61;
                const auto rejected=sPlayerbotActionBroker.Create(invalid,f.brokerEvent);
                require(!rejected.created && rejected.reasonCode=="craft_fee_changed","native_broker_changed_fee_must_reject");
                const auto result=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
                require(result.created && result.reasonCode=="commission_queued","native_broker_admission:"+result.reasonCode);
                const auto duplicate=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
                require(duplicate.created && duplicate.reasonCode=="commission_queued","native_broker_pending_replay_required");
            } else {
                const auto result=sLivingActivityCoordinator.SubmitMailCommission(accepted.agreement);f.blocker=result.blocker;
                require(result.code==AdmissionCode::Pending,"commission_fixture_admission:"+f.blocker);
            }
            f.admitted=true;return;
        }
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.request.task.id);
        if(!saved){f.blocker="commission_fixture_admission_pending";return;}
        if(brokerCase && !f.brokerAccepted && !std::ifstream("/isolated/evidence/commission-mail-input-v1.json")) {
            CommissionJob actual,expected;std::string why;
            require(DecodeCommissionJob(saved->checkpoint.data,actual,why) &&
                DecodeCommissionJob(f.request.task.checkpoint.data,expected,why),"native_broker_accepted_agreement_required");
            expected.agreement.acceptedAtMs=actual.agreement.acceptedAtMs;
            require(EncodeCommissionContract(expected.agreement)==EncodeCommissionContract(actual.agreement),
                "native_broker_cannot_change_fixture_consent");
            f.input.put("agreement",EncodeCommissionContract(actual.agreement));
            std::ofstream out("/isolated/evidence/commission-mail-input-v1.json");out<<Json(f.input);out.close();
            require(bool(out),"native_broker_acceptance_evidence_required");f.brokerAccepted=true;
        }
        if(capacityCase && !f.capacityFilled) {
            CommissionJob job;std::string why;
            require(DecodeCommissionJob(saved->checkpoint.data,job,why),"capacity_fixture_commission_required");
            if(job.craftFinishedRevision) {
                require(!sLivingActivityCoordinator.DefersNativeSave(f.actor) && saved->phase!=Phase::Executing,
                    "capacity_fixture_must_precede_parcel_execution");
                FillIsolatedCommissionCapacity(*actor,*saved);f.capacityFilled=true;return;
            }
        }
        const auto blocker=state.executionBlockers.find(saved->id);
        f.blocker=blocker==state.executionBlockers.end()?saved->checkpoint.blocker:blocker->second;
        if(NowMs()>=f.nextDiagnostic) {
            f.nextDiagnostic=NowMs()+30000;
            boost::property_tree::ptree report;report.put("boot",state.boot);report.put("task",saved->id);
            report.put("blocker",f.blocker);report.put("safety",NativeSafety(actor));
            report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));report.put("gameplay_proof",false);
            report.put("map",actor->GetMapId());report.put("x",actor->GetPositionX());report.put("y",actor->GetPositionY());
            report.put("z",actor->GetPositionZ());report.put("at_ms",NowMs());
            std::ofstream out("/isolated/evidence/commission-mail-progress-"+state.boot+".json");out<<Json(report);
        }
        require(!Terminal(saved->phase),"commission_fixture_premature_terminal_state");
        ProfessionHistory history;CommissionDeliveryProof proof;
        if(!sLivingActivityCoordinator.ReadProfessionHistory(f.actor,saved->id,saved->revision,history,f.blocker) ||
            !InspectCommissionDelivery(*saved,history,proof,f.blocker))return;
        if(proof.send.empty())return;
        if(brokerCase && f.brokerAccepted) {
            // Replayed after actual material consumption, not merely queued.
            const auto duplicate=sPlayerbotActionBroker.Create(f.brokerProposal,f.brokerEvent);
            require(duplicate.created && duplicate.reasonCode=="commission_queued","native_broker_postsend_replay_required");
            auto changed=f.brokerProposal;changed.quantity=2;
            const auto rejected=sPlayerbotActionBroker.Create(changed,f.brokerEvent);
            require(!rejected.created && rejected.reasonCode=="commission_agreement_conflict","native_broker_replay_conflict_required");
            f.input.put("broker_postsend_replay_verified",true);
        }
        require(!sLivingActivityCoordinator.DefersNativeSave(f.actor),"commission_fixture_native_save_pending");
        require(!parcelCase || proof.quote.additional.size()==1,"two_native_attachments_required");
        const auto proceeds=IsolatedCommissionCapacityProceeds(*saved,capacityCase);
        if(proceeds)f.input.put("capacity_sale_proceeds",proceeds);
        require(actor->GetMoney()==uint64_t(f.money)+proceeds-proof.quote.postage && actor->GetItemCount(2454,true)==f.output,
            "commission_fixture_native_stock_or_postage_mismatch");
        f.input.put("send_operation",proof.send);f.input.put("mail",proof.mail);
        f.input.put("sent_item",proof.quote.item);f.input.put("native_send_verified",true);
        if(parcelCase){f.input.put("sent_quantity",CommissionMailQuantity(proof.quote));f.input.put("postage",proof.quote.postage);}
        emit(true,"",actor);
    } catch(const std::exception& error){emit(false,error.what(),actor);}
}

// Second boot, same accepted task. Emulate ONLY customer mailbox clicks and
// crafter payment collection. Native mail hooks supply the receipts; ordinary
// coordinator recovery/settlement must finish the order without another craft.
template<class FixtureState>
void RunIsolatedCommissionCollectionFixture(FixtureState& state,bool returnCase=false) {
    struct Fixture {
        uint32_t actor=0,recipient=0;
        uint64_t deadline=0,nextDiagnostic=0;
        uint32_t capacityProceeds=0;
        bool senderLogin=false,customerLogin=false,positioned=false,customerRevived=false;
        bool paid=false;
        std::set<uint32_t> collected;
        std::string task,blocker,lastDiagnostic;
        boost::property_tree::ptree input;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto read=[&](const std::string& path) {
        std::ifstream in(path,std::ios::ate);
        require(in.good() && in.tellg()>0 && in.tellg()<=1024*1024,"bounded_existing_commission_evidence_required");
        in.seekg(0);boost::property_tree::ptree result;boost::property_tree::read_json(in,result);return result;
    };
    auto intent=[&](const char* name,Player& actor) {
        const auto path=std::string("/isolated/evidence/")+name;
        require(!std::ifstream(path),"uncertain_native_mail_click_must_be_reconciled_not_repeated");
        boost::property_tree::ptree report;report.put("task",f.task);report.put("actor",actor.GetGUIDLow());
        report.put("boot",state.boot);report.put("money_before",actor.GetMoney());
        report.put("output_before",actor.GetItemCount(2454,true));
        report.add_child("inventory_before",GuildMailFixtureInventory(actor));
        std::ofstream out(path);out<<Json(report);out.close();require(bool(out),"native_mail_click_intent_saved");
    };
    auto emit=[&](bool passed,const std::string& failure,Player* sender,Player* customer) {
        boost::property_tree::ptree report;report.put("suite",returnCase?"activity-commission-return-v1":"activity-commission-collect-v1");
        report.put("test_only",true);report.put("outcome",passed?"passed":"failed");report.put("failure",failure);
        report.put("boot",state.boot);report.put("task",f.task);report.put("actor",f.actor);report.put("recipient",f.recipient);
        report.put("blocker",f.blocker);report.put("same_task_restart",state.boot!=f.input.template get<std::string>("setup_boot",state.boot));
        report.put("fixture_customer_positioning",f.positioned);report.put("independent_audit_required",true);
        report.put("fixture_sender_positioning",f.positioned);report.put("fixture_customer_resurrection",f.customerRevived);
        report.put("autonomous_recovery_or_travel_proof",false);
        report.put("new_item_grants",0);report.put("money_grants",0);report.put("ordinary_receipt_settlement",true);
        if(sender)report.put("sender_money",sender->GetMoney());
        if(customer){report.put("customer_money",customer->GetMoney());report.put("customer_output",customer->GetItemCount(2454,true));}
        std::ofstream out(std::string("/isolated/evidence/")+(returnCase?"activity-commission-return-":"activity-commission-collect-")+state.boot+".json");
        out<<Json(report);out.close();state.fixtureFinished=true;
        if(!out)sLog.outError("Living commission collection evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* sender=nullptr;Player* customer=nullptr;
    struct ProgressOnReturn {
        std::function<void()> report;
        ~ProgressOnReturn(){try{report();}catch(const std::exception&) {}}
    } progress{[&] {
        if(state.fixtureFinished || (f.blocker==f.lastDiagnostic && NowMs()<f.nextDiagnostic))return;
        f.lastDiagnostic=f.blocker;f.nextDiagnostic=NowMs()+30000;
        boost::property_tree::ptree report;report.put("boot",state.boot);report.put("task",f.task);
        report.put("blocker",f.blocker);report.put("at_ms",NowMs());report.put("gameplay_proof",false);
        report.put("journal_pending",state.pending.size());report.put("database_pending",state.ioPending);
        if(f.actor)report.put("activity_json",sLivingActivityCoordinator.ActorJson(f.actor));
        auto actor=[&](const char* name,Player* player) {
            boost::property_tree::ptree p;p.put("loaded",bool(player));
            if(player) {
                p.put("guid",player->GetGUIDLow());p.put("in_world",player->IsInWorld());
                p.put("safety",NativeSafety(player));p.put("stopped",player->IsStopped());
                p.put("grouped",bool(player->GetGroup()));p.put("trade",bool(player->GetTradeData()));
                p.put("service_busy",LivingServiceExecution::Busy(player));p.put("mailbox",NativeNearbyMailbox(*player));
                p.put("map",player->GetMapId());p.put("x",player->GetPositionX());p.put("y",player->GetPositionY());
                p.put("z",player->GetPositionZ());
            }
            report.add_child(name,p);
        };
        actor("sender",sender);actor("customer",customer);
        std::ofstream out("/isolated/evidence/commission-collection-progress-"+state.boot+".json");out<<Json(report);
    }};
    try {
        if(!f.actor) {
            f.input=read("/isolated/evidence/commission-mail-input-v1.json");
            f.task=f.input.template get<std::string>("task");const auto boot=f.input.template get<std::string>("setup_boot");
            require(IsUuid(f.task) && IsUuid(boot) && boot!=state.boot,"commission_sent_boot_restart_required");
            const auto sent=read("/isolated/evidence/commission-mail-sent-v1.json");
            f.capacityProceeds=sent.template get<uint32_t>("capacity_sale_proceeds",0);
            require(sent.template get<std::string>("task")==f.task && sent.template get<std::string>("outcome")=="sent_checkpoint",
                "verified_sent_checkpoint_required");
            require(IsUuid(sent.template get<std::string>("verification_boot")) &&
                sent.template get<std::string>("verification_boot")!=state.boot,"sent_checkpoint_restart_required");
            f.actor=f.input.template get<uint32_t>("actor");f.recipient=f.input.template get<uint32_t>("recipient");
            require(f.actor && f.recipient && f.actor!=f.recipient,"original_native_participants_required");
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            state.professionCohortActors[0].store(f.recipient,std::memory_order_release);
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        if(!f.deadline)f.deadline=NowMs()+900000;
        sender=sRandomPlayerbotMgr.GetPlayerBot(f.actor);customer=sRandomPlayerbotMgr.GetPlayerBot(f.recipient);
        if(NowMs()>f.deadline){emit(false,"commission_collection_deadline_task_preserved",sender,customer);return;}
        auto login=[&](Player* actor,uint32_t guid,bool& requested) {
            if(!actor && !requested && sRandomPlayerbotMgr.GetAllBots().size()<49){requested=true;sRandomPlayerbotMgr.AddRandomBot(guid);}
        };
        login(sender,f.actor,f.senderLogin);login(customer,f.recipient,f.customerLogin);
        if(!sender || !customer){f.blocker="commission_collection_native_login_pending";return;}
        for(auto* actor:{sender,customer}) {
            require(actor->GetPlayerbotAI() && !actor->GetPlayerbotAI()->IsRealPlayer() && actor->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"native_fixture_bot_required");
            // The replay holds customer AI, so a copied dead customer cannot
            // perform autonomous recovery. Explicit setup below is not proof
            // of death recovery. Never clear combat/transport/transfer safety.
            const auto allowedSetup=actor==customer && !f.positioned ? uint32_t(Safety::Death) : 0u;
            if(!actor->IsInWorld() || !actor->GetMap() || (NativeSafety(actor)&~allowedSetup) || actor->GetGroup() ||
                actor->GetTradeData() || actor->GetMap()->IsDungeon()){f.blocker="commission_collection_safety_wait";return;}
        }
        for(const auto& row:state.cache)if(row.second.actor==f.recipient && row.second.mode==Mode::Active && !Terminal(row.second.phase))
            require(false,"customer_existing_obligation_preserved");
        state.enforceEffects.store(true,std::memory_order_release);
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(f.task);
        if(!saved){f.blocker="commission_collection_restored_task_pending";return;}
        require(saved->actor==f.actor && IsCommissionJob(*saved),"original_commission_task_required");
        ProfessionHistory history;CommissionDeliveryProof proof;
        if(!sLivingActivityCoordinator.ReadProfessionHistory(f.actor,f.task,saved->revision,history,f.blocker) ||
            !InspectCommissionDelivery(*saved,history,proof,f.blocker))return;
        require(proof.quote.receiver==f.recipient && proof.quote.cod==60 && (returnCase || proof.returned.empty()),"original_unreturned_parcel_required");
        if(returnCase && !proof.returned.empty()) {
            require(proof.received.empty() && proof.fee.empty(),"returned_unpaid_order_cannot_have_customer_or_payment_receipt");
            if(saved->phase!=Phase::Failed){f.blocker="ordinary_return_collection_and_closure_pending";return;}
            require(sender->GetMoney()==uint64_t(f.input.template get<uint32_t>("money_before"))+f.capacityProceeds-proof.quote.postage &&
                customer->GetMoney()==f.input.template get<uint32_t>("customer_money_before"),"returned_order_money_conservation");
            require(sender->GetItemCount(2454,true)==f.input.template get<uint32_t>("output_before")+1,
                "returned_crafted_item_must_be_in_sender_inventory");
            emit(true,"",sender,customer);return;
        }
        if(proof.state==CommissionDeliveryState::Complete) {
            if(saved->phase!=Phase::Completed){f.blocker="ordinary_commission_settlement_pending";return;}
            require(sender->GetMoney()==uint64_t(f.input.template get<uint32_t>("money_before"))+f.capacityProceeds-proof.quote.postage+60 &&
                customer->GetMoney()==f.input.template get<uint32_t>("customer_money_before")-60,"native_commission_money_conservation");
            emit(true,"",sender,customer);return;
        }
        if(!f.positioned) {
            require(customer->GetMapId()==sender->GetMapId(),"same_map_copied_customer_required");
            const ObjectGuid originalMailbox(proof.quote.mailbox);
            require(originalMailbox.IsGameObject(),"original_native_mailbox_guid_required");
            const auto* spawn=sObjectMgr.GetGOData(originalMailbox.GetCounter());
            const auto* info=spawn?sObjectMgr.GetGameObjectInfo(spawn->id):nullptr;
            require(spawn && spawn->mapid==sender->GetMapId() && info && info->type==GAMEOBJECT_TYPE_MAILBOX,
                "original_same_map_mailbox_spawn_required");
            require(!LivingServiceExecution::Busy(sender) && !LivingServiceExecution::Busy(customer) &&
                !sLivingActivityCoordinator.DefersNativeSave(f.actor) && !sLivingActivityCoordinator.DefersNativeSave(f.recipient),
                "fixture_setup_native_operation_must_finish");
            const auto path="/isolated/evidence/commission-collection-setup-"+state.boot+".json";
            require(!std::ifstream(path),"fixture_setup_intent_must_not_repeat");
            boost::property_tree::ptree setup;setup.put("task",f.task);setup.put("boot",state.boot);
            setup.put("original_mailbox",proof.quote.mailbox);setup.put("test_only",true);
            setup.put("sender_positioning",true);setup.put("customer_positioning",true);
            setup.put("customer_resurrection",!customer->IsAlive());setup.put("autonomous_recovery_or_travel_proof",false);
            setup.put("sender_money",sender->GetMoney());setup.put("customer_money",customer->GetMoney());
            std::ofstream out(path);out<<Json(setup);out.close();require(bool(out),"fixture_setup_intent_saved");
            f.customerRevived=!customer->IsAlive();
            if(f.customerRevived){customer->ResurrectPlayer(1.0f,false);customer->SpawnCorpseBones();}
            for(auto* actor:{sender,customer}) {
                actor->GetMotionMaster()->Clear();actor->StopMoving();
                actor->NearTeleportTo(spawn->posX,spawn->posY,spawn->posZ,spawn->orientation);
            }
            f.positioned=true;f.blocker="fixture_mailbox_positioning_pending";return;
        }
        const auto mailbox=NativeNearbyMailbox(*sender);
        if(!mailbox){f.blocker="commission_sender_mailbox_unavailable";return;}
        if(NativeNearbyMailbox(*customer)!=mailbox){f.blocker="commission_customer_mailbox_arrival_pending";return;}
        if(proof.received.empty()) {
            const auto parts=CommissionMailAttachments(proof.quote);
            const auto part=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return std::none_of(
                proof.receivedAttachments.begin(),proof.receivedAttachments.end(),[&](const auto& delivered){return delivered.item==p.item;});});
            require(part!=parts.end(),"missing_attachment_receipt_required");
            if(f.collected.count(part->item)){f.blocker="native_customer_receipt_pending";return;}
            auto* mail=customer->GetMail(proof.mail);require(mail,"original_customer_mail_required");
            if(mail->deliver_time>time(nullptr)) {
                f.deadline=std::max(f.deadline,(uint64_t(mail->deliver_time)+300)*1000);
                f.blocker="native_parcel_delivery_time_pending";return;
            }
            require(mail->COD==(proof.receivedAttachments.empty()?60u:0u) &&
                mail->items.size()==parts.size()-proof.receivedAttachments.size() &&
                std::any_of(mail->items.begin(),mail->items.end(),[&](const auto& i){return i.item_guid==part->item;}),
                "original_uncollected_native_parcel_required");
            if(returnCase) {
                require(parts.size()==1 && proof.receivedAttachments.empty(),"single_unpaid_return_fixture_required");
                intent("commission-return-click-v1.json",*customer);f.collected.insert(part->item);
                WorldPacket packet(CMSG_MAIL_RETURN_TO_SENDER,20);
                packet<<ObjectGuid(mailbox)<<uint32(proof.mail)<<sender->GetObjectGuid();
                customer->GetSession()->HandleMailReturnToSender(packet);return;
            }
            const auto click=proof.quote.additional.empty()?std::string("commission-customer-click-v1.json"):
                "commission-customer-click-"+std::to_string(part->item)+"-v2.json";
            intent(click.c_str(),*customer);f.collected.insert(part->item);
            WorldPacket packet(CMSG_MAIL_TAKE_ITEM,16);packet<<ObjectGuid(mailbox)<<uint32(proof.mail)<<uint32(part->item);
            customer->GetSession()->HandleMailTakeItem(packet);return;
        }
        if(proof.fee.empty()) {
            if(f.paid){f.blocker="native_fee_receipt_pending";return;}
            auto* mail=sender->GetMail(proof.paymentMail);require(mail && mail->money==60,"native_cod_payment_required");
            if(mail->deliver_time>time(nullptr)) {
                f.deadline=std::max(f.deadline,(uint64_t(mail->deliver_time)+300)*1000);
                f.blocker="native_cod_delivery_time_pending";return;
            }
            intent("commission-payment-click-v1.json",*sender);f.paid=true;
            WorldPacket packet(CMSG_MAIL_TAKE_MONEY,12);packet<<ObjectGuid(mailbox)<<uint32(proof.paymentMail);
            sender->GetSession()->HandleMailTakeMoney(packet);return;
        }
    } catch(const std::exception& error){emit(false,error.what(),sender,customer);}
}
}
