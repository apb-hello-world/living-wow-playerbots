#include "ProfessionInterrupted.h"
static void ProfessionInterruptedDatabase(Connection& db) {
    using namespace LivingActivityTest;
    InterruptedFixture f;std::string blocker;const auto id=SqlValue(f.task.id);
    const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984797802")+std::to_string(n);};
    auto initial=f.task;initial.revision=1;initial.phase=Phase::Queued;
    assert(db.Write(TaskWrite(initial,0,rid(1),"fixture_created")));
    initial.revision=2;initial.phase=Phase::Preparing;
    assert(db.Write(TaskWrite(initial,1,rid(2),"fixture_preparing")));
    std::vector<ClaimReceiptChange> changes;std::vector<NativeResourceBalance> balances;
    for(const auto& c:f.batch.claims) {
        changes.push_back({c,0});balances.push_back({c.actor,c.itemGuid,c.itemEntry,uint32_t(c.quantity),0,"bags"});
    }
    initial.revision=3;assert(db.Write(ResourceReservationWrite(initial,2,rid(3),changes,balances)));
    const auto& op=*f.history.interruptedCraft;
    assert(db.Write(OperationIntentWrite(f.task,3,op.receipt.id,"profession_craft",op.beforeState)));
    assert(db.Execute("ALTER TABLE characters ADD COLUMN money INT UNSIGNED NOT NULL DEFAULT 0"));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(705,5,100)"));
    assert(db.Execute("CREATE TABLE character_skills(guid INT,skill INT,value INT,PRIMARY KEY(guid,skill))"));
    assert(db.Execute("INSERT INTO character_skills VALUES(705,171,1)"));
    assert(db.Execute("ALTER TABLE character_inventory ADD COLUMN item_template INT NOT NULL DEFAULT 0"));
    for(const auto& item:f.frame.stacks) {
        assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES("+
            std::to_string(item.guid)+','+std::to_string(item.entry)+",705,"+std::to_string(item.count)+')'));
        assert(db.Execute("INSERT INTO character_inventory(guid,item,bag,slot,item_template) VALUES(705,"+
            std::to_string(item.guid)+",0,"+std::to_string(item.slot)+','+std::to_string(item.entry)+')'));
    }
    ProfessionHistoryCursor cursor;assert(cursor.Begin(f.task,db.History(f.task),blocker));
    while(!cursor.Result().complete) assert(cursor.Advance(blocker));
    f.history=cursor.Result();assert(f.history.interruptedCraft);
    ProfessionPreparation prepared;
    assert(PrepareInterruptedProfession(f.task,f.current,f.history,f.batch,f.frame,1200,f.receipt,prepared,blocker));
    assert(!db.Write(prepared.plan,true)); // Atomic rollback retains intent and claims.
    assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="4");
    auto rejectRace=[&](const std::string& change,const std::string& restore) {
        assert(db.Execute(change));assert(!db.Write(prepared.plan));assert(db.Execute(restore));
        assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="4");
    };
    rejectRace("UPDATE characters SET money=99 WHERE guid=705","UPDATE characters SET money=100 WHERE guid=705");
    rejectRace("UPDATE character_skills SET value=2 WHERE guid=705","UPDATE character_skills SET value=1 WHERE guid=705");
    rejectRace("UPDATE item_instance SET count=4 WHERE guid=802","UPDATE item_instance SET count=5 WHERE guid=802");
    rejectRace("UPDATE item_instance SET owner_guid=706 WHERE guid=800","UPDATE item_instance SET owner_guid=705 WHERE guid=800");
    rejectRace("UPDATE character_inventory SET slot=26 WHERE item=800","UPDATE character_inventory SET slot=23 WHERE item=800");
    const auto operation=SqlValue(op.receipt.id),claim=SqlValue(f.batch.claims[0].id);
    rejectRace("UPDATE living_activity_claim SET revision=2 WHERE claim_id="+claim,
        "UPDATE living_activity_claim SET revision=1 WHERE claim_id="+claim);
    rejectRace("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+operation,
        "UPDATE living_activity_operation SET state='intent' WHERE operation_id="+operation);
    rejectRace("UPDATE living_activity_operation SET after_state='{\"possible\":true}' WHERE operation_id="+operation,
        "UPDATE living_activity_operation SET after_state='{}' WHERE operation_id="+operation);
    // An independently pending operation for the actor makes intent-only proof insufficient.
    const auto second=SqlValue(rid(4));
    assert(db.Execute("INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,request_hash,state,native_reference,before_state,after_state,evidence_code,created_at_ms,updated_at_ms) VALUES("+
        second+','+id+",3,'vendor_purchase','fixture','intent','','{}','{}','',1100,1100)"));
    assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE living_activity_operation SET state='rejected',evidence_code='fixture_no_effect' WHERE operation_id="+second));
    {Connection restarted;assert(restarted.Write(prepared.plan));}
    assert(db.Write(prepared.plan)); // Lost acknowledgement cannot repeat the transition.
    assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+operation)=="rejected");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=705")=="100");
    assert(db.Scalar("SELECT SUM(count) FROM item_instance WHERE owner_guid=705")=="7");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+id+" AND state='held' AND revision=1")=="3");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE transition_id="+SqlValue(f.receipt))=="1");
    ProfessionHistoryCursor recovered;assert(recovered.Begin(prepared.task,db.History(prepared.task),blocker));
    while(!recovered.Result().complete) assert(recovered.Advance(blocker));
    assert(!recovered.Result().unresolvedOperation && recovered.Result().attempts.size()==1 &&
        !recovered.Result().attempts[0].nativeEffectVerified);
    auto changed=f;changed.current.boot=rid(5);ProfessionPreparation different;
    assert(PrepareInterruptedProfession(changed.task,changed.current,changed.history,changed.batch,changed.frame,1200,
        changed.receipt,different,blocker));
    assert(!db.Write(different.plan)); // Same operation ID cannot acknowledge a changed recovery.
}
