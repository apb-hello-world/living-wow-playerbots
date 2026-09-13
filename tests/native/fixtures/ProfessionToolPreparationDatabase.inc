#include "ProfessionToolPreparation.h"
// Actual MariaDB journal/CAS tests. Native frames below are explicitly test
// fixtures, not evidence that a realm character performed these operations.
static void ProfessionToolPreparationDatabase(Connection& db) {
    using namespace LivingActivityTest;
    ToolPreparationFixture f;std::string why;
    const auto rid=[](unsigned n){return std::string("ce5c1f26-0ac4-4e3c-a4a3-f6c035d8101")+std::to_string(n);};
    const auto id=SqlValue(f.task.id),operation=SqlValue(f.row.receipt.id);
    auto preparing=f.task;preparing.revision=1;preparing.phase=Phase::Queued;
    preparing.checkpoint.data=EncodeProfessionJob(f.workflow.intent);
    assert(db.Write(TaskWrite(preparing,0,rid(1),"fixture_created")));
    ++preparing.revision;preparing.phase=Phase::Preparing;
    assert(db.Write(TaskWrite(preparing,1,rid(2),"fixture_preparing")));
    const auto parent=preparing;++preparing.revision;preparing.checkpoint.data=f.task.checkpoint.data;
    preparing.checkpoint.step="profession_tool_prepare";
    assert(PreserveProfessionIntent(parent,preparing,why));
    assert(db.Write(TaskWrite(preparing,2,rid(3),"task_admitted")));
    std::vector<ClaimReceiptChange> originals;std::vector<NativeResourceBalance> balances;
    for(const auto& input:f.proof.inputs)originals.push_back({input.before,0});
    for(const auto& item:f.before.stacks)balances.push_back({item.actor,item.guid,item.entry,item.count,0,"bags"});
    ++preparing.revision;
    assert(db.Write(ResourceReservationWrite(preparing,3,rid(4),originals,balances)));
    ++preparing.revision;preparing.phase=Phase::Executing;
    assert(db.Write(OperationIntentWrite(preparing,4,f.row.receipt.id,"profession_craft",f.row.beforeState)));
    boost::property_tree::ptree parsed;std::istringstream stream(f.row.afterState);boost::property_tree::read_json(stream,parsed);
    std::ostringstream capture;boost::property_tree::write_json(capture,parsed.get_child("native.result"),false);
    const auto result=AcquiredOperationWrite(f.task,5,f.row.receipt,rid(5),capture.str(),f.proof.inputs,{6218,1},f.proof.gains);
    assert(db.Write(result.journal));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(87001,1,100)"));
    assert(db.Execute("INSERT INTO character_skills(guid,skill,value) VALUES(87001,333,2)"));
    for(const auto& item:f.after.stacks) {
        assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES("+
            std::to_string(item.guid)+','+std::to_string(item.entry)+",87001,"+std::to_string(item.count)+')'));
        assert(db.Execute("INSERT INTO character_inventory(guid,item,bag,slot,item_template) VALUES(87001,"+
            std::to_string(item.guid)+",0,"+std::to_string(item.slot)+','+std::to_string(item.entry)+')'));
    }
    auto read=[&]() {
        ProfessionHistoryCursor cursor;assert(cursor.Begin(f.task,db.History(f.task),why));
        while(!cursor.Result().complete)assert(cursor.Advance(why));
        f.snapshot.attempts=cursor.Result().attempts;f.snapshot.revision=f.task.revision;
        assert(f.snapshot.attempts.size()==1 && f.snapshot.attempts[0].journalDigest.size()==64);
    };
    read();ProfessionSettlement ready;
    assert(PrepareProfessionSettlement(f.task,f.snapshot,f.claims,f.balances,1100,f.receipt,ready,why));
    assert(!db.Write(ready.plan,true));
    assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="6");
    const auto after=db.Scalar("SELECT after_state FROM living_activity_operation WHERE operation_id="+operation);
    auto race=[&](const std::string& change,const std::string& restore) {
        assert(db.Execute(change));assert(!db.Write(ready.plan));assert(db.Execute(restore));
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+id+" AND state='held'")=="2");
    };
    race("UPDATE living_activity_operation SET after_state=JSON_SET(after_state,'$.native.result.created_quantity',2) WHERE operation_id="+operation,
         "UPDATE living_activity_operation SET after_state="+SqlValue(after)+" WHERE operation_id="+operation);
    race("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+operation,
         "UPDATE living_activity_operation SET state='verified' WHERE operation_id="+operation);
    const auto toolClaim=SqlValue(f.claims.claims.back().id);
    race("UPDATE living_activity_claim SET revision=2 WHERE claim_id="+toolClaim,
         "UPDATE living_activity_claim SET revision=1 WHERE claim_id="+toolClaim);
    race("UPDATE item_instance SET count=2 WHERE guid=8700104","UPDATE item_instance SET count=1 WHERE guid=8700104");
    race("UPDATE item_instance SET owner_guid=87002 WHERE guid=8700104","UPDATE item_instance SET owner_guid=87001 WHERE guid=8700104");
    race("UPDATE character_inventory SET slot=39 WHERE item=8700104","UPDATE character_inventory SET slot=26 WHERE item=8700104");
    assert(db.Write(ready.plan) && db.Write(ready.plan));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+id+" AND code='profession_tool_retained'")=="1");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+id+" AND state='held'")=="2");
    assert(db.Scalar("SELECT count FROM item_instance WHERE guid=8700104")=="1");
    f.task=ready.task;read();f.snapshot.stock={{10940,2}};
    assert(PrepareProfessionSettlement(f.task,f.snapshot,f.claims,f.balances,1200,rid(6),ready,why));
    assert(db.Write(ready.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+id)=="completed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+id+" AND state='released'")=="2");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=87001")=="100");
    assert(db.Scalar("SELECT SUM(count) FROM item_instance WHERE owner_guid=87001")=="3");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+id+" AND native_reference LIKE 'spell:7418:%'")=="0");
}
