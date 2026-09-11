#include "ProfessionResume.h"
static void ProfessionResumeDatabase(Connection& db) {
    using namespace LivingActivityTest;
    ResumeFixture f;const auto id=SqlValue(f.task.id);std::string blocker;
    auto initial=f.task;initial.revision=1;initial.phase=Phase::Queued;
    assert(db.Write(TaskWrite(initial,0,"ff2efbdf-f0ec-4539-b840-299847977010","fixture_created")));
    initial.revision=2;initial.phase=Phase::Preparing;
    assert(db.Write(TaskWrite(initial,1,"ff2efbdf-f0ec-4539-b840-299847977011","fixture_preparing")));
    assert(db.Write(ResourceReservationWrite(f.task,2,"ff2efbdf-f0ec-4539-b840-299847977012",
        {{f.batch.claims[0],0}},f.balances)));
    ProfessionPreparation prepared;
    assert(PrepareProfessionResumption(f.task,f.current,f.view,f.batch,f.balances,1200,f.receipt,prepared,blocker));
    const auto claim=SqlValue(f.batch.claims[0].id);
    assert(!db.Write(prepared.plan,true));
    assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="3");
    // Race: a claim changed between native inspection and journal acknowledgement.
    assert(db.Execute("UPDATE living_activity_claim SET native_reference=903 WHERE claim_id="+claim));
    assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE living_activity_claim SET native_reference=902 WHERE claim_id="+claim));
    assert(db.Execute("UPDATE living_activity_claim SET state='reconciling' WHERE claim_id="+claim));
    assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE living_activity_claim SET state='held' WHERE claim_id="+claim));
    const std::string op="ff2efbdf-f0ec-4539-b840-299847977013";
    assert(db.Execute("INSERT INTO living_activity_operation (operation_id,task_id,task_revision,kind,request_hash,state,native_reference,before_state,after_state,evidence_code,created_at_ms,updated_at_ms) VALUES ("+
        SqlValue(op)+','+id+",3,'vendor_purchase','fixture','intent','','{}','{}','',1100,1100)"));
    assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(op)));
    assert(!db.Write(prepared.plan));
    // This fixture records a rejected operation with no native effect; it is
    // journal fault injection, not a gameplay transaction or purchase receipt.
    assert(db.Execute("UPDATE living_activity_operation SET state='rejected',evidence_code='fixture_no_effect' WHERE operation_id="+SqlValue(op)));
    {Connection restarted;assert(restarted.Write(prepared.plan));}
    assert(db.Write(prepared.plan)); // Lost acknowledgement is idempotent.
    assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id="+id)=="4");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+id)=="preparing");
    assert(db.Scalar("SELECT checkpoint FROM living_activity_task WHERE task_id="+id)==f.task.checkpoint.data);
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id="+claim)=="1");
    assert(db.Scalar("SELECT native_reference FROM living_activity_claim WHERE claim_id="+claim)=="902");
    assert(db.Scalar("SELECT revision FROM living_activity_claim WHERE claim_id="+claim)=="1");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+id)=="1");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+id+" AND code='profession_preparation_revalidated'")=="1");
    ProfessionPreparation duplicate;
    assert(PrepareProfessionResumption(f.task,f.current,f.view,f.batch,f.balances,1201,f.receipt,duplicate,blocker));
    assert(!db.Write(duplicate.plan)); // Receipt cannot be reused with different contents.
}
