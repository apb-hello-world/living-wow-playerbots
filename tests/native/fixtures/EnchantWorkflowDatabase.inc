#include "EnchantWorkflow.h"
// Real journal/recovery SQL against native-shaped fixture tables. The fixture
// writes below are fault injection, NOT evidence of an in-game enchant cast.
static void EnchantWorkflowDatabase(Connection& db) {
    using namespace LivingActivityTest;
    EnchantWorkflowFixture f;std::string blocker;
    const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-2998479810")+(n<10?"0":"")+std::to_string(n);};
    const auto id=SqlValue(f.task.id),subject=SqlValue(f.intent.claim.id),material=SqlValue(f.material.id);
    assert(db.Scalar("SELECT MAX(version) FROM living_activity_schema")=="2");
    assert(db.Execute("ALTER TABLE item_instance ADD COLUMN enchantments TEXT NOT NULL DEFAULT ''"));
    std::string blank;
    for (unsigned i=0;i<33;++i) blank+="0 ";
    const auto applied=std::string("24 ")+blank.substr(2);
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(706,5,400)"));
    assert(db.Execute("INSERT INTO character_skills VALUES(706,333,20)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count,enchantments) VALUES(806,10938,706,1,''),(906,2398,706,1,"+SqlValue(blank)+')'));
    assert(db.Execute("INSERT INTO character_inventory(guid,item,bag,slot,item_template) VALUES(706,806,0,23,10938),(706,906,0,4,2398)"));
    auto task=f.task;task.phase=Phase::Queued;task.revision=1;
    assert(db.Write(TaskWrite(task,0,rid(0),"enchant_fixture_created")));
    task.phase=Phase::Preparing;++task.revision;
    assert(db.Write(TaskWrite(task,1,rid(1),"enchant_fixture_preparing")));
    ++task.revision;
    const std::vector<NativeResourceBalance> native={{706,806,10938,1,0,"bags"},{706,906,2398,1,0,"equipment"}};
    assert(db.Write(ResourceReservationWrite(task,2,rid(2),{{f.material,0},{f.intent.claim,0}},native)));
    auto pending=f.Pending();task.phase=Phase::Executing;task.checkpoint.step="profession_craft";++task.revision;
    assert(db.Write(OperationIntentWrite(task,3,pending.receipt.id,"profession_craft",pending.beforeState)));
    auto history=[&](const Task& owner) {
        ProfessionHistoryCursor cursor;assert(cursor.Begin(owner,db.History(owner),blocker));
        while(!cursor.Result().complete) assert(cursor.Advance(blocker));
        return cursor.Result();
    };
    const auto interrupted=history(task);assert(interrupted.interruptedCraft);
    auto saved=task;auto current=task.context;current.boot=rid(3);
    saved.context.boot.clear();saved.context.actorGeneration=saved.context.mapGeneration=0;
    ProfessionPreparation recovery;
    assert(PrepareInterruptedProfession(saved,current,interrupted,f.Held(),f.before,1100,rid(4),recovery,blocker,{},&f.intent.before));
    assert(!db.Write(recovery.plan,true));
    auto race=[&](const std::string& change,const std::string& restore) {
        assert(db.Execute(change));assert(!db.Write(recovery.plan));assert(db.Execute(restore));
        assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="4");
        assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(pending.receipt.id))=="intent");
    };
    race("UPDATE item_instance SET enchantments="+SqlValue(applied)+" WHERE guid=906",
         "UPDATE item_instance SET enchantments="+SqlValue(blank)+" WHERE guid=906");
    race("UPDATE character_inventory SET slot=23 WHERE item=906","UPDATE character_inventory SET slot=4 WHERE item=906");
    race("UPDATE item_instance SET owner_guid=707 WHERE guid=906","UPDATE item_instance SET owner_guid=706 WHERE guid=906");
    race("UPDATE item_instance SET count=0 WHERE guid=806","UPDATE item_instance SET count=1 WHERE guid=806");
    race("UPDATE characters SET money=399 WHERE guid=706","UPDATE characters SET money=400 WHERE guid=706");
    race("UPDATE character_skills SET value=21 WHERE guid=706","UPDATE character_skills SET value=20 WHERE guid=706");
    race("UPDATE living_activity_claim SET location='bags' WHERE claim_id="+subject,
         "UPDATE living_activity_claim SET location='equipment' WHERE claim_id="+subject);
    race("UPDATE living_activity_claim SET revision=2 WHERE claim_id="+subject,
         "UPDATE living_activity_claim SET revision=1 WHERE claim_id="+subject);
    // The actual copied-map failure observed a pre-effect callback gap, then
    // rolled back its native save. Preserve that observation through recovery.
    const auto missing=f.MissingCallback();task.phase=Phase::Reconciling;++task.revision;
    task.checkpoint.blocker=missing.receipt.evidence;
    assert(db.Write(OperationOutcomeWrite(task,4,missing.receipt,rid(9),missing.afterState)));
    saved=task;saved.context.boot.clear();saved.context.actorGeneration=saved.context.mapGeneration=0;
    const auto captureHistory=history(task);assert(captureHistory.interruptedCraft);
    assert(PrepareInterruptedProfession(saved,current,captureHistory,f.Held(),f.before,1100,rid(4),recovery,blocker,{},&f.intent.before));
    assert(db.Execute("UPDATE item_instance SET enchantments="+SqlValue(applied)+" WHERE guid=906"));
    assert(!db.Write(recovery.plan));
    assert(db.Execute("UPDATE item_instance SET enchantments="+SqlValue(blank)+" WHERE guid=906"));
    assert(db.Execute("UPDATE living_activity_operation SET after_state=JSON_SET(after_state,'$.effect_entered',true) WHERE operation_id="+SqlValue(pending.receipt.id)));
    assert(!db.Write(recovery.plan));
    assert(db.Execute("UPDATE living_activity_operation SET after_state="+SqlValue(missing.afterState)+" WHERE operation_id="+SqlValue(pending.receipt.id)));
    {Connection restarted;assert(restarted.Write(recovery.plan));}
    assert(db.Write(recovery.plan)); // Lost acknowledgement does not consume the reagent or enchant the target.
    task=recovery.task;
    assert(db.Scalar("SELECT JSON_UNQUOTE(JSON_EXTRACT(after_state,'$.prior_observation.recipe')) FROM living_activity_operation WHERE operation_id="+
        SqlValue(pending.receipt.id))=="7443");
    assert(db.Scalar("SELECT enchantments FROM item_instance WHERE guid=906")==blank);
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+id+" AND state='held'")=="2");
    const auto resumed=history(task);
    assert(!resumed.unresolvedOperation && resumed.attempts.size()==1 && !resumed.attempts[0].nativeEffectVerified);
    task.phase=Phase::Preparing;++task.revision;
    assert(db.Write(TaskWrite(task,task.revision-1,rid(5),"enchant_fixture_retry")));
    task.phase=Phase::Executing;task.checkpoint.step="profession_craft";++task.revision;
    const auto cast=rid(6);
    assert(db.Write(OperationIntentWrite(task,task.revision-1,cast,"profession_craft",pending.beforeState)));
    OperationResult receipt=f.row.receipt;receipt.id=cast;receipt.taskRevision=task.revision;
    receipt.nativeReference="spell:7443:operation:"+cast;
    task.phase=Phase::Verifying;++task.revision;
    const auto effect=ConsumedOperationWrite(task,task.revision-1,receipt,rid(7),f.Capture(false),f.inputs);
    assert(!db.Write(effect.journal,true));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+material)=="held");
    assert(db.Write(effect.journal));assert(db.Write(effect.journal));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+material)=="consumed");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+subject)=="held");
    auto snapshot=f.Snapshot();snapshot.revision=task.revision;snapshot.context=task.context;
    const auto completedHistory=history(task);snapshot.attempts=completedHistory.attempts;snapshot.skill=21;
    assert(snapshot.attempts.size()==2 && snapshot.attempts[1].nativeEffectVerified);
    ProfessionSettlement settlement;
    assert(PrepareProfessionSettlement(task,snapshot,f.Held(false),{{706,906,2398,1,0,"equipment"}},1200,rid(8),settlement,blocker));
    const auto exact=db.Scalar("SELECT after_state FROM living_activity_operation WHERE operation_id="+SqlValue(cast));
    assert(db.Execute("UPDATE living_activity_operation SET after_state=JSON_SET(after_state,'$.native.after.skill',20) WHERE operation_id="+SqlValue(cast)));
    assert(!db.Write(settlement.plan));
    assert(db.Execute("UPDATE living_activity_operation SET after_state="+SqlValue(exact)+" WHERE operation_id="+SqlValue(cast)));
    assert(!db.Write(settlement.plan,true));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+subject)=="held");
    {Connection restarted;assert(restarted.Write(settlement.plan));}
    assert(db.Write(settlement.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+id)=="completed");
    assert(db.Scalar("SELECT CONCAT(state,':',quantity,':',location,':',revision) FROM living_activity_claim WHERE claim_id="+subject)=="released:1:equipment:2");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+id+" AND code='profession_completed'")=="1");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=706")=="400");
    assert(db.Scalar("SELECT enchantments FROM item_instance WHERE guid=906")==blank); // Journal tests never execute native spells.
}
