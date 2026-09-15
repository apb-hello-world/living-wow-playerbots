// Isolated supplied capacity pressure. This never calls a vendor, bank, mail,
// settlement or money mutation. Ordinary commission preparation must recover.
#include "playerbot/LivingNativeParcelSlot.h"
namespace {
void FillIsolatedCommissionCapacity(Player& actor,const Task& task) {
    const char* done="/isolated/evidence/commission-capacity-filled-v1.json";
    if(std::ifstream existing{done};existing) {
        boost::property_tree::ptree prior;boost::property_tree::read_json(existing,prior);
        if(prior.get<std::string>("task")!=task.id || prior.get<uint32_t>("actor")!=actor.GetGUIDLow())
            throw std::runtime_error("capacity_fixture_replay_identity_mismatch");
        return; // Never grant again across a replay.
    }
    const char* intent="/isolated/evidence/commission-capacity-intent-v1.json";
    if(std::ifstream(intent))throw std::runtime_error("capacity_fixture_interrupted_grants_require_audit");
    boost::property_tree::ptree record;record.put("task",task.id);record.put("actor",actor.GetGUIDLow());
    record.put("test_only",true);record.put("money_before",actor.GetMoney());
    record.add_child("inventory_before",GuildMailFixtureInventory(actor));
    {std::ofstream out(intent);out<<Json(record);out.close();if(!out)throw std::runtime_error("capacity_fixture_intent_failed");}
    const auto* proto=sObjectMgr.GetItemPrototype(7074);
    if(!proto || !proto->SellPrice || proto->Quality!=ITEM_QUALITY_POOR)throw std::runtime_error("capacity_fixture_disposable_item_required");
    boost::property_tree::ptree grants;
    auto fill=[&](uint8_t bag,uint8_t slot) {
        if(actor.GetItemByPos(bag,slot))return;
        ItemPosCountVec positions;
        if(actor.CanStoreNewItem(bag,slot,positions,7074,1)!=EQUIP_ERR_OK)return;
        if(positions.size()!=1 || positions.front().pos!=uint16_t(uint16_t(bag)<<8|slot))
            throw std::runtime_error("capacity_fixture_exact_empty_slot_required");
        if(grants.size()>=128)throw std::runtime_error("capacity_fixture_bounded_fill_failed");
        auto* item=actor.StoreNewItem(positions,7074,true);
        if(!item || item->GetCount()!=1)throw std::runtime_error("capacity_fixture_bounded_fill_failed");
        boost::property_tree::ptree g;g.put("guid",item->GetGUIDLow());g.put("entry",7074);g.put("quantity",1);
        g.put("bag",bag);g.put("slot",slot);grants.push_back({"",g});
    };
    for(uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)fill(INVENTORY_SLOT_BAG_0,slot);
    for(uint8_t bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag)
        if(auto* container=static_cast<Bag*>(actor.GetItemByPos(INVENTORY_SLOT_BAG_0,bag)))
            for(uint8_t slot=0;slot<container->GetBagSize();++slot)fill(bag,slot);
    Item* potion=nullptr;
    for(const auto& stack:GuildMailFixtureInventory(actor)) {
        const auto guid=stack.second.get<uint32_t>("guid");
        if(stack.second.get<uint32_t>("entry")==2454 && stack.second.get<uint32_t>("quantity")>1)
            potion=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,guid));
    }
    uint16_t destination=0;
    if(grants.empty() || !potion || EmptyNativeParcelSlot(actor,*potion,potion->GetCount()-1,destination))
        throw std::runtime_error("capacity_fixture_must_block_actual_parcel_split");
    record.add_child("grants",grants);record.put("split_blocked",true);record.put("output_guid",potion->GetGUIDLow());
    record.put("output_count",potion->GetCount());record.put("unit_sell_price",proto->SellPrice);
    actor.SaveToDB();
    std::ofstream out(done);out<<Json(record);out.close();if(!out)throw std::runtime_error("capacity_fixture_evidence_failed");
}
uint32_t IsolatedCommissionCapacityProceeds(const Task& task,bool required=true) {
    auto rows=CharacterDatabase.PQuery("SELECT before_state,after_state,evidence_code FROM living_activity_operation "
        "WHERE task_id='%s' AND kind='capacity_vendor_sale' AND state='verified' ORDER BY task_revision LIMIT 9",task.id.c_str());
    if(!rows) {
        if(required)throw std::runtime_error("capacity_fixture_requires_verified_native_sale");
        return 0;
    }
    uint64_t total=0;unsigned count=0;
    do {
        boost::property_tree::ptree before,after;std::istringstream in(rows->Fetch()[0].GetCppString()),out(rows->Fetch()[1].GetCppString());
        boost::property_tree::read_json(in,before);boost::property_tree::read_json(out,after);
        const auto& q=before.get_child("native.native");const auto& actual=after.get_child("native");
        const auto copper=q.get<uint32_t>("copper");
        if(++count>8 || rows->Fetch()[2].GetCppString()!="native_capacity_sale_money_item_and_slot_observed" || !copper ||
            actual.get<uint64_t>("money")!=q.get<uint64_t>("money")+copper || actual.get<std::string>("slot_empty")!="true")
            throw std::runtime_error("capacity_fixture_invalid_native_sale_receipt");
        total+=copper;
    }while(rows->NextRow());
    if(total>UINT32_MAX)throw std::runtime_error("capacity_fixture_sale_overflow");return uint32_t(total);
}
}
