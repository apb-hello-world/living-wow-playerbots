// Isolated binary only. No fabricated craft/send receipts or money grants.
// Only missing herbs (one of each) may be supplied and are recorded separately.
// The recorded agreement is fixture consent, not a human request.
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
                f.setup=f.admitted=true;
            }
        }
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
            require(actor->GetItemCount(3371,false)>=1,"owned_native_commission_vial_required");
            boost::property_tree::ptree grants;
            for(uint32_t entry:{765u,2449u})if(!actor->GetItemCount(entry,true)) {
                ItemPosCountVec positions;
                require(actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)==EQUIP_ERR_OK,
                    "commission_fixture_herb_capacity_required");
                boost::property_tree::ptree row;row.put("entry",entry);row.put("quantity",1);grants.push_back({"",row});
            }
            CommissionJob job;job.agreement={"lwc-"+std::to_string(NowMs()),"isolated-commission-mail-v1","mail",
                EncodeProfessionJob(recipe),f.actor,f.recipient,60,NowMs()};job.craft=job.agreement.recipe;
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
            f.input.put("customer_money_before",customer->Fetch()[2].GetUInt32());
            f.input.put("item_grants",grants.size());f.input.add_child("material_grants",grants);
            f.input.put("earned_materials_proof",false);f.input.put("money_grants",0);f.input.put("skill_grants",0);
            f.input.put("recipe_grants",0);f.input.put("fixture_consent",true);f.input.put("ordinary_due_queue",true);
            f.input.add_child("inventory_before",GuildMailFixtureInventory(*actor));
            std::ofstream intent("/isolated/evidence/commission-mail-input-v1.json");intent<<Json(f.input);intent.close();
            require(bool(intent),"commission_fixture_intent_saved");
            for(const auto& row:grants) {
                const auto entry=row.second.template get<uint32_t>("entry");ItemPosCountVec positions;
                require(!actor->GetItemCount(entry,true) &&
                    actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)==EQUIP_ERR_OK,
                    "commission_fixture_input_changed");
                auto* herb=actor->StoreNewItem(positions,entry,true);
                require(herb && actor->GetItemCount(entry,false)==1,"commission_fixture_input_grant_failed");
            }
            actor->SaveToDB();
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

// Second boot, same accepted task. Emulate ONLY customer mailbox clicks and
// crafter payment collection. Native mail hooks supply the receipts; ordinary
// coordinator recovery/settlement must finish the order without another craft.
template<class FixtureState>
void RunIsolatedCommissionCollectionFixture(FixtureState& state) {
    struct Fixture {
        uint32_t actor=0,recipient=0;
        uint64_t deadline=0;
        bool senderLogin=false,customerLogin=false,positioned=false;
        bool collected=false,paid=false;
        std::string task,blocker;
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
        boost::property_tree::ptree report;report.put("suite","activity-commission-collect-v1");
        report.put("test_only",true);report.put("outcome",passed?"passed":"failed");report.put("failure",failure);
        report.put("boot",state.boot);report.put("task",f.task);report.put("actor",f.actor);report.put("recipient",f.recipient);
        report.put("blocker",f.blocker);report.put("same_task_restart",state.boot!=f.input.template get<std::string>("setup_boot",state.boot));
        report.put("fixture_customer_positioning",f.positioned);report.put("independent_audit_required",true);
        report.put("new_item_grants",0);report.put("money_grants",0);report.put("ordinary_receipt_settlement",true);
        if(sender)report.put("sender_money",sender->GetMoney());
        if(customer){report.put("customer_money",customer->GetMoney());report.put("customer_output",customer->GetItemCount(2454,true));}
        std::ofstream out("/isolated/evidence/activity-commission-collect-"+state.boot+".json");
        out<<Json(report);out.close();state.fixtureFinished=true;
        if(!out)sLog.outError("Living commission collection evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());
    };
    Player* sender=nullptr;Player* customer=nullptr;
    try {
        if(!f.actor) {
            f.input=read("/isolated/evidence/commission-mail-input-v1.json");
            f.task=f.input.template get<std::string>("task");const auto boot=f.input.template get<std::string>("setup_boot");
            require(IsUuid(f.task) && IsUuid(boot) && boot!=state.boot,"commission_sent_boot_restart_required");
            const auto sent=read("/isolated/evidence/commission-mail-sent-v1.json");
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
            if(!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() ||
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
        require(proof.quote.receiver==f.recipient && proof.quote.cod==60 && proof.returned.empty(),"original_unreturned_parcel_required");
        if(proof.state==CommissionDeliveryState::Complete) {
            if(saved->phase!=Phase::Completed){f.blocker="ordinary_commission_settlement_pending";return;}
            require(sender->GetMoney()==f.input.template get<uint32_t>("money_before")-30+60 &&
                customer->GetMoney()==f.input.template get<uint32_t>("customer_money_before")-60,"native_commission_money_conservation");
            emit(true,"",sender,customer);return;
        }
        const auto mailbox=NativeNearbyMailbox(*sender);
        if(!mailbox){f.blocker="commission_sender_mailbox_unavailable";return;}
        if(!f.positioned) {
            require(customer->GetMapId()==sender->GetMapId(),"same_map_copied_customer_required");
            customer->NearTeleportTo(sender->GetPositionX(),sender->GetPositionY(),sender->GetPositionZ(),sender->GetOrientation());
            f.positioned=true;return; // Explicit fixture positioning, not travel proof.
        }
        if(NativeNearbyMailbox(*customer)!=mailbox){f.blocker="commission_customer_mailbox_arrival_pending";return;}
        if(proof.received.empty()) {
            if(f.collected){f.blocker="native_customer_receipt_pending";return;}
            auto* mail=customer->GetMail(proof.mail);require(mail,"original_customer_mail_required");
            if(mail->deliver_time>time(nullptr)) {
                f.deadline=std::max(f.deadline,(uint64_t(mail->deliver_time)+300)*1000);
                f.blocker="native_parcel_delivery_time_pending";return;
            }
            require(mail->COD==60 && mail->items.size()==1 && mail->items.front().item_guid==proof.quote.item,
                "original_uncollected_native_parcel_required");
            intent("commission-customer-click-v1.json",*customer);f.collected=true;
            WorldPacket packet(CMSG_MAIL_TAKE_ITEM,16);packet<<ObjectGuid(mailbox)<<uint32(proof.mail)<<uint32(proof.quote.item);
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
