// Isolated native proof of the core trade boundary, NOT an accepted commission
// workflow. Uses one existing unclaimed stack and existing money, no grants.
#include "playerbot/LivingCommissionTrade.h"
namespace {
template<class FixtureState>
void RunIsolatedTradeCaptureFixture(FixtureState& state) {
    struct Fixture {
        uint32_t actor=0,recipient=0;
        bool actorLogin=false,recipientLogin=false,positioned=false;
        uint64_t deadline=0;
        boost::property_tree::ptree input;
    };
    static Fixture f;
    if(state.fixtureFinished || NowMs()<state.fixtureNext)return;
    state.fixtureNext=NowMs()+500;
    auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto write=[&](const std::string& path,const boost::property_tree::ptree& data) {
        std::ofstream out(path);out<<Json(data);out.close();require(bool(out),"trade_fixture_evidence_write_failed");
    };
    boost::property_tree::ptree report;
    report.put("suite","activity-trade-capture-v1");report.put("boot",state.boot);
    report.put("test_only",true);report.put("commission_workflow_proof",false);
    report.put("item_grants",0);report.put("money_grants",0);
    try {
        if(!f.actor) {
            auto id=[&](const char* name) {
                const auto* raw=std::getenv(name);const std::string text=raw?raw:"";
                require(!text.empty() && text.size()<=9 && text.find_first_not_of("0123456789")==std::string::npos,
                    "explicit_native_trade_actors_required");
                return uint32_t(std::stoul(text));
            };
            f.actor=id("LIVING_WOW_NATIVE_FIXTURE_ACTOR");f.recipient=id("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
            require(f.actor && f.recipient && f.actor!=f.recipient,"distinct_trade_actors_required");
            require(!std::ifstream("/isolated/evidence/native-trade-input-v1.json"),
                "existing_trade_intent_requires_audit_not_replay");
            state.gameplayFixtureActor.store(f.actor,std::memory_order_release);
            state.professionCohortActors[0].store(f.recipient,std::memory_order_release);
            f.deadline=NowMs()+600000;
        }
        if(sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending || !state.pending.empty() ||
            !state.incoming.empty() || !state.resources.Protection().ready)return;
        require(NowMs()<f.deadline,"native_trade_safety_or_login_deadline");
        auto* actor=sRandomPlayerbotMgr.GetPlayerBot(f.actor);
        auto* customer=sRandomPlayerbotMgr.GetPlayerBot(f.recipient);
        auto login=[&](Player* bot,uint32_t guid,bool& requested) {
            if(!bot && !requested && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                requested=true;sRandomPlayerbotMgr.AddRandomBot(guid);
            }
        };
        login(actor,f.actor,f.actorLogin);login(customer,f.recipient,f.recipientLogin);
        if(!actor || !customer)return;
        const auto protection=state.resources.Protection();
        for(auto* bot:{actor,customer}) {
            require(bot->GetPlayerbotAI() && !bot->GetPlayerbotAI()->IsRealPlayer() && bot->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()),"copied_bot_only_trade_required");
            if(!bot->IsInWorld() || !bot->GetMap() || NativeSafety(bot) || bot->GetGroup() || bot->GetTradeData() ||
                bot->GetMap()->IsDungeon() || LivingServiceExecution::Busy(bot) ||
                sLivingActivityCoordinator.DefersNativeSave(bot->GetGUIDLow()))return;
            require(!protection.actorClaims.count(bot->GetGUIDLow()),"existing_trade_actor_claims_preserved");
            for(const auto& row:state.cache)if(row.second.actor==bot->GetGUIDLow() && row.second.accepted &&
                !Terminal(row.second.phase))require(false,"existing_trade_actor_commitment_preserved");
        }
        require(actor->GetTeam()==customer->GetTeam() && actor->GetMapId()==customer->GetMapId(),
            "same_faction_and_map_trade_fixture_required");
        if(!f.positioned) {
            f.input.put("boot",state.boot);f.input.put("actor",f.actor);f.input.put("recipient",f.recipient);
            f.input.put("test_only",true);f.input.put("fixture_relocations",1);
            f.input.put("autonomous_travel_proof",false);
            f.input.put("recipient_original_map",customer->GetMapId());
            f.input.put("recipient_original_x",customer->GetPositionX());f.input.put("recipient_original_y",customer->GetPositionY());
            f.input.put("recipient_original_z",customer->GetPositionZ());f.input.put("recipient_original_o",customer->GetOrientation());
            write("/isolated/evidence/native-trade-positioning-"+state.boot+".json",f.input);
            customer->GetMotionMaster()->Clear();customer->StopMoving();actor->GetMotionMaster()->Clear();actor->StopMoving();
            customer->NearTeleportTo(actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ(),actor->GetOrientation());
            f.positioned=true;return;
        }
        require(actor->IsWithinDistInMap(customer,TRADE_DISTANCE,false),"native_trade_arrival_required");
        Item* item=nullptr;
        for(uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot) {
            auto* candidate=actor->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);
            if(candidate && candidate->GetEntry()==2454 && candidate->CanBeTraded() && !candidate->IsInTrade() &&
                !sGuildSupplies.Reserved(candidate->GetGUIDLow()) && !candidate->HasGeneratedLoot()) {item=candidate;break;}
        }
        require(item && item->GetCount() && item->GetCount()<=20,"existing_tradeable_potion_stack_required");
        require(customer->GetMoney()>=60 && uint64_t(actor->GetMoney())+60<=MAX_MONEY_AMOUNT,
            "existing_native_trade_fee_required");
        const CommissionTradeQuote quote{f.actor,f.recipient,2454,60,actor->GetMoney(),customer->GetMoney(),
            {{item->GetGUIDLow(),item->GetCount()}}};
        const auto actorCount=actor->GetItemCount(2454,true),customerCount=customer->GetItemCount(2454,true);
        f.input.put("item",item->GetGUIDLow());f.input.put("quantity",item->GetCount());f.input.put("entry",2454);
        f.input.put("fee",60);f.input.put("actor_money_before",quote.actorMoney);f.input.put("recipient_money_before",quote.recipientMoney);
        f.input.put("actor_items_before",actorCount);f.input.put("recipient_items_before",customerCount);
        f.input.put("item_grants",0);f.input.put("money_grants",0);
        write("/isolated/evidence/native-trade-input-v1.json",f.input);
        auto offer=[&]() {
            WorldPacket initiate(CMSG_INITIATE_TRADE,8);initiate<<customer->GetObjectGuid();
            actor->GetSession()->HandleInitiateTradeOpcode(initiate);
            require(actor->GetTradeData() && customer->GetTradeData(),"native_trade_open_required");
            WorldPacket begin;customer->GetSession()->HandleBeginTradeOpcode(begin);
            WorldPacket offered(CMSG_SET_TRADE_ITEM,3);
            offered<<uint8_t(0)<<item->GetBagSlot()<<item->GetSlot();
            actor->GetSession()->HandleSetTradeItemOpcode(offered);
            WorldPacket fee(CMSG_SET_TRADE_GOLD,4);fee<<uint32_t(60);customer->GetSession()->HandleSetTradeGoldOpcode(fee);
            require(actor->GetTradeData()->GetItem(TradeSlots(0))==item && customer->GetTradeData()->GetMoney()==60,
                "exact_native_trade_offer_required");
        };
        offer();
        {
            NativeTradeCapture cancelled;WorldPacket cancel;
            customer->GetSession()->HandleCancelTradeOpcode(cancel);
            require(!cancelled.Completed() && cancelled.Rows().empty() && !actor->GetTradeData() && !customer->GetTradeData() &&
                actor->GetMoney()==quote.actorMoney && customer->GetMoney()==quote.recipientMoney &&
                actor->GetItemCount(2454,true)==actorCount && customer->GetItemCount(2454,true)==customerCount,
                "cancelled_trade_must_not_transfer_or_prove_completion");
        }
        report.put("cancelled_trade_unchanged",true);offer();
        NativeTradeCapture capture;
        WorldPacket acceptCustomer(CMSG_ACCEPT_TRADE,4);acceptCustomer<<uint32_t(0);
        customer->GetSession()->HandleAcceptTradeOpcode(acceptCustomer);
        require(!capture.Completed() && actor->GetTradeData() && customer->GetTradeData(),"one_acceptance_not_completion");
        WorldPacket acceptActor(CMSG_ACCEPT_TRADE,4);acceptActor<<uint32_t(0);
        actor->GetSession()->HandleAcceptTradeOpcode(acceptActor);
        std::string why;require(VerifyCommissionTrade(quote,capture,why),why);
        require(!actor->GetTradeData() && !customer->GetTradeData() &&
            actor->GetItemCount(2454,true)+quote.items.front().quantity==actorCount &&
            customer->GetItemCount(2454,true)==uint64_t(customerCount)+quote.items.front().quantity,
            "actual_native_item_conservation_required");
        report.add_child("input",f.input);report.put("actor_money_after",actor->GetMoney());
        report.put("recipient_money_after",customer->GetMoney());report.put("actor_items_after",actor->GetItemCount(2454,true));
        report.put("recipient_items_after",customer->GetItemCount(2454,true));
        boost::property_tree::ptree destinations;
        for(const auto& transfer:capture.Rows())for(const auto& d:transfer.destinations) {
            boost::property_tree::ptree p;p.put("source",transfer.item);p.put("position",d.position);p.put("quantity",d.quantity);
            p.put("before_item",d.beforeItem);p.put("before_count",d.beforeCount);p.put("after_item",d.afterItem);p.put("after_count",d.afterCount);
            destinations.push_back({"",p});
        }
        report.add_child("destinations",destinations);report.put("independent_database_audit_required",true);
        report.put("outcome","passed");
    }catch(const std::exception& error){report.put("outcome","failed");report.put("failure",error.what());}
    write("/isolated/evidence/activity-trade-capture-"+state.boot+".json",report);
    state.fixtureFinished=true;sLog.outString("Living isolated trade capture: %s",Json(report).c_str());
}
}
